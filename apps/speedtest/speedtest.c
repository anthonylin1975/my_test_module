/*
 * 
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>

#ifdef __linux__
#define __USE_GNU
#include <pthread.h>
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#else
#include <pthread.h>
#endif

#include <IOEX_carrier.h>
#include <IOEX_session.h>

#include "config.h"

typedef enum RUNNING_MODE {
    ACTIVE_MODE,
    PASSIVE_MODE
} RUNNING_MODE;

typedef enum PROCESS_MODE {
    PARENT_MODE,
    CHILD_MODE
} PROCESS_MODE;

static bool g_connected = false;
static char g_peer_id[IOEX_MAX_ID_LEN+1] = {0};
static RUNNING_MODE g_mode = PASSIVE_MODE;
static char g_transferred_file[1024] = {0};
static char g_friend_address[IOEX_MAX_ADDRESS_LEN + 1] = {0};
static int g_fd[2] = {-1, -1};
static int g_stream_id = 0;
static size_t g_data_len = 0;

pthread_mutex_t g_screen_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;

static struct {
    IOEXSession *ws;
    int unchanged_streams;
    char remote_sdp[2048];
    size_t sdp_len;
    int bulk_mode;
    size_t packets;
    size_t bytes;
    struct timeval first_stamp;
    struct timeval last_stamp;
} session_ctx;

static void friend_accept(IOEXCarrier *, int, char **);
static void invite(IOEXCarrier *, int, char **);
static void session_init(IOEXCarrier *);
static void session_new(IOEXCarrier *, int, char **);
static void session_request(IOEXCarrier *);
static void session_reply_request(IOEXCarrier *, int, char **);
static void stream_add(IOEXCarrier *, int, char **);
static void stream_bulk_write(IOEXCarrier *, int, char **);
static void stream_bulk_receive(IOEXCarrier *, int, char **);
static void stream_get_info(IOEXCarrier *, int, char **);

static void close_pipe(void)
{
    if (g_fd[0] > 0)
        close(g_fd[0]);

    if (g_fd[1] > 0)
        close(g_fd[1]);
}

static int create_pipe(void)
{
    int ret = pipe(g_fd);

    if (ret < 0)
        goto error_exit;

    return 0;

error_exit:
    close_pipe();
    return -1;
}

static int setup_pipe(PROCESS_MODE mode)
{
    if (mode == PARENT_MODE) {
        close(g_fd[1]);
        g_fd[1] = -1;
    } else {
        int ret = 0;

        close(g_fd[0]);
        g_fd[0] = -1;

        ret = dup2(g_fd[1], STDOUT_FILENO);
        if (ret < 0) {
            close_pipe();
            return -1;
        }

        setbuf(stdout, NULL);
    }
    
    return 0;
}

static int generate_checksum(char **argv, char *buf, int size)
{
    pid_t pid = 0;
    int status = 0;

    status = create_pipe();
    if (status < 0) {
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        return -1;
    } else if (pid == 0) {
        status = setup_pipe(CHILD_MODE);
        if (status < 0)
            exit(-1);

        if (execvp(*argv, argv) < 0) {
            exit(-1);
        }
    } else {
        FILE *fp = NULL;
        char md5[64] = {0};

        while (wait(&status) != pid);
        if (status != 0)
            return -1;

        status = setup_pipe(PARENT_MODE);
        if (status < 0) 
            return -1;

        fp = fdopen(g_fd[0], "r");
        if (fp == NULL) {
            close_pipe();
            return -1;
        }

        fscanf(fp, "%32s", md5);
        fclose(fp);
        close_pipe();
        if (strlen(md5) > size - 1)
            return -1;

        strcpy(buf, md5);

        return status;
    }

    return 0;
}

static void log_print(const char *format, va_list args)
{
    pthread_mutex_lock(&g_screen_lock);
    vfprintf(stdout, format, args);
    pthread_mutex_unlock(&g_screen_lock);
}

static void output(const char *format, ...)
{
    va_list args;

    va_start(args, format);

    pthread_mutex_lock(&g_screen_lock);
    vfprintf(stdout, format, args);
    pthread_mutex_unlock(&g_screen_lock);

    va_end(args);
}

const char *presence_name[] = {
    "none",    // None;
    "away",    // Away;
    "busy",    // Busy;
};

/*
void friend(const char *addr)
{
    char *addr_arg[1] = {addr};
    char *p = NULL;

    if (addr == NULL)
        return;

    p = IOEX_get_id_by_address(addr, g_peer_id, IOEX_MAX_ID_LEN + 1);
    if (p != NULL) {
        char *arg[1] = {g_peer_id};
        friend_add(w, 1, addr_arg);
    } else {
        output("Got user ID unsuccessfully.\n");
    }
}
*/

