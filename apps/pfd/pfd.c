#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <getopt.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>

#include <sys/resource.h>

#include <IOEX_carrier.h>
#include <IOEX_session.h>

#include <vlog.h>
#include <linkedhashtable.h>
#include <rc_mem.h>

#include "config.h"

static PFConfig *config;

static IOEXCarrier *carrier;

typedef struct SessionEntry {
    HashEntry he;
    IOEXSession *session;
} SessionEntry;

Hashtable *sessions;

// Client only
static IOEXSession *cli_session;
static int cli_streamid;

static void session_entry_destroy(void *p)
{
    SessionEntry *entry = (SessionEntry *)p;
    if (entry && entry->session) {
        char peer[IOEX_MAX_ID_LEN*2+8];

        IOEX_session_get_peer(entry->session, peer, sizeof(peer));
        IOEX_session_close(entry->session);
        vlogI("Session to %s closed", peer);
    }
}

static void add_session(IOEXSession *ws)
{
    assert(ws);

    SessionEntry *entry = rc_alloc(sizeof(SessionEntry), session_entry_destroy);
    if (!entry) {
        perror("Out of memory");
        exit(-1);
    }

    entry->he.data = entry;
    entry->he.key = ws;
    entry->he.keylen = sizeof(IOEXSession *);
    entry->session = ws;

    hashtable_put(sessions, &entry->he);

    deref(entry);
}

static int exist_session(IOEXSession *ws)
{
    if (sessions)
        return hashtable_exist(sessions, ws, sizeof(IOEXSession *));
    else
        return 0;
}

static void delete_session(IOEXSession *ws)
{
    if (!sessions)
        return;

    SessionEntry *entry = hashtable_remove(sessions, ws, sizeof(IOEXSession *));
    if (entry) {
        if (config->mode == MODE_CLIENT) {
            cli_session = NULL;
            cli_streamid = -1;
        }

        deref(entry);
    }
}

static void setup_portforwardings(void);

// Client only
static void peer_connection_changed(IOEXConnectionStatus status)
{
    if (status == IOEXConnectionStatus_Connected) {
        vlogI("Portforwarding server is online, setup portforwardings...");
        setup_portforwardings();
    } else {
        vlogI("Portforwarding server is being offline.");

        // Close current session if exist
        if (cli_session)
            delete_session(cli_session);

        vlogI("Portforwarding service will available when server peer online.");
    }
}

// Client only
static void carrier_ready(IOEXCarrier *w, void *context)
{
    int rc;
    char uid[IOEX_MAX_ID_LEN+1];
    char addr[IOEX_MAX_ADDRESS_LEN+1];

    vlogI("Carrier is ready!");
    vlogI("User ID: %s", IOEX_get_userid(w, uid, sizeof(uid)));
    vlogI("Address: %s", IOEX_get_address(w, addr, sizeof(addr)));

    if (config->mode == MODE_SERVER)
        return; // Server mode: do nothing.

    const char *friendid = config->serverid;

    if (!IOEX_is_friend(w, friendid)) {
        vlogI("Portforwarding server not friend yet, send friend request...");

        rc = IOEX_add_friend(w, config->server_address, "IOEX Carrier PFD/C");
        if (rc < 0) {
            vlogE("Add portforwarding server as friend failed (0x%8X)",
                  IOEX_get_error());
        } else {
            vlogI("Add portforwarding server as friend success!");
        }
    } else {
        IOEXFriendInfo fi;
        IOEX_get_friend_info(w, friendid, &fi);
        peer_connection_changed(fi.status);
    }
}

// Client only
static void friend_connection(IOEXCarrier *w, const char *friendid,
                              IOEXConnectionStatus status, void *context)
{
    if (config->mode == MODE_SERVER)
        return; // Server mode: do nothing.

    if (strcmp(friendid, config->serverid) != 0)
        return; // Ignore uninterested peer

    peer_connection_changed(status);
}

