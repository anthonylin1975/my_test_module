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
#include <assert.h>
#include <unistd.h>
#include <time.h>
#include <limits.h>
#include <pthread.h>

#include <CUnit/Basic.h>
#include "IOEX_carrier.h"
#include "IOEX_session.h"
#include "cond.h"
#include "tests.h"
#include "test_helper.h"
#include "test_assert.h"

static void* carrier_run_entry(void *arg)
{
    IOEXCarrier *w = ((CarrierContext*)arg)->carrier;
    int rc;

    rc = IOEX_run(w, 10);
    if (rc != 0) {
        printf("Error: start carrier loop error %d.\n", IOEX_get_error());
        IOEX_kill(w);
    }

    return NULL;
}

static void carrier_idle_cb(IOEXCarrier *w, void *context)
{
    IOEXCallbacks *cbs = ((CarrierContext*)context)->cbs;

    if (cbs && cbs->idle)
        cbs->idle(w, context);
}

static
void carrier_connection_status_cb(IOEXCarrier *w, IOEXConnectionStatus status,
                                  void *context)
{
    IOEXCallbacks *cbs = ((CarrierContext*)context)->cbs;

    switch (status) {
        case IOEXConnectionStatus_Connected:
            test_log_info("Connected to carrier network.\n");
            break;

        case IOEXConnectionStatus_Disconnected:
            test_log_info("Disconnect from carrier network.\n");
            break;

        default:
            test_log_error("Error!!! Got unknown connection status %d.\n", status);
    }

    if (cbs && cbs->connection_status)
        cbs->connection_status(w, status, context);
}

static void carrier_ready_cb(IOEXCarrier *w, void *context)
{
    IOEXCallbacks *cbs = ((CarrierContext*)context)->cbs;

    test_log_info("Carrier is ready.\n");

    if (cbs && cbs->ready)
        cbs->ready(w, context);
}

static void carrier_self_info_cb(IOEXCarrier *w, const IOEXUserInfo *info,
                                 void *context)
{
    IOEXCallbacks *cbs = ((CarrierContext*)context)->cbs;

    if (cbs && cbs->self_info)
        cbs->self_info(w, info, context);
}

static bool carrier_friend_list_cb(IOEXCarrier *w, const IOEXFriendInfo* info,
                                   void* context)
{
    IOEXCallbacks *cbs = ((CarrierContext*)context)->cbs;

    if (cbs && cbs->friend_list)
        return cbs->friend_list(w, info, context);

    return true;
}

static void carrier_friend_info_cb(IOEXCarrier *w, const char *friendid,
                                   const IOEXFriendInfo *info, void *context)
{
    IOEXCallbacks *cbs = ((CarrierContext*)context)->cbs;

    if (cbs && cbs->friend_info)
        cbs->friend_info(w, friendid, info, context);
}

static void carrier_friend_connection_cb(IOEXCarrier *w, const char *friendid,
                                IOEXConnectionStatus status, void *context)
{
    IOEXCallbacks *cbs = ((CarrierContext *)context)->cbs;

    if (cbs && cbs->friend_connection)
        cbs->friend_connection(w, friendid, status, context);
}

static void carrier_friend_presence_cb(IOEXCarrier *w, const char *friendid,
                                IOEXPresenceStatus presence, void *context)
{
    IOEXCallbacks *cbs = ((CarrierContext*)context)->cbs;

    if (cbs && cbs->friend_presence)
        cbs->friend_presence(w, friendid, presence, context);
}

static void carrier_friend_request_cb(IOEXCarrier *w, const char *userid,
                                      const IOEXUserInfo *info,
                                      const char *hello, void *context)
{
    IOEXCallbacks *cbs = ((CarrierContext*)context)->cbs;

    if (cbs && cbs->friend_request)
        cbs->friend_request(w, userid, info, hello, context);
}

static void carrier_friend_added_cb(IOEXCarrier *w, const IOEXFriendInfo *info,
                                    void *context)
{
    IOEXCallbacks *cbs = ((CarrierContext*)context)->cbs;

    if (cbs && cbs->friend_added)
        cbs->friend_added(w, info, context);
}

static void carrier_friend_removed_cb(IOEXCarrier *w, const char *friendid,
                                      void *context)
{
    IOEXCallbacks *cbs = ((CarrierContext*)context)->cbs;

    if (cbs && cbs->friend_removed)
        cbs->friend_removed(w, friendid, context);
}

static void carrier_friend_message_cb(IOEXCarrier *w, const char *from,
                                      const void *msg, size_t len, void *context)
{
    IOEXCallbacks *cbs = ((CarrierContext*)context)->cbs;

    if (cbs && cbs->friend_message)
        cbs->friend_message(w, from, msg, len, context);
}

static void carrier_friend_invite_cb(IOEXCarrier *w, const char *from,
                                     const void *data, size_t len,
                                     void *context)
{
    IOEXCallbacks *cbs = ((CarrierContext*)context)->cbs;

    if (cbs && cbs->friend_invite)
        cbs->friend_invite(w, from, data, len, context);
}