static void friend_add(IOEXCarrier *w, int argc, char *argv[])
{
    int rc;

    if (argc != 1) {
        output("Invalid invocation.\n");
        return;
    }

    rc = IOEX_add_friend(w, argv[0], "Hello");
    if (rc == 0) {
        g_mode = ACTIVE_MODE;
        output("Request to add a new friend successfully.\n");
    }
    else
        output("Request to add a new friend unsuccessfully(0x%x).\n",
                IOEX_get_error());
}

static void friend_accept(IOEXCarrier *w, int argc, char *argv[])
{
    int rc;

    if (argc != 1) {
        output("Invalid invocation.\n");
        return;
    }

    rc = IOEX_accept_friend(w, argv[0]);
    if (rc == 0)
        output("Accept friend request successfully.\n");
    else
        output("Accept friend request unsuccessfully(0x%x).\n", IOEX_get_error());
}

static void friend_remove(IOEXCarrier *w, int argc, char *argv[])
{
    int rc;

    if (argc != 1) {
        output("Invalid invocation.\n");
        return;
    }

    rc = IOEX_remove_friend(w, argv[0]);
    if (rc == 0)
        output("Remove friend %s successfully.\n", argv[0]);
    else
        output("Remove friend %s unsuccessfully(0x%x).\n", argv[0], IOEX_get_error());
}

static int first_friends_item = 1;

static const char *connection_name[] = {
    "online",
    "offline"
};

/* This callback share by list_friends and global
 * friend list callback */
static bool friends_list_callback(IOEXCarrier *w, const IOEXFriendInfo *friend_info,
                                 void *context)
{
    static int count;

    if (first_friends_item) {
        count = 0;
        output("Friend list from carrier network:\n");
        output("  %-46s %8s %s\n", "ID", "Connection", "Label");
        output("  %-46s %8s %s\n", "----------------", "----------", "-----");
    }

    if (friend_info) {
        output("  %-46s %8s %s\n", friend_info->user_info.userid,
               connection_name[friend_info->status], friend_info->label);
        first_friends_item = 0;
        count++;
    } else {
        /* The list ended */
        output("  ----------------\n");
        output("Total %d friends.\n", count);

        first_friends_item = 1;
    }

    return true;
}

/* This callback share by list_friends and global
 * friend list callback */
static void display_friend_info(const IOEXFriendInfo *fi)
{
    output("           ID: %s\n", fi->user_info.userid);
    output("         Name: %s\n", fi->user_info.name);
    output("  Description: %s\n", fi->user_info.description);
    output("       Gender: %s\n", fi->user_info.gender);
    output("        Phone: %s\n", fi->user_info.phone);
    output("        Email: %s\n", fi->user_info.email);
    output("       Region: %s\n", fi->user_info.region);
    output("        Label: %s\n", fi->label);
    output("     Presence: %s\n", presence_name[fi->presence]);
    output("   Connection: %s\n", connection_name[fi->status]);
}

static void friend_added_callback(IOEXCarrier *w, const IOEXFriendInfo *info,
                                  void *context)
{
    strcpy(g_peer_id, info->user_info.userid);
    output("New friend added:\n");
    display_friend_info(info);
}

static void friend_removed_callback(IOEXCarrier *w, const char *friendid,
                                    void *context)
{
    output("Friend %s removed!\n", friendid);
}

static void send_message(IOEXCarrier *w, int argc, char *argv[])
{
    int rc;

    if (argc != 2) {
        output("Invalid invocation.\n");
        return;
    }

    rc = IOEX_send_friend_message(w, argv[0], argv[1], strlen(argv[1]) + 1);
    if (rc == 0)
        output("Send message successfully.\n");
    else
        output("Send message unsuccessfully(0x%x).\n", IOEX_get_error());
}

