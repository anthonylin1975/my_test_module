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
#include <CUnit/Basic.h>

#include "IOEX_carrier.h"
#include "cond.h"
#include "tests.h"
#include "test_helper.h"

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

static Condition DEFINE_COND(ready_cond);
static Condition DEFINE_COND(cond);

static CarrierContext carrier_context = {
    .cbs = &callbacks,
    .carrier = NULL,
    .ready_cond = &ready_cond,
    .cond = &cond,
    .extra = NULL
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

static void test_set_friend_label(void)
{
    IOEXCarrier *w = test_context.carrier->carrier;
    IOEXFriendInfo info;
    int rc;

    test_context.context_reset(&test_context);

    rc = add_friend_anyway(&test_context, robotid, robotaddr);;
    CU_ASSERT_EQUAL_FATAL(rc, 0);
    CU_ASSERT_TRUE_FATAL(IOEX_is_friend(w, robotid));

    rc = IOEX_set_friend_label(w, robotid, "test_robot");
    CU_ASSERT_EQUAL_FATAL(rc, 0);

    rc = IOEX_get_friend_info(w, robotid, &info);
    CU_ASSERT_EQUAL_FATAL(rc, 0);
    CU_ASSERT_STRING_EQUAL(info.label, "test_robot");
}

static void test_set_stranger_label(void)
{
    IOEXCarrier *w = test_context.carrier->carrier;
    int rc;

    test_context.context_reset(&test_context);

    rc = remove_friend_anyway(&test_context, robotid);
    CU_ASSERT_EQUAL_FATAL(rc, 0);
    CU_ASSERT_FALSE_FATAL(IOEX_is_friend(w, robotid));

    rc = IOEX_set_friend_label(w, robotid, "test_robot");
    CU_ASSERT_EQUAL(rc, -1);
    CU_ASSERT_EQUAL(IOEX_get_error(), IOEX_GENERAL_ERROR(IOEXERR_NOT_EXIST));
}

static void test_set_self_label(void)
{
    IOEXCarrier *w = test_context.carrier->carrier;
    char userid[IOEX_MAX_ID_LEN + 1];
    int rc;

    (void)IOEX_get_userid(w, userid, sizeof(userid));
    rc = IOEX_set_friend_label(w, userid, "test_robot");
    CU_ASSERT_EQUAL(rc, -1);
    CU_ASSERT_EQUAL(IOEX_get_error(), IOEX_GENERAL_ERROR(IOEXERR_NOT_EXIST));
}

static CU_TestInfo cases[] = {
    { "test_set_friend_label",   test_set_friend_label },
    { "test_set_stranger_label", test_set_stranger_label },
    { "test_set_self_label",     test_set_self_label },
    { NULL, NULL }
};

CU_TestInfo *friend_label_test_get_cases(void)
{
    return cases;
}

int friend_label_test_suite_init(void)
{
    int rc;

    rc = test_suite_init(&test_context);
    if (rc < 0) {
        CU_FAIL("Error: test suite initialize error");
        return -1;
    }

    return 0;
}

int friend_label_test_suite_cleanup(void)
{
    test_suite_cleanup(&test_context);

    return 0;
}