// Server and client
static void friend_request(IOEXCarrier *w, const char *userid,
            const IOEXUserInfo *info, const char *hello, void *context)
{
    int rc;
    int status = -1;

    if (config->mode == MODE_SERVER &&
            hashtable_exist(config->users, userid, strlen(userid))) {
        status = 0;
    }

    vlogI("%s friend request from %s.", status == 0 ? "Accept" : "Refuse",
            info->userid);

    if (status != 0) {
        vlogI("Skipped unathorized friend request from %s.", userid);
        return;
    } else {
        rc = IOEX_accept_friend(w, userid);
        if (rc < 0) {
            vlogE("Accept friend request failed(%08X).", IOEX_get_error());
            return;
        } else {
            vlogI("Accepted user %s to be friend.", userid);
        }
    }
}

// Client only
static void session_request_complete(IOEXSession *ws, int status,
                const char *reason, const char *sdp, size_t len, void *context)
{
    const char *state_name[] = {
        "raw",
        "initialized",
        "transport ready",
        "connecting",
        "connected",
        "deactived",
        "closed",
        "error"
    };
    IOEXStreamState state;
    int rc;

    if (status != 0) {
        vlogE("Session request complete with error(%d:%s).", status, reason);
        return;
    }

    rc = IOEX_stream_get_state(ws, cli_streamid, &state);
    while (rc == 0 && state < IOEXStreamState_transport_ready) {
        usleep(100);
        rc = IOEX_stream_get_state(ws, cli_streamid, &state);
    }

    if (rc < 0) {
        vlogE("Acquire stream state in session failed(%08X).", IOEX_get_error());
        delete_session(ws);
        return;
    }

    if (state != IOEXStreamState_transport_ready) {
        vlogE("Session stream state wrong %s.", state_name[state]);
        delete_session(ws);
        return;
    }

    rc = IOEX_session_start(ws, sdp, len);
    if (rc < 0) {
        vlogE("Start session to portforwarding server peer failed(%08X).", IOEX_get_error());
        delete_session(ws);
    } else
        vlogI("Start session to portforwarding server peer success.");
}

// Server and client
static void stream_state_changed(IOEXSession *ws, int stream,
                                 IOEXStreamState state, void *context)
{
    int rc;
    char peer[IOEX_MAX_ID_LEN*2+8];

    IOEX_session_get_peer(ws, peer, sizeof(peer));

    if (state == IOEXStreamState_failed
            || state == IOEXStreamState_closed) {
        vlogI("Session to %s closed %s.", peer,
              state == IOEXStreamState_closed ? "normally" : "on connection error");

        if (config->mode == MODE_SERVER && exist_session(ws))
            free(IOEX_session_get_userdata(ws));

        delete_session(ws);
        return;
    }

    if (config->mode == MODE_CLIENT) {
        if (state == IOEXStreamState_initialized) {
            rc = IOEX_session_request(ws, session_request_complete, NULL);
            if (rc < 0) {
                vlogE("Session request to portforwarding server peer failed(%08X)", IOEX_get_error());
                delete_session(ws);
            } else {
                vlogI("Session request to portforwarding server success.");
            }
        } else if (state == IOEXStreamState_connected) {
            HashtableIterator it;

            hashtable_iterate(config->services, &it);
            while (hashtable_iterator_has_next(&it)) {
                PFService *svc;
                hashtable_iterator_next(&it, NULL, NULL, (void **)&svc);

                int rc = IOEX_stream_open_port_forwarding(ws, stream,
                            svc->name, PortForwardingProtocol_TCP, svc->host, svc->port);
                if (rc <= 0)
                    vlogE("Open portforwarding for service %s on %s:%s failed(%08X).",
                          svc->name, svc->host, svc->port, IOEX_get_error());
                else
                    vlogI("Open portforwarding for service %s on %s:%s success.",
                          svc->name, svc->host, svc->port);

                deref(svc);
            }
        }
    } else {
        if (state == IOEXStreamState_initialized) {
            rc = IOEX_session_reply_request(ws, 0, NULL);
            if (rc < 0) {
                vlogE("Session request from %s, reply failed(%08X)", peer, IOEX_get_error());
                free(IOEX_session_get_userdata(ws));
                delete_session(ws);
                return;
            }
            vlogI("Session request from %s, accepted!", peer);
        } else if (state == IOEXStreamState_transport_ready) {
            char *sdp = (char *)IOEX_session_get_userdata(ws);

            rc = IOEX_session_start(ws, sdp, strlen(sdp));
            IOEX_session_set_userdata(ws, NULL);
            free(sdp);
            if (rc < 0) {
                vlogE("Start session to %s failed(%08X).", peer, IOEX_get_error());
                delete_session(ws);
            } else
                vlogI("Start session to %s success.", peer);
        }
    }
}