static void invite_response_callback(IOEXCarrier *w, const char *friendid,
                                     int status, const char *reason,
                                     const void *data, size_t len, void *context)
{
    output("Got invite response from %s. ", friendid);
    if (status == 0) {
        char *new_arg[1] = {NULL};
        char *add_stream_arg[2] = {NULL, NULL};

        output("message within response: %.*s\n", (int)len, (const char *)data);

        session_init(w);
        output("Session initialized successfully.\n");

        new_arg[0] = (char*)friendid;
        session_new(w, 1, new_arg);
        output("Session created successfully.\n");

        add_stream_arg[0] = (char*)"plain";
        add_stream_arg[1] = (char*)"reliable";
        stream_add(w, 2, add_stream_arg);
    } else {
        output("refused: %s\n", reason);
    }
}

static void invite(IOEXCarrier *w, int argc, char *argv[])
{
    int rc;

    if (argc != 2) {
        output("Invalid invocation.\n");
        return;
    }

    rc = IOEX_invite_friend(w, argv[0], argv[1], strlen(argv[1]),
                               invite_response_callback, NULL);
    if (rc == 0)
        output("Send invite request successfully.\n");
    else
        output("Send invite request unsuccessfully(0x%x).\n", IOEX_get_error());
}

static void reply_invite(IOEXCarrier *w, int argc, char *argv[])
{
    int rc;
    int status = 0;
    const char *reason = NULL;
    const char *msg = NULL;
    size_t msg_len = 0;

    if (argc != 3) {
        output("Invalid invocation.\n");
        return;
    }

    if (strcmp(argv[1], "confirm") == 0) {
        msg = argv[2];
        msg_len = strlen(argv[2]);
    } else if (strcmp(argv[2], "refuse") == 0) {
        status = -1; // TODO: fix to correct status code.
        reason = argv[2];
    } else {
        output("Unknown sub command: %s\n", argv[1]);
        return;
    }

    rc = IOEX_reply_friend_invite(w, argv[0], status, reason, msg, msg_len);
    if (rc == 0)
        output("Send invite reply to inviter successfully.\n");
    else
        output("Send invite reply to inviter unsuccessfully(0x%x).\n", IOEX_get_error());
}

static void session_request_callback(IOEXCarrier *w, const char *from,
            const char *sdp, size_t len, void *context)
{
    strncpy(session_ctx.remote_sdp, sdp, len);
    session_ctx.remote_sdp[len] = 0;
    session_ctx.sdp_len = len;
    char *recv_arg[1] = {NULL};
    char *reply_arg[1] = {NULL};

    output("Session request from[%s] with SDP:\n%s.\n", from, session_ctx.remote_sdp);

    recv_arg[0] = (char*)"start";
    stream_bulk_receive(w, 1, recv_arg);

    reply_arg[0] = (char*)"ok";
    session_reply_request(w, 1, reply_arg);
}

static void session_request_complete_callback(IOEXSession *ws, int status,
                const char *reason, const char *sdp, size_t len, void *context)
{
    int rc;

    output("Session complete, status: %d, reason: %s\n", status,
           reason ? reason : "null");

    if (status != 0)
        return;

    strncpy(session_ctx.remote_sdp, sdp, len);
    session_ctx.remote_sdp[len] = 0;
    session_ctx.sdp_len = len;

    rc = IOEX_session_start(session_ctx.ws, session_ctx.remote_sdp,
                               session_ctx.sdp_len);

    output("Session start %s.\n", rc == 0 ? "successfully" : "unsuccessfully");
}