static IOEXCallbacks callbacks = {
    .idle            = carrier_idle_cb,
    .connection_status = carrier_connection_status_cb,
    .ready           = carrier_ready_cb,
    .self_info       = carrier_self_info_cb,
    .friend_list     = carrier_friend_list_cb,
    .friend_info     = carrier_friend_info_cb,
    .friend_connection = carrier_friend_connection_cb,
    .friend_presence = carrier_friend_presence_cb,
    .friend_request  = carrier_friend_request_cb,
    .friend_added    = carrier_friend_added_cb,
    .friend_removed  = carrier_friend_removed_cb,
    .friend_message  = carrier_friend_message_cb,
    .friend_invite   = carrier_friend_invite_cb
};

int test_suite_init_ext(TestContext *context, bool udp_disabled)
{
    CarrierContext *wctxt = context->carrier;
    char datadir[PATH_MAX];
    IOEXOptions opts = {
        .udp_enabled = !udp_disabled,
        .persistent_location = datadir,
        .bootstraps_size = global_config.bootstraps_size,
        .bootstraps = NULL
    };
    int i = 0;

    sprintf(datadir, "%s/tests", global_config.data_location);

    opts.bootstraps = (BootstrapNode *)calloc(1, sizeof(BootstrapNode) * opts.bootstraps_size);
    if (!opts.bootstraps) {
        test_log_error("Error: out of memory.");
        return -1;
    }

    for (i = 0 ; i < opts.bootstraps_size; i++) {
        BootstrapNode *b = &opts.bootstraps[i];
        BootstrapNode *node = global_config.bootstraps[i];

        b->ipv4 = node->ipv4;
        b->ipv6 = node->ipv6;
        b->port = node->port;
        b->public_key = node->public_key;
    }

    wctxt->carrier = IOEX_new(&opts, &callbacks, wctxt);
    free(opts.bootstraps);

    if (!wctxt->carrier) {
        test_log_error("Error: carrier new error (0x%x)\n", IOEX_get_error());
        return -1;
    }

    cond_reset(wctxt->cond);
    cond_reset(wctxt->ready_cond);

    pthread_create(&wctxt->thread, 0, &carrier_run_entry, wctxt);
    cond_wait(wctxt->ready_cond);

    return 0;
}

int test_suite_init(TestContext *context)
{
	return test_suite_init_ext(context, false);
}

int test_suite_cleanup(TestContext *context)
{
    CarrierContext *wctxt = context->carrier;

    IOEX_kill(wctxt->carrier);
    pthread_join(wctxt->thread, 0);
    cond_deinit(wctxt->cond);

    return 0;
}

int add_friend_anyway(TestContext *context, const char *userid,
                      const char *address)
{
    CarrierContext *wctxt = context->carrier;
    int rc;

    if (IOEX_is_friend(wctxt->carrier, userid)) {
        while(!wctxt->robot_online)
            usleep(500);
        return 0;
    }

    rc = IOEX_add_friend(wctxt->carrier, address, "auto-reply");
    if (rc < 0) {
        test_log_error("Error: attempt to add friend error.\n");
        return rc;
    }

    // wait for friend_added callback invoked.
    cond_wait(wctxt->cond);

    // wait for friend_connection (online) callback invoked.
    cond_wait(wctxt->cond);

    return 0;
}

int remove_friend_anyway(TestContext *context, const char *userid)
{
    CarrierContext *wctxt = context->carrier;
    int rc;

    if (!IOEX_is_friend(wctxt->carrier, userid)) {
        while (wctxt->robot_online)
            usleep(500);
        return 0;
    } else {
        while (!wctxt->robot_online)
            usleep(500);
    }

    rc = IOEX_remove_friend(wctxt->carrier, userid);
    if (rc < 0) {
        test_log_error("Error: remove friend error (%x)\n", IOEX_get_error());
        return rc;
    }

    // wait for friend_connection (online -> offline) callback invoked.
    cond_wait(wctxt->cond);

    // wait for friend_removed callback invoked.
    cond_wait(wctxt->cond);

    return 0;
}

int robot_sinit(void)
{
    return robot_ctrl("sinit\n");
}

void robot_sfree(void)
{
    robot_ctrl("sfree\n");
}

const char *stream_state_name(IOEXStreamState state)
{
    const char *state_name[] = {
        "raw",
        "initialized",
        "transport ready",
        "connecting",
        "connected",
        "deactivated",
        "closed",
        "failed"
    };

    return state_name[state];
}