// Server and client
static void session_request_callback(IOEXCarrier *w, const char *from,
                                   const char *sdp, size_t len, void *context)
{
    IOEXSession *ws;
    PFUser *user;
    char userid[IOEX_MAX_ID_LEN + 1];
    char *p;
    int i;
    int rc;
    int options = config->options;

    IOEXStreamCallbacks stream_callbacks;

    vlogI("Session request from %s", from);

    ws = IOEX_session_new(w, from);
    if (ws == NULL) {
        vlogE("Create session failed(%08X).", IOEX_get_error());
        return;
    }

    if (config->mode == MODE_CLIENT) {
        // Client mode: just refuse the request.
        vlogI("Refuse session request from %s.", from);
        IOEX_session_reply_request(ws, -1, "Refuse");
        IOEX_session_close(ws);
        return;
    }

    // Server prepare the portforwarding services

    p = strchr(from, '@');
    if (p) {
        size_t len = p - from;
        strncpy(userid, from, len);
        userid[len] = 0;
    } else
        strcpy(userid, from);

    user = (PFUser *)hashtable_get(config->users, userid, strlen(userid));
    if (user == NULL) {
        // Not in allowed user list. Refuse session request.
        vlogI("Refuse session request from %s.", from);
        IOEX_session_reply_request(ws, -1, "Refuse");
        IOEX_session_close(ws);
        return;
    }

    for (i = 0; user->services[i] != NULL; i++) {
        PFService *svc = (PFService *)hashtable_get(config->services,
                            user->services[i], strlen(user->services[i]));

        rc = IOEX_session_add_service(ws, svc->name,
                        PortForwardingProtocol_TCP, svc->host, svc->port);
        if (rc < 0)
            vlogE("Prepare service %s for %s failed(%08X).",
                  svc->name, userid, IOEX_get_error());
        else
            vlogI("Add service %s for %s.", svc->name, userid);
    }

    p = strdup(sdp);
    IOEX_session_set_userdata(ws, p);

    add_session(ws);
    memset(&stream_callbacks, 0, sizeof(stream_callbacks));
    stream_callbacks.state_changed = stream_state_changed;
    rc = IOEX_session_add_stream(ws, IOEXStreamType_application,
                    options | IOEX_STREAM_MULTIPLEXING | IOEX_STREAM_PORT_FORWARDING,
                    &stream_callbacks, NULL);
    if (rc <= 0) {
        vlogE("Session request from %s, can not add stream(%08X)", from, IOEX_get_error());
        IOEX_session_reply_request(ws, -1, "Error");
        delete_session(ws);
        free(p);
    }
}

// Client only
static void setup_portforwardings(void)
{
    IOEXStreamCallbacks stream_callbacks;
    int options = config->options;

    // May be previous session not closed properly.
    if (cli_session != NULL)
        delete_session(cli_session);

    cli_session = IOEX_session_new(carrier, config->serverid);
    if (cli_session == NULL) {
        vlogE("Create session to portforwarding server failed(%08X).", IOEX_get_error());
        return;
    }

    vlogI("Created session to portforwarding server.");

    add_session(cli_session);

    memset(&stream_callbacks, 0, sizeof(stream_callbacks));
    stream_callbacks.state_changed = stream_state_changed;

    cli_streamid = IOEX_session_add_stream(cli_session, IOEXStreamType_application,
                options | IOEX_STREAM_MULTIPLEXING | IOEX_STREAM_PORT_FORWARDING,
                &stream_callbacks, NULL);
    if (cli_streamid <= 0) {
        vlogE("Add stream to session failed(%08X)", IOEX_get_error());
        delete_session(cli_session);
    } else {
        vlogI("Add stream %d to session success.", cli_streamid);
    }
}

static void shutdown(void)
{
    Hashtable *ss = sessions;

    sessions = NULL;
    if (ss)
        deref(ss);

    if (carrier) {
        IOEX_session_cleanup(carrier);
        IOEX_kill(carrier);
        carrier = NULL;
    }

    if (config) {
        deref(config);
        config = NULL;
    }
}