static void stream_bulk_receive(IOEXCarrier *w, int argc, char *argv[])
{
    if (argc != 1) {
        output("Invalid invocation.\n");
        return;
    }

    if (strcmp(argv[0], "start") == 0) {
        session_ctx.bulk_mode = 1;
        session_ctx.bytes = 0;
        session_ctx.packets = 0;

        output("Ready to receive bulk data\n");
    } else if (strcmp(argv[0], "end") == 0) {
        struct timeval start = session_ctx.first_stamp;
        struct timeval end = session_ctx.last_stamp;

        int duration = (int)((end.tv_sec - start.tv_sec) * 1000000 +
                             (end.tv_usec - start.tv_usec)) / 1000;
        duration = (duration == 0)  ? 1 : duration;
        float speed = ((session_ctx.bytes / duration) * 1000) / 1024;

        output("\nFinish! Total %" PRIu64 " bytes in %d.%d seconds. %.2f KB/s\n",
               session_ctx.bytes,
               (int)(duration / 1000), (int)(duration % 1000), speed);

        session_ctx.bulk_mode = 0;
        session_ctx.bytes = 0;
        session_ctx.packets = 0;
    } else  {
        output("Invalid invocation.\n");
        return;
    }
}

static void stream_on_data(IOEXSession *ws, int stream, const void *data,
                           size_t len, void *context)
{
    if (session_ctx.bulk_mode) {
        static int first_time = 0;
        static int trans_fd = 0;
        int ret = 0;

        if (first_time == 0) {
            first_time = 1;
            trans_fd = open(g_transferred_file, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
            if (trans_fd < 0) {
                output("stream_on_data open unsuccessfully.");
                return;
            }
        }

        if (session_ctx.packets)
            gettimeofday(&session_ctx.last_stamp, NULL);
        else
            gettimeofday(&session_ctx.first_stamp, NULL);

        session_ctx.bytes += len;
        session_ctx.packets++;
        ret = write(trans_fd, data, len);
        if (ret < len) {
            close(trans_fd);
            output("stream_on_data write unsuccessfully.");
            return;
        }

        if (session_ctx.bytes == g_data_len) {
            char *arg[2] = {NULL, NULL};
            char *md5sum_arg[4] = {(char*)"md5sum", (char*)"-b", g_transferred_file, NULL};
            char buf[64] = {0};
            int ret = 0;

            close(trans_fd);
            ret = generate_checksum(md5sum_arg, buf, sizeof(buf));
            if (ret < 0)
                output("passive end generated checksum unsuccessfully.");

            arg[0] = g_peer_id;
            arg[1] = buf;
            send_message((IOEXCarrier*)context, 2, arg);
            exit(0);
        }
    } else {
        output("Stream [%d] received data [%.*s]\n", stream, (int)len, (char*)data);
    }
}

static void stream_on_state_changed(IOEXSession *ws, int stream,
        IOEXStreamState state, void *context)
{
    const char *state_name[] = {
        "raw",
        "initialized",
        "transport_ready",
        "connecting",
        "connected",
        "deactivated",
        "closed",
        "failed"
    };

    output("Stream [%d] state changed to: %s\n", stream, state_name[state]);

    if (state == IOEXStreamState_transport_ready)
        --session_ctx.unchanged_streams;

    if (state == IOEXStreamState_initialized) {
        if (g_mode == ACTIVE_MODE) {
        session_request((IOEXCarrier*)context);
    } else if (g_mode == PASSIVE_MODE) {
        char *reply_arg[3] = {NULL, NULL, NULL};
        
        reply_arg[0] = g_peer_id;
        reply_arg[1] = (char*)"confirm";
        reply_arg[2] = (char*)"ok";
        reply_invite((IOEXCarrier*)context, 3, reply_arg);
    } else {
        output("Exception occurred");
    }
    } else if (state == IOEXStreamState_connected) {
        if (g_mode == ACTIVE_MODE) {
            char *info_arg[1] = {NULL};
            char stream_id[32] = {0};
            char msg_len[32] = {0};
            char *msg_arg[2] = {NULL, NULL};
            char *write_arg[3] = {NULL, NULL, NULL};
            char buf[3][32] = {{0}, {0}, {0}};
            int pkt_size = 1000;
            int pkt_count = 1000;
            struct stat st = {0};
            int ret;

            ret = stat(g_transferred_file, &st);
            if (ret < 0)
                return;

            g_data_len = st.st_size;
            if (g_data_len == 0)
                return;

            sprintf(stream_id, "%d", g_stream_id);
            info_arg[0] = stream_id;
            stream_get_info((IOEXCarrier*)context, 1, info_arg);

            msg_arg[0] = g_peer_id;
            sprintf(msg_len, "%zu", g_data_len);
            msg_arg[1] = msg_len;
            send_message((IOEXCarrier*)context, 2, msg_arg);

            sprintf(buf[0], "%d", g_stream_id);
            sprintf(buf[1], "%d", pkt_size);
            sprintf(buf[2], "%d", pkt_count);
            write_arg[0] = buf[0];
            write_arg[1] = buf[1];
            write_arg[2] = buf[2];
            stream_bulk_write((IOEXCarrier*)context, 3, write_arg);
        } else {

        }
    } else {
        
    }
}

static void session_init(IOEXCarrier *w)
{
    int rc;

    rc = IOEX_session_init(w, session_request_callback, w);
    if (rc < 0) {
        output("Session initialized unsuccessfully.\n");
    }
    else {
        output("Session initialized successfully.\n");
    }
}

static void session_new(IOEXCarrier *w, int argc, char *argv[])
{
    if (argc != 1) {
        output("Invalid invocation.\n");
        return;
    }

    session_ctx.ws = IOEX_session_new(w, argv[0]);
    if (!session_ctx.ws) {
        output("Create session unsuccessfully.\n");
    } else {
        output("Create session successfully.\n");
    }
    session_ctx.unchanged_streams = 0;
}

static void stream_add(IOEXCarrier *w, int argc, char *argv[])
{
    int rc;
    int options = 0;

    IOEXStreamCallbacks callbacks;

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.state_changed = stream_on_state_changed;
    callbacks.stream_data = stream_on_data;

    if (argc < 1) {
        output("Invalid invocation.\n");
        return;
    } else if (argc > 1) {
        int i;

        for (i = 0; i < argc; i++) {
            if (strcmp(argv[i], "reliable") == 0) {
                options |= IOEX_STREAM_RELIABLE;
            } else if (strcmp(argv[i], "plain") == 0) {
                options |= IOEX_STREAM_PLAIN;
            } else {
                output("Invalid invocation.\n");
                return;
            }
        }
    }

    rc = IOEX_session_add_stream(session_ctx.ws, IOEXStreamType_text,
                                options, &callbacks, w);
    if (rc < 0) {
        output("Add stream unsuccessfully.\n");
    }
    else {
        session_ctx.unchanged_streams++;
        output("Add stream successfully and stream id %d.\n", rc);
        g_stream_id = rc;
    }
}

static void session_request(IOEXCarrier *w)
{
    int rc;

    rc = IOEX_session_request(session_ctx.ws,
                             session_request_complete_callback, w);
    if (rc < 0) {
        output("session request unsuccessfully.\n");
    }
    else {
        output("session request successfully.\n");
    }
}

static void session_reply_request(IOEXCarrier *w, int argc, char *argv[])
{
    int rc;

    if ((argc != 1) && (argc != 2)) {
        output("Invalid invocation.\n");
        return;
    }

    if ((strcmp(argv[0], "ok") == 0) && (argc == 1)) {
        rc = IOEX_session_reply_request(session_ctx.ws, 0, NULL);
        if (rc < 0) {
            output("response invite unsuccessfully.\n");
        }
        else {
            output("response invite successfully.\n");

            while (session_ctx.unchanged_streams > 0)
                usleep(200);

            rc = IOEX_session_start(session_ctx.ws, session_ctx.remote_sdp,
                                       session_ctx.sdp_len);

            output("Session start %s.\n", rc == 0 ? "successfully" : "unsuccessfully");
        }
    }
    else if ((strcmp(argv[0], "refuse") == 0) && (argc == 2)) {
        rc = IOEX_session_reply_request(session_ctx.ws, 1, argv[2]);
        if (rc < 0) {
            output("response invite unsuccessfully.\n");
        }
        else {
            output("response invite successfully.\n");
        }
    }
    else {
        output("Invalid response.\n");
        return;
    }
}

struct bulk_write_args {
    int stream;
    //int packet_size;
    //int packet_count;
};
struct bulk_write_args args;

static void *bulk_write_thread(void *arg)
{
    ssize_t rc;
    char packet[1024];
    struct bulk_write_args *args = (struct bulk_write_args *)arg;
    struct timeval start, end;
    int duration;
    float speed;
    int trans_fd;

    trans_fd = open(g_transferred_file, O_RDONLY);
    if (trans_fd < 0)
        return NULL;

    output("Begin sending data");

    gettimeofday(&start, NULL);

    while ((rc = read(trans_fd, packet, sizeof(packet))) > 0) {
        rc = IOEX_stream_write(session_ctx.ws, args->stream,
                                  packet, rc);
        if (rc == 0) {
            usleep(100);
            continue;
        } else if (rc < 0) {
            if (IOEX_get_error() == IOEX_GENERAL_ERROR(IOEXERR_BUSY)) {
                usleep(100);
                continue;
            } else {
                output("\nWrite data unsuccessfully.\n");
                output("error no:%x", IOEX_get_error());
                close(trans_fd);
                return NULL;
            }
        }
    }

    gettimeofday(&end, NULL);

    close(trans_fd);

    duration = (int)((end.tv_sec - start.tv_sec) * 1000000 +
                     (end.tv_usec - start.tv_usec)) / 1000;
    duration = (duration == 0) ? 1 : duration;
    speed = ((g_data_len / duration) * 1000) / 1024;

    output("\nFinish! Total %zu bytes in %d.%d seconds. %.2f KB/s\n",
           g_data_len,
           (int)(duration / 1000), (int)(duration % 1000), speed);

    return NULL;
}

static void stream_bulk_write(IOEXCarrier *w, int argc, char *argv[])
{
    pthread_attr_t attr;
    pthread_t th;

    if (argc != 3) {
        output("Invalid invocation.\n");
        return;
    }

    args.stream = atoi(argv[0]);

    if (args.stream <= 0 /*|| args.packet_size <= 0 || args.packet_count <= 0*/) {
        output("Invalid invocation.\n");
        return;
    }

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&th, &attr, bulk_write_thread, &args);
    pthread_join(th, NULL);
    pthread_attr_destroy(&attr);
}

