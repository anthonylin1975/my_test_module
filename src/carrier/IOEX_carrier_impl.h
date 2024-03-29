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

#ifndef __IOEX_CARRIER_IMPL_H__
#define __IOEX_CARRIER_IMPL_H__

#include <stdlib.h>

#include <crypto.h>
#include <linkedhashtable.h>
#include <linkedlist.h>

#include "IOEX_carrier.h"

#include "dht_callbacks.h"
#include "dht.h"

#define MAX_IPV4_ADDRESS_LEN (15)
#define MAX_IPV6_ADDRESS_LEN (47)

typedef struct DHT {
    uint8_t padding[32];  // reserved for DHT.
} DHT;

typedef struct BootstrapNodeBuf {
    char ipv4[MAX_IPV4_ADDRESS_LEN + 1];
    char ipv6[MAX_IPV6_ADDRESS_LEN + 1];
    uint16_t port;
    uint8_t public_key[DHT_PUBLIC_KEY_SIZE];
} BootstrapNodeBuf;

typedef struct Preferences {
    char *data_location;
    bool udp_enabled;
    size_t bootstraps_size;
    BootstrapNodeBuf *bootstraps;
} Preferences;

typedef enum FriendEventType {
    FriendEventType_Added,
    FriendEventType_Removed
} FriendEventType;

typedef struct FriendEvent {
    ListEntry le;
    FriendEventType type;
    IOEXFriendInfo fi;
} FriendEvent;

typedef struct FileTracker {
    ListEntry le;
    IOEXFileInfo fi;
} FileTracker;

struct IOEXCarrier {
    void *session;  // reserved for session.

    DHT dht;

    Preferences pref;

    uint8_t public_key[DHT_PUBLIC_KEY_SIZE];
    uint8_t address[DHT_ADDRESS_SIZE];
    char base58_addr[IOEX_MAX_ADDRESS_LEN + 1];

    IOEXUserInfo me;
    IOEXPresenceStatus presence_status;
    IOEXConnectionStatus connection_status;
    bool is_ready;

    IOEXCallbacks callbacks;
    void *context;

    DHTCallbacks dht_callbacks;

    List *friend_events; // for friend_added/removed.
    Hashtable *friends;

    List *file_senders;
    List *file_receivers;

    Hashtable *tcallbacks;
    Hashtable *thistory;

    pthread_t main_thread;

    int running;
    int quit;
};

typedef void (*friend_invite_callback)(IOEXCarrier *, const char *,
                                       const char *, size_t, void *);
typedef struct SessionExtension {
    IOEXCarrier              *carrier;

    friend_invite_callback  friend_invite_cb;
    void                    *friend_invite_context;

    uint8_t                 reserved[1];
} SessionExtension;

typedef struct FriendLabelItem {
    uint32_t friend_number;
    char *label;
} FriendLabelItem;

typedef struct TransactedCallback {
    HashEntry he;
    int64_t tid;
    void *callback_func;
    void *callback_context;
} TransactedCallback;

CARRIER_API
void IOEX_set_error(int error);

#endif /* __IOEX_CARRIER_IMPL_H__ */
