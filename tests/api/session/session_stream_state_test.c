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

#include <stdlib.h>
#include <unistd.h>
#include <CUnit/Basic.h>

#include "IOEX_carrier.h"
#include "IOEX_session.h"
#include "cond.h"
#include "tests.h"
#include "test_helper.h"
#include "test_assert.h"

static inline void wakeup(void* context)
{
    cond_signal(((CarrierContext *)context)->cond);
}

static void ready_cb(IOEXCarrier *w, void *context)
{
    cond_signal(((CarrierContext *)context)->ready_cond);
}

static
void friend_added_cb(IOEXCarrier *w, const IOEXFriendInfo *info, void *context)
{
    wakeup(context);
}

static void friend_removed_cb(IOEXCarrier *w, const char *friendid, void *context)
{
    wakeup(context);
}

static void friend_connection_cb(IOEXCarrier *w, const char *friendid,
                                 IOEXConnectionStatus status, void *context)
{
    CarrierContext *wctxt = (CarrierContext *)context;

    wakeup(context);
    wctxt->robot_online = (status == IOEXConnectionStatus_Connected);

    test_log_debug("Robot connection status changed -> %s\n",
                    connection_str(status));
}

static IOEXCallbacks callbacks = {
    .idle            = NULL,
    .connection_status = NULL,
    .ready           = ready_cb,
    .self_info       = NULL,
    .friend_list     = NULL,
    .friend_connection = friend_connection_cb,
    .friend_info     = NULL,
    .friend_presence = NULL,
    .friend_request  = NULL,
    .friend_added    = friend_added_cb,
    .friend_removed  = friend_removed_cb,
    .friend_message  = NULL,
    .friend_invite   = NULL
};

static Condition DEFINE_COND(carrier_ready_cond);
static Condition DEFINE_COND(carrier_cond);

static CarrierContext carrier_context = {
    .cbs = &callbacks,
    .carrier = NULL,
    .ready_cond = &carrier_ready_cond,
    .cond = &carrier_cond,
    .extra = NULL
};

static void session_request_complete_callback(IOEXSession *ws, int status,
                const char *reason, const char *sdp, size_t len, void *context)
{
    SessionContext *sctxt = (SessionContext *)context;

    test_log_debug("Session complete, status: %d, reason: %s\n", status,
                   reason ? reason : "null");

    sctxt->request_complete_status = status;

    if (status == 0) {
        int rc;

        rc = IOEX_session_start(ws, sdp, len);
        CU_ASSERT_EQUAL(rc, 0);
    }

    cond_signal(sctxt->request_complete_cond);
}

static Condition DEFINE_COND(session_request_complete_cond);

static SessionContext session_context = {
    .request_cb = NULL,
    .request_received = 0,
    .request_cond = NULL,

    .request_complete_cb = session_request_complete_callback,
    .request_complete_status = -1,
    .request_complete_cond = &session_request_complete_cond,

    .session = NULL,
    .extra   = NULL
};

static void stream_on_data(IOEXSession *ws, int stream, const void *data,
                           size_t len, void *context)
{
    test_log_debug("Stream [%d] received data [%.*s]\n", stream, (int)len,
                    (char*)data);
}

static void stream_state_changed(IOEXSession *ws, int stream,
                                 IOEXStreamState state, void *context)
{
    StreamContext *stream_ctxt = (StreamContext *)context;

    stream_ctxt->state = state;
    stream_ctxt->state_bits |= (1 << state);

    test_log_debug("Stream [%d] state changed to: %s\n", stream,
                    stream_state_name(state));

    cond_signal(stream_ctxt->cond);
}

static IOEXStreamCallbacks stream_callbacks = {
    .stream_data = stream_on_data,
    .state_changed = stream_state_changed
};

static Condition DEFINE_COND(stream_cond);

static StreamContext stream_context = {
    .cbs = &stream_callbacks,
    .stream_id = -1,
    .state = 0,
    .state_bits = 0,
    .cond = &stream_cond,
    .extra = NULL
};