static void stream_get_info(IOEXCarrier *w, int argc, char *argv[])
{
    int rc;
    IOEXTransportInfo info;

    const char *topology_name[] = {
        "LAN",
        "P2P",
        "RELAYED"
    };

    const char *addr_type[] = {
        "HOST   ",
        "SREFLEX",
        "PREFLEX",
        "RELAY  "
    };

    if (argc != 1) {
        output("Invalid invocation.\n");
        return;
    }

    rc = IOEX_stream_get_transport_info(session_ctx.ws, atoi(argv[0]), &info);
    if (rc < 0) {
        output("get remote addr unsuccessfully.\n");
        return;
    }

    output("Stream transport information:\n");
    output("    Network: %s\n", topology_name[info.topology]);

    output("      Local: %s %s:%d", addr_type[info.local.type], info.local.addr, info.local.port);
    if (*info.local.related_addr)
        output(" related %s:%d\n", info.local.related_addr, info.local.related_port);
    else
        output("\n");

    output("     Remote: %s %s:%d", addr_type[info.remote.type], info.remote.addr, info.remote.port);
    if (*info.remote.related_addr)
        output(" related %s:%d\n", info.remote.related_addr, info.remote.related_port);
    else
        output("\n");
}

static void idle_callback(IOEXCarrier *w, void *context)
{
    static int first_time = 1;

    if (g_connected && (IOEX_is_ready(w)) && (first_time == 1)) {
        if (strlen(g_friend_address) > 0) {
            char *addr_arg[1] = {(char*)g_friend_address};
            char *p = NULL;

            p = IOEX_get_id_by_address(g_friend_address, g_peer_id, IOEX_MAX_ID_LEN + 1);
            if (p != NULL) {
                char *arg[1] = {g_peer_id};
                friend_remove(w, 1, arg);
                friend_add(w, 1, addr_arg);
                first_time = 0;
            } else {
                output("Got user ID unsuccessfully.\n");
            }
        }
    }
}

