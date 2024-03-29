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
#include <string.h>
#include <CUnit/Basic.h>

#include "IOEX_carrier.h"
#include "cond.h"
#include "tests.h"
#include "test_helper.h"

struct CarrierContextExtra {
    char* from;
    char* msg;
    int len;
};

static CarrierContextExtra extra = {
    .from = NULL,
    .msg  = NULL,
    .len  = 0
};

static inline void wakeup(void* context)
{
    cond_signal(((CarrierContext *)context)->cond);
}

static void ready_cb(IOEXCarrier *w, void *context)
{
    cond_signal(((CarrierContext *)context)->ready_cond);
}

static void friend_added_cb(IOEXCarrier *w, const IOEXFriendInfo *info, void *context)
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

static void friend_message_cb(IOEXCarrier *w, const char *from, const void *msg, size_t len,
                              void *context)
{
    CarrierContextExtra *extra = ((CarrierContext *)context)->extra;

    extra->from = strdup(from);
    extra->msg  = strdup((const char *)msg);
    extra->len  = (int)len;

    wakeup(context);
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
    .friend_message  = friend_message_cb,
    .friend_invite   = NULL
};

static Condition DEFINE_COND(ready_cond);
static Condition DEFINE_COND(cond);

static CarrierContext carrier_context = {
    .cbs = &callbacks,
    .carrier = NULL,
    .ready_cond = &ready_cond,
    .cond = &cond,
    .extra = &extra
};

static void test_context_reset(TestContext *context)
{
    cond_reset(context->carrier->cond);
}

static TestContext test_context = {
    .carrier = &carrier_context,
    .session = NULL,
    .stream  = NULL,
    .context_reset = test_context_reset
};

static void test_send_message_to_friend(void)
{
    CarrierContext *wctxt = test_context.carrier;
    int rc;

    test_context.context_reset(&test_context);

    rc = add_friend_anyway(&test_context, robotid, robotaddr);
    CU_ASSERT_EQUAL_FATAL(rc, 0);
    CU_ASSERT_TRUE_FATAL(IOEX_is_friend(wctxt->carrier, robotid));

    const char* out = "message-test";
    rc = IOEX_send_friend_message(wctxt->carrier, robotid, out, strlen(out) + 1);
    CU_ASSERT_EQUAL_FATAL(rc, 0);

    char in[64];
    rc = wait_robot_ack("%64s", in);
    CU_ASSERT_EQUAL(rc, 1);
    CU_ASSERT_STRING_EQUAL(in, out);
}

static void test_send_message_from_friend(void)
{
    CarrierContext *wctxt = test_context.carrier;
    CarrierContextExtra *extra = wctxt->extra;
    char userid[IOEX_MAX_ID_LEN + 1];
    int rc;

    test_context.context_reset(&test_context);

    rc = add_friend_anyway(&test_context, robotid, robotaddr);
    CU_ASSERT_EQUAL_FATAL(rc, 0);
    CU_ASSERT_TRUE_FATAL(IOEX_is_friend(wctxt->carrier, robotid));

    IOEX_get_userid(wctxt->carrier, userid, sizeof(userid));
    const char* msg = "message-test";

    rc = robot_ctrl("fmsg %s %s\n", userid, msg);
    CU_ASSERT_FATAL(rc > 0);

    // wait for message from robot.
    cond_wait(wctxt->cond);

    CU_ASSERT_NSTRING_EQUAL(extra->from, robotid, strlen(robotid));
    CU_ASSERT_STRING_EQUAL(extra->msg, msg);
    CU_ASSERT_EQUAL(strlen(extra->msg), strlen(msg));
    CU_ASSERT_EQUAL(extra->len, strlen(msg) + 1);

    FREE_ANYWAY(extra->from);
    FREE_ANYWAY(extra->msg);
}

static void test_send_message_to_stranger(void)
{
    CarrierContext *wctxt = test_context.carrier;
    int rc;

    test_context.context_reset(&test_context);

    rc = remove_friend_anyway(&test_context, robotid);
    CU_ASSERT_EQUAL_FATAL(rc, 0);
    CU_ASSERT_FALSE_FATAL(IOEX_is_friend(wctxt->carrier, robotid));

    const char* msg = "test-message";
    rc = IOEX_send_friend_message(wctxt->carrier, robotid, msg, strlen(msg));
    CU_ASSERT_EQUAL(rc, -1);
    CU_ASSERT_EQUAL(IOEX_get_error(), IOEX_GENERAL_ERROR(IOEXERR_NOT_EXIST));
}

static void test_send_message_to_self(void)
{
    CarrierContext *wctxt = test_context.carrier;
    char userid[IOEX_MAX_ID_LEN + 1];
    char nodeid[IOEX_MAX_ID_LEN + 1];
    const char* msg = "test-message";
    int rc;

    test_context.context_reset(&test_context);

    (void)IOEX_get_userid(wctxt->carrier, userid, sizeof(userid));
    (void)IOEX_get_nodeid(wctxt->carrier, nodeid, sizeof(nodeid));
    rc = IOEX_send_friend_message(wctxt->carrier, userid, msg, strlen(msg));
    CU_ASSERT_EQUAL_FATAL(rc, -1);
    CU_ASSERT_EQUAL_FATAL(IOEX_get_error(), IOEX_GENERAL_ERROR(IOEXERR_INVALID_ARGS));
}

static CU_TestInfo cases[] = {
    { "test_send_message_to_friend",   test_send_message_to_friend },
    { "test_send_message_from_friend", test_send_message_from_friend },
    { "test_send_message_to_stranger", test_send_message_to_stranger },
    { "test_send_message_to_self",     test_send_message_to_self },
    {NULL, NULL }
};

CU_TestInfo *friend_message_test_get_cases(void)
{
    return cases;
}

int friend_message_test_suite_init(void)
{
    int rc;

    rc = test_suite_init(&test_context);
    if (rc < 0) {
        CU_FAIL("Error: test suite initialize error");
        return -1;
    }

    return 0;
}

int friend_message_test_suite_cleanup(void)
{
    test_suite_cleanup(&test_context);

    return 0;
}