void test_stream_scheme(IOEXStreamType stream_type, int stream_options,
                        TestContext *context, int (*do_work_cb)(TestContext *))
{
    CarrierContext *wctxt = context->carrier;
    SessionContext *sctxt = context->session;
    StreamContext *stream_ctxt = context->stream;

    int rc;
    char cmd[32];
    char result[32];

    context->context_reset(context);

    rc = add_friend_anyway(context, robotid, robotaddr);
    CU_ASSERT_EQUAL_FATAL(rc, 0);
    CU_ASSERT_TRUE_FATAL(IOEX_is_friend(wctxt->carrier, robotid));

    rc = IOEX_session_init(wctxt->carrier, NULL, NULL);
    CU_ASSERT_EQUAL_FATAL(rc, 0);

    rc = robot_sinit();
    TEST_ASSERT_TRUE(rc > 0);

    rc = wait_robot_ack("%32s %32s", cmd, result);
    TEST_ASSERT_TRUE(rc == 2);
    TEST_ASSERT_TRUE(strcmp(cmd, "sinit") == 0);
    TEST_ASSERT_TRUE(strcmp(result, "success") == 0);

    sctxt->session = IOEX_session_new(wctxt->carrier, robotid);
    TEST_ASSERT_TRUE(sctxt->session != NULL);

    stream_ctxt->stream_id = IOEX_session_add_stream(sctxt->session,
                                        stream_type, stream_options,
                                        stream_ctxt->cbs, stream_ctxt);
    TEST_ASSERT_TRUE(stream_ctxt->stream_id > 0);

    cond_wait(stream_ctxt->cond);
    TEST_ASSERT_TRUE(stream_ctxt->state == IOEXStreamState_initialized);
    TEST_ASSERT_TRUE(stream_ctxt->state_bits & (1 << IOEXStreamState_initialized));

    rc = IOEX_session_request(sctxt->session, sctxt->request_complete_cb, sctxt);
    TEST_ASSERT_TRUE(rc == 0);

    cond_wait(stream_ctxt->cond);
    TEST_ASSERT_TRUE(stream_ctxt->state == IOEXStreamState_transport_ready);
    TEST_ASSERT_TRUE(stream_ctxt->state_bits & (1 << IOEXStreamState_transport_ready));

    rc = wait_robot_ack("%32s %32s", cmd, result);
    TEST_ASSERT_TRUE(rc == 2);
    TEST_ASSERT_TRUE(strcmp(cmd, "srequest") == 0);
    TEST_ASSERT_TRUE(strcmp(result, "received") == 0);

    rc = robot_ctrl("sreply confirm %d %d\n", stream_type, stream_options);
    TEST_ASSERT_TRUE(rc > 0);

    cond_wait(sctxt->request_complete_cond);
    TEST_ASSERT_TRUE(sctxt->request_received == 0);
    TEST_ASSERT_TRUE(sctxt->request_complete_status == 0);

    rc = wait_robot_ack("%32s %32s", cmd, result);
    TEST_ASSERT_TRUE(rc == 2);
    TEST_ASSERT_TRUE(strcmp(cmd, "sreply") == 0);
    TEST_ASSERT_TRUE(strcmp(result, "success") == 0);

    cond_wait(stream_ctxt->cond);

    if (stream_ctxt->state != IOEXStreamState_connecting &&
        stream_ctxt->state != IOEXStreamState_connected) {
        // if error, consume ctrl acknowlege from robot.
        wait_robot_ack("%32s %32s", cmd, result);
    }

    // Stream 'connecting' state is a transient state.
    TEST_ASSERT_TRUE(stream_ctxt->state == IOEXStreamState_connecting ||
                     stream_ctxt->state == IOEXStreamState_connected);
    TEST_ASSERT_TRUE(stream_ctxt->state_bits & (1 << IOEXStreamState_connecting));

    rc = wait_robot_ack("%32s %32s", cmd, result);
    TEST_ASSERT_TRUE(rc == 2);
    TEST_ASSERT_TRUE(strcmp(cmd, "sconnect") == 0);
    TEST_ASSERT_TRUE(strcmp(result, "success") == 0);

    cond_wait(stream_ctxt->cond);
    TEST_ASSERT_TRUE(stream_ctxt->state == IOEXStreamState_connected);
    TEST_ASSERT_TRUE(stream_ctxt->state_bits & (1 << IOEXStreamState_connected));

    rc = do_work_cb ? do_work_cb(context) : 0;
    TEST_ASSERT_TRUE(rc == 0);

    rc = IOEX_session_remove_stream(sctxt->session, stream_ctxt->stream_id);
    TEST_ASSERT_TRUE(rc == 0);
    stream_ctxt->stream_id = -1;

    cond_wait(stream_ctxt->cond);
    TEST_ASSERT_TRUE(stream_ctxt->state == IOEXStreamState_closed);
    TEST_ASSERT_TRUE(stream_ctxt->state_bits & (1 << IOEXStreamState_closed));

cleanup:
    if (stream_ctxt->stream_id > 0) {
        IOEX_session_remove_stream(sctxt->session, stream_ctxt->stream_id);
        stream_ctxt->stream_id = -1;
    }

    if (sctxt->session) {
        IOEX_session_close(sctxt->session);
        sctxt->session = NULL;
    }

    IOEX_session_cleanup(wctxt->carrier);
    robot_sfree();
}

const char* connection_str(enum IOEXConnectionStatus status)
{
    const char* str = NULL;

    switch (status) {
        case IOEXConnectionStatus_Connected:
            str = "connected";
            break;
        case IOEXConnectionStatus_Disconnected:
            str = "disconnected";
            break;
        default:
            str = "unknown";
            break;
    }
    return str;
}