static void connection_callback(IOEXCarrier *w, IOEXConnectionStatus status,
                                void *context)
{
    switch (status) {
    case IOEXConnectionStatus_Connected:
        g_connected = true;
        output("Connected to carrier network.\n");
        break;

    case IOEXConnectionStatus_Disconnected:
        g_connected = false;
        output("Disconnected from carrier network.\n");
        break;

    default:
        output("Error!!! Got unknown connection status %d.\n", status);
    }
}

static void friend_info_callback(IOEXCarrier *w, const char *friendid,
                                 const IOEXFriendInfo *info, void *context)
{
    output("Friend information changed:\n");
    display_friend_info(info);
}

static void friend_connection_callback(IOEXCarrier *w, const char *friendid,
                                       IOEXConnectionStatus status, void *context)
{
    switch (status) {
    case IOEXConnectionStatus_Connected:
        output("Friend[%s] connection changed to be online\n", friendid);

        if ((strcmp(g_peer_id, friendid) == 0) && (g_mode == ACTIVE_MODE)) {
            char *arg[2] = {NULL, NULL};

            arg[0] = g_peer_id;
            arg[1] = (char*)"hello";
            invite(w, 2, arg);
        }
        break;

    case IOEXConnectionStatus_Disconnected:
        output("Friend[%s] connection changed to be offline.\n", friendid);
        break;

    default:
        output("Error!!! Got unknown connection status %d.\n", status);
    }
}