static void signal_handler(int signum)
{
    shutdown();
}

int sys_coredump_set(bool enable)
{
    const struct rlimit rlim = {
        enable ? RLIM_INFINITY : 0,
        enable ? RLIM_INFINITY : 0
    };

    return setrlimit(RLIMIT_CORE, &rlim);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpointer-to-int-cast"

static uint32_t session_hash_code(const void *key, size_t len)
{
    return (uint32_t)key;
}

#pragma GCC diagnostic pop

static int session_hash_compare(const void *key1, size_t len1,
                                const void *key2, size_t len2)
{
    if (key1 == key2)
        return 0;
    else if (key1 < key2)
        return -1;
    else
        return 1;
}

int main(int argc, char *argv[])
{
    IOEXOptions opts;
    IOEXCallbacks callbacks;
    char buffer[2048] = { 0 };
    int wait_for_attach = 0;
    int rc;
    int opt;
    int idx;
    int i;

    sys_coredump_set(true);

    signal(SIGINT, signal_handler);
    // Uncatchable: signal(SIGKILL, signal_handler);
    signal(SIGHUP, signal_handler);
    signal(SIGTERM, signal_handler);

    struct option options[] = {
        { "config",         required_argument,  NULL, 'c' },
        { "debug",          no_argument,        NULL, 1 },
        { "help",           no_argument,        NULL, 'h' },
        { NULL,             0,                  NULL, 0 }
    };

    while ((opt = getopt_long(argc, argv, "c:h?", options, &idx)) != -1) {
        switch (opt) {
        case 'c':
            strcpy(buffer, optarg);
            break;
        case 1:
            wait_for_attach = 1;
            break;

        case 'h':
        case '?':
        default:
            printf("\nUSAGE: wmpfd [-c CONFIG_FILE]\n\n");
            exit(-1);
        }
    }

    if (wait_for_attach) {
        printf("Wait for debugger attaching, process id is: %d.\n", getpid());
        printf("After debugger attached, press any key to continue......");
        getchar();
    }

    if (!*buffer) {
        realpath(argv[0], buffer);
        strcat(buffer, ".conf");
    }

    config = load_config(buffer);
    if (!config) {
        return -1;
    }

    // Initialize carrier options.
    memset(&opts, 0, sizeof(opts));

    opts.udp_enabled = config->udp_enabled;
    opts.persistent_location = config->datadir;
    opts.bootstraps_size = config->bootstraps_size;
    opts.bootstraps = (BootstrapNode *)calloc(1, sizeof(BootstrapNode) * opts.bootstraps_size);
    if (!opts.bootstraps) {
        fprintf(stderr, "out of memory.");
        deref(config);
        return -1;
    }

    for (i = 0 ; i < config->bootstraps_size; i++) {
        BootstrapNode *b = &opts.bootstraps[i];
        BootstrapNode *node = config->bootstraps[i];

        b->ipv4 = node->ipv4;
        b->ipv6 = node->ipv6;
        b->port = node->port;
        b->public_key = node->public_key;
    }

    sessions = hashtable_create(16, 1, session_hash_code, session_hash_compare);
    if (!sessions) {
        deref(config);
        return -1;
    }

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.ready = carrier_ready;
    callbacks.friend_connection = friend_connection;
    callbacks.friend_request = friend_request;

    IOEX_log_init(config->loglevel, config->logfile, NULL);

    carrier = IOEX_new(&opts, &callbacks, config);
    free(opts.bootstraps);

    if (!carrier) {
        fprintf(stderr, "Can not create Carrier instance (%08X).\n",
                IOEX_get_error());
        shutdown();
        return -1;
    }

    rc = IOEX_session_init(carrier, session_request_callback, NULL);
    if (rc < 0) {
        fprintf(stderr, "Can not initialize Carrier session extension (%08X).",
                IOEX_get_error());
        shutdown();
        return -1;
    }

    rc = IOEX_run(carrier, 500);
    if (rc < 0)
        fprintf(stderr, "Can not start Carrier.\n");

    shutdown();
    return rc;
}
