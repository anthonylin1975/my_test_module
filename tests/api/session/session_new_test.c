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
    .extra = NULL,
};

static SessionContext session_context = {
    .request_cb = NULL,
    .request_received = 0,
    .request_cond = NULL,

    .request_complete_cb = NULL,
    .request_complete_status = -1,
    .request_complete_cond = NULL,

    .session = NULL,
    .extra   = NULL
};

static void test_context_reset(TestContext *context)
{
    context->session->session = NULL;
}

static TestContext test_context = {
    .carrier = &carrier_context,
    .session = &session_context,
    .stream  = NULL,

    .context_reset = test_context_reset
};

static
void new_session_with_friend(TestContext *context)
{
    CarrierContext *wctxt = context->carrier;
    SessionContext *sctxt = context->session;
    int rc;

    context->context_reset(context);

    rc = add_friend_anyway(context, robotid, robotaddr);
    CU_ASSERT_EQUAL_FATAL(rc, 0);
    CU_ASSERT_TRUE_FATAL(IOEX_is_friend(wctxt->carrier, robotid));

    rc = IOEX_session_init(wctxt->carrier, NULL, NULL);
    CU_ASSERT_EQUAL_FATAL(rc, 0);

    sctxt->session = IOEX_session_new(wctxt->carrier, robotid);
    TEST_ASSERT_TRUE(sctxt->session != NULL);

    if (sctxt->session) {
        IOEX_session_close(sctxt->session);
        sctxt->session = NULL;
    }

cleanup:
    IOEX_session_cleanup(wctxt->carrier);
}

static void test_new_session(void)
{
    new_session_with_friend(&test_context);
}

static
void new_session_with_stranger(TestContext *context)
{
    CarrierContext *wctxt = context->carrier;
    SessionContext *sctxt = context->session;
    int rc;

    test_context_reset(context);

    rc = remove_friend_anyway(context, robotid);
    CU_ASSERT_EQUAL_FATAL(rc, 0);
    CU_ASSERT_FALSE_FATAL(IOEX_is_friend(wctxt->carrier, robotid));

    rc = IOEX_session_init(wctxt->carrier, NULL, NULL);
    CU_ASSERT_EQUAL_FATAL(rc, 0);

    sctxt->session = IOEX_session_new(wctxt->carrier, robotid);
    TEST_ASSERT_TRUE(!sctxt->session);
    TEST_ASSERT_TRUE(IOEX_get_error() == IOEX_GENERAL_ERROR(IOEXERR_NOT_EXIST));

cleanup:
    IOEX_session_cleanup(wctxt->carrier);
}

static void test_new_session_with_stranger(void)
{
    new_session_with_stranger(&test_context);
}

static void new_session_without_init(TestContext *context)
{
    CarrierContext *wctxt = context->carrier;
    SessionContext *sctxt = context->session;
    int rc;

    test_context_reset(context);

    rc = add_friend_anyway(context, robotid, robotaddr);
    CU_ASSERT_EQUAL_FATAL(rc, 0);
    CU_ASSERT_TRUE_FATAL(IOEX_is_friend(wctxt->carrier, robotid));

    sctxt->session = IOEX_session_new(wctxt->carrier, robotid);
    CU_ASSERT_PTR_NULL(sctxt->session);
    CU_ASSERT_EQUAL(IOEX_get_error(), IOEX_GENERAL_ERROR(IOEXERR_NOT_EXIST));
}

static void test_new_session_without_init(void)
{
    new_session_without_init(&test_context);
}

static CU_TestInfo cases[] = {
    { "test_new_session", test_new_session },
    { "test_new_session_with_stranger", test_new_session_with_stranger },
    { "test_new_session_without_init", test_new_session_without_init },
    { NULL, NULL }
};

CU_TestInfo *session_new_test_get_cases(void)
{
    return cases;
}

int session_new_test_suite_init(void)
{
    int rc;

    rc = test_suite_init(&test_context);
    if (rc < 0)
        CU_FAIL("Error: test suite initialize error");

    return rc;
}

int session_new_test_suite_cleanup(void)
{
    test_suite_cleanup(&test_context);

    return 0;
}