static void friend_presence_callback(IOEXCarrier *w, const char *friendid,
                                     IOEXPresenceStatus status,
                                     void *context)
{
    if (status >= IOEXPresenceStatus_None &&
        status <= IOEXPresenceStatus_Busy) {
        output("Friend[%s] change presence to %s\n", friendid, presence_name[status]);
    } else {
        output("Error!!! Got unknown presence status %d.\n", status);
    }
}

static void friend_request_callback(IOEXCarrier *w, const char *userid,
                                    const IOEXUserInfo *info, const char *hello,
                                    void *context)
{
    char *arg[1] = {NULL};

    output("Friend request from user[%s] with HELLO: %s.\n",
           *info->name ? info->name : userid, hello);
    
    arg[0] = (char*)userid;
    friend_accept(w, 1, arg);
}

static void message_callback(IOEXCarrier *w, const char *from,
                             const void *msg, size_t len, void *context)
{
    output("Message from friend[%s]: %.*s\n", from, (int)len, msg);

    if (g_mode == PASSIVE_MODE) {
        g_data_len = atoi((const char *)msg);
        output("Got data length: %zu.\n", g_data_len);
    } else {
        char *arg[] = {(char*)"md5sum", (char*)"-b", g_transferred_file, NULL};
        char buf[64] = {0};
        int ret = 0;

        output("Got md5 checksum from peer:%s\n", msg);
        
        ret = generate_checksum(arg, buf, sizeof(buf));
        if (ret < 0)
            output("active end generated checksum unsuccessfully.");
        else
            output("My md5 checksum:%s\n", buf);

        if (strcasecmp(buf, (const char *)msg) == 0) {
            output("Sent data successfully!\n");
        } else {
            output("Sent data unsuccessfully!\n");
        }
        exit(0);
    }
}

static void invite_request_callback(IOEXCarrier *w, const char *from,
                                    const void *data, size_t len, void *context)
{
    char *new_arg[1] = {NULL};
    char *add_stream_arg[2] = {NULL, NULL};

    output("Invite request from[%s] with data: %.*s\n", from, (int)len, (const char *)data);

    strcpy(g_peer_id, from);
    session_init(w);

    new_arg[0] = (char*)from;
    session_new(w, 1, new_arg);

    add_stream_arg[0] = (char*)"plain";
    add_stream_arg[1] = (char*)"reliable";
    stream_add(w, 2, add_stream_arg);
}

static void ready_callback(IOEXCarrier *w, void *context)
{
    output("ready_callback invoked\n");
}

static void usage(void)
{
    printf("Speedtest, an interactive console application.\n");
    printf("Usage: speedtest [OPTION]...\n");
    printf("\n");
    printf("First run options:\n");
    printf("  -a, --frdaddr=ADDRESS(optional) Set friend address.\n");
    printf("  -c, --config=CONFIG_FILE        Set config file path.\n");
    printf("  -f, --file=TRANS_FILE           Set transferred file.\n");
    printf("\n");
    printf("Debugging options:\n");
    printf("      --debug                     Wait for debugger attach after start.\n");
    printf("\n");
}

#include <sys/resource.h>

int sys_coredump_set(bool enable)
{
    const struct rlimit rlim = {
        enable ? RLIM_INFINITY : 0,
        enable ? RLIM_INFINITY : 0
    };

    return setrlimit(RLIMIT_CORE, &rlim);
}