static void test_context_reset(TestContext *context)
{
    SessionContext *session = context->session;
    StreamContext *stream = context->stream;

    cond_reset(session->request_complete_cond);

    session->request_received = 0;
    session->request_complete_status = -1;
    session->session = NULL;

    cond_reset(context->stream->cond);

    stream->stream_id = -1;
    stream->state = 0;
    stream->state_bits = 0;
}

static TestContext test_context = {
    .carrier = &carrier_context,
    .session = &session_context,
    .stream  = &stream_context,

    .context_reset = test_context_reset
};

static
int check_stream_state(TestContext *context, IOEXStreamState target_state)
{
    IOEXSession *session = context->session->session;
    int stream_id = context->stream->stream_id;
    int rc;
    IOEXStreamState cur_state;

    rc = IOEX_stream_get_state(session, stream_id, &cur_state);
    TEST_ASSERT_TRUE(rc == 0);
    TEST_ASSERT_TRUE(cur_state == target_state);
    return 0;

cleanup:
    return -1;
}

static
void test_stream_state_scheme(int stream_options, TestContext *context,
                    int (*check_state_cb)(TestContext *, IOEXStreamState))
{
    CarrierContext *wctxt = context->carrier;
    SessionContext *sctxt = context->session;
    StreamContext *stream_ctxt = context->stream;

    int rc;
    char cmd[32];
    char result[32];
    IOEXStreamState cur_state;

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
                                        IOEXStreamType_text, stream_options,
                                        stream_ctxt->cbs, stream_ctxt);
    TEST_ASSERT_TRUE(stream_ctxt->stream_id > 0);

    cond_wait(stream_ctxt->cond);
    TEST_ASSERT_TRUE(stream_ctxt->state == IOEXStreamState_initialized);
    TEST_ASSERT_TRUE(stream_ctxt->state_bits & (1 << IOEXStreamState_initialized));

    rc = check_state_cb(context, IOEXStreamState_initialized);
    TEST_ASSERT_TRUE(rc == 0);

    rc = IOEX_session_request(sctxt->session, sctxt->request_complete_cb, sctxt);
    TEST_ASSERT_TRUE(rc == 0);

    cond_wait(stream_ctxt->cond);
    TEST_ASSERT_TRUE(stream_ctxt->state == IOEXStreamState_transport_ready);
    TEST_ASSERT_TRUE(stream_ctxt->state_bits & (1 << IOEXStreamState_transport_ready));

    rc = check_state_cb(context, IOEXStreamState_transport_ready);
    TEST_ASSERT_TRUE(rc == 0);

    rc = wait_robot_ack("%32s %32s", cmd, result);
    TEST_ASSERT_TRUE(rc == 2);
    TEST_ASSERT_TRUE(strcmp(cmd, "srequest") == 0);
    TEST_ASSERT_TRUE(strcmp(result, "received") == 0);

    rc = robot_ctrl("sreply confirm %d %d\n", IOEXStreamType_text,
                    stream_options);
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

    TEST_ASSERT_TRUE(stream_ctxt->state == IOEXStreamState_connecting ||
                     stream_ctxt->state == IOEXStreamState_connected);
    TEST_ASSERT_TRUE(stream_ctxt->state_bits & (1 << IOEXStreamState_connecting));

    //TODO: need to replace with check_state_cb.
    rc = IOEX_stream_get_state(sctxt->session, stream_ctxt->stream_id, &cur_state);
    TEST_ASSERT_TRUE(rc == 0);
    TEST_ASSERT_TRUE(cur_state == IOEXStreamState_connecting ||
                     cur_state == IOEXStreamState_connected);

    rc = wait_robot_ack("%32s %32s", cmd, result);
    TEST_ASSERT_TRUE(rc == 2);
    TEST_ASSERT_TRUE(strcmp(cmd, "sconnect") == 0);
    TEST_ASSERT_TRUE(strcmp(result, "success") == 0);

    cond_wait(&stream_cond);
    TEST_ASSERT_TRUE(stream_ctxt->state == IOEXStreamState_connected);
    TEST_ASSERT_TRUE(stream_ctxt->state_bits & (1 << IOEXStreamState_connected));

    rc = check_state_cb(context, IOEXStreamState_connected);
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