void signal_handler(int signum)
{
    exit(-1);
}

int main(int argc, char *argv[])
{
    SpeedtestConfig *cfg;
    char buffer[2048];
    IOEXCarrier *w;
    IOEXOptions opts;
    int wait_for_attach = 0;
    char buf[IOEX_MAX_ADDRESS_LEN+1];
    IOEXCallbacks callbacks;
    int rc;
    int i;

    int opt;
    int idx;
    struct option options[] = {
        { "frdaddr",        optional_argument,  NULL, 'a' },
        { "config",         required_argument,  NULL, 'c' },
        { "file",           required_argument,  NULL, 'f' },
        { "debug",          no_argument,        NULL, 2 },
        { "help",           no_argument,        NULL, 'h' },
        { NULL,             0,                  NULL, 0 }
    };

    signal(SIGINT, signal_handler);
    signal(SIGHUP, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGSEGV, signal_handler);

    sys_coredump_set(true);

    memset(&opts, 0, sizeof(opts));

    while ((opt = getopt_long(argc, argv, "a:c:f:h?",
            options, &idx)) != -1) {
        switch (opt) {
        case 'a':
            if (optarg != NULL)
                strncpy(g_friend_address, optarg, IOEX_MAX_ADDRESS_LEN);
            break;
        case 'c':
            strcpy(buffer, optarg);
            break;
        
        case 'f':
            strcpy(g_transferred_file, optarg);
            break;

        case 2:
            wait_for_attach = 1;
            break;

        case 'h':
        case '?':
        default:
            usage();
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

    if (!*g_transferred_file) {
        fprintf(stdout, "Transferred file name was no specified!\n");
        return -1;
    }

    cfg = load_config(buffer);
    if (!cfg) {
        fprintf(stderr, "loading configure failed !\n");
        return -1;
    }

    IOEX_log_init(cfg->loglevel, cfg->logfile, log_print);

    opts.udp_enabled = cfg->udp_enabled;
    opts.persistent_location = cfg->datadir;
    opts.bootstraps_size = cfg->bootstraps_size;
    opts.bootstraps = (BootstrapNode *)calloc(1, sizeof(BootstrapNode) * opts.bootstraps_size);
    if (!opts.bootstraps) {
        fprintf(stderr, "out of memory.");
        deref(cfg);
        return -1;
    }

    for (i = 0 ; i < cfg->bootstraps_size; i++) {
        BootstrapNode *b = &opts.bootstraps[i];
        BootstrapNode *node = cfg->bootstraps[i];

        b->ipv4 = node->ipv4;
        b->ipv6 = node->ipv6;
        b->port = node->port;
        b->public_key = node->public_key;
    }

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.idle = idle_callback;
    callbacks.connection_status = connection_callback;
    callbacks.friend_list = friends_list_callback;
    callbacks.friend_connection = friend_connection_callback;
    callbacks.friend_info = friend_info_callback;
    callbacks.friend_presence = friend_presence_callback;
    callbacks.friend_request = friend_request_callback;
    callbacks.friend_added = friend_added_callback;
    callbacks.friend_removed = friend_removed_callback;
    callbacks.friend_message = message_callback;
    callbacks.friend_invite = invite_request_callback;
    callbacks.ready = ready_callback;

    w = IOEX_new(&opts, &callbacks, NULL);
    deref(cfg);
    free(opts.bootstraps);

    if (!w) {
        output("Error create carrier instance: 0x%x\n", IOEX_get_error());
        output("Press any key to quit...");
        goto quit;
    }

    output("Carrier node identities:\n");
    output("   Node ID: %s\n", IOEX_get_nodeid(w, buf, sizeof(buf)));
    output("   User ID: %s\n", IOEX_get_userid(w, buf, sizeof(buf)));
    output("   Address: %s\n\n", IOEX_get_address(w, buf, sizeof(buf)));
    output("\n");

    rc = IOEX_run(w, 10);
    if (rc != 0) {
        output("Error start carrier loop: 0x%x\n", IOEX_get_error());
        output("Press any key to quit...");
        IOEX_kill(w);
        goto quit;
    }

quit:
    return 0;
}