static inline
void test_stream_state(int stream_options)
{
    return test_stream_state_scheme(stream_options, &test_context,
                                    check_stream_state);
}

static void test_normal_stream_state(void)
{
    test_stream_state(0);
}

static void test_plain_stream_state(void)
{
    int stream_options = 0;
    stream_options |= IOEX_STREAM_PLAIN;

    test_stream_state(stream_options);
}

static void test_reliable_stream_state(void)
{
    int stream_options = 0;
    stream_options |= IOEX_STREAM_RELIABLE;

    test_stream_state(stream_options);
}

static void test_plain_reliable_stream_state(void)
{
    int stream_options = 0;

    stream_options |= IOEX_STREAM_PLAIN;
    stream_options |= IOEX_STREAM_RELIABLE;

    test_stream_state(stream_options);
}

static void test_multiplexing_stream_state(void)
{
    int stream_options = 0;
    stream_options |= IOEX_STREAM_MULTIPLEXING;

    test_stream_state(stream_options);
}

static void test_plain_multiplexing_stream_state(void)
{
    int stream_options = 0;

    stream_options |= IOEX_STREAM_PLAIN;
    stream_options |= IOEX_STREAM_MULTIPLEXING;

    test_stream_state(stream_options);
}

static void test_reliable_multiplexing_stream_state(void)
{
    int stream_options = 0;

    stream_options |= IOEX_STREAM_RELIABLE;
    stream_options |= IOEX_STREAM_MULTIPLEXING;

    test_stream_state(stream_options);
}

static void test_plain_reliable_multiplexing_stream_state(void)
{
    int stream_options = 0;

    stream_options |= IOEX_STREAM_PLAIN;
    stream_options |= IOEX_STREAM_RELIABLE;
    stream_options |= IOEX_STREAM_MULTIPLEXING;

    test_stream_state(stream_options);
}

static void test_reliable_portforwarding_stream_state(void)
{
    int stream_options = 0;

    stream_options |= IOEX_STREAM_PLAIN;
    stream_options |= IOEX_STREAM_RELIABLE;
    stream_options |= IOEX_STREAM_MULTIPLEXING;
    stream_options |= IOEX_STREAM_PORT_FORWARDING;

    test_stream_state(stream_options);
}

static void test_plain_reliable_portforwarding_stream_state(void)
{
    int stream_options = 0;

    stream_options |= IOEX_STREAM_PLAIN;
    stream_options |= IOEX_STREAM_RELIABLE;
    stream_options |= IOEX_STREAM_MULTIPLEXING;
    stream_options |= IOEX_STREAM_PORT_FORWARDING;

    test_stream_state(stream_options);
}

static CU_TestInfo cases[] = {
    { "test_normal_stream_state", test_normal_stream_state },
    { "test_plain_stream_state", test_plain_stream_state },
    { "test_reliable_stream_state", test_reliable_stream_state },
    { "test_plain_reliable_stream_state", test_plain_reliable_stream_state },
    { "test_multiplexing_stream_state", test_multiplexing_stream_state },
    { "test_plain_multiplexing_stream_state", test_plain_multiplexing_stream_state },
    { "test_reliable_multiplexing_stream_state", test_reliable_multiplexing_stream_state },
    { "test_plain_reliable_multiplexing_stream_state", test_plain_reliable_multiplexing_stream_state },
    { "test_reliable_portforwarding_stream_state", test_reliable_portforwarding_stream_state },
    { "test_plain_reliable_portforwarding_stream_state", test_plain_reliable_portforwarding_stream_state },

    { NULL, NULL }
};

CU_TestInfo *session_stream_state_test_get_cases(void)
{
    return cases;
}

int session_stream_state_test_suite_init(void)
{
    int rc;

    rc = test_suite_init(&test_context);
    if (rc < 0)
        CU_FAIL("Error: test suite initialize error");

    return rc;
}

int session_stream_state_test_suite_cleanup(void)
{
    test_suite_cleanup(&test_context);

    return 0;
}
