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

#ifndef __DHT_WRAPPER_H__
#define __DHT_WRAPPER_H__

#define DHT_PUBLIC_KEY_SIZE     32U
#define DHT_ADDRESS_SIZE        (32U + sizeof(uint32_t) + sizeof(uint16_t))

typedef struct DHT DHT;

typedef bool (*FriendsIterateCallback)(uint32_t friend_number,
                                       const uint8_t *public_key,
                                       int user_status,
                                       const uint8_t *desc, size_t desc_length,
                                       void *context);

typedef void (*SelfInfoCallback)(const uint8_t *address,
                                 const uint8_t *public_key,
                                 int user_status,
                                 const uint8_t *desc, size_t desc_length,
                                 void *context);

int dht_new(const uint8_t *savedata, size_t datalen, bool udp_enabled, DHT *dht);

void dht_kill(DHT *dht);

int dht_bootstrap(DHT *dht, const char *ipv4, const char *ipv6, int port,
                  const uint8_t *address);

void dht_self_set_nospam(DHT *dht, uint32_t nospam);

uint32_t dht_self_get_nospam(DHT *dht);

void dht_self_get_secret_key(DHT *dht, uint8_t *secret_key);

void dht_self_set_status(DHT *dht, int status);

int dht_self_get_status(DHT *dht);

int dht_get_self_info(DHT *dht, SelfInfoCallback cb, void *context);

int dht_get_friends(DHT *dht, FriendsIterateCallback cb, void *context);

size_t dht_get_savedata_size(DHT *dht);

void dht_get_savedata(DHT *dht, uint8_t *data);

int dht_iteration_idle(DHT *dht);

void dht_iterate(DHT *dht, void *context);

int dht_self_set_name(DHT *dht, uint8_t *name, size_t length);

int dht_self_set_desc(DHT *dht, uint8_t *status_msg, size_t length);

int dht_get_friend_number(DHT *dht, const uint8_t *public_key,
                          uint32_t *friend_number);


int dht_friend_add(DHT *dht, const uint8_t *address, const uint8_t *msg,
                   size_t length, uint32_t *friend_number);


int dht_friend_add_norequest(DHT *dht, const uint8_t *public_key,
                             uint32_t *friend_number);


int dht_friend_message(DHT *dht, uint32_t friend_number,
                       const uint8_t *data, size_t length);

int dht_friend_delete(DHT *dht, uint32_t friend_number);

int dht_get_random_tcp_relay(DHT *dht, char *tcp_relay, size_t buflen,
                             uint8_t *public_key);

int dht_file_send_request(DHT *dht, uint32_t friend_number, const char *fullpath,
                          uint32_t *filenum);

int dht_file_send_accept(DHT *dht, uint32_t friend_number, const uint32_t file_number);

int dht_file_send_seek(DHT *dht, uint32_t friend_number, const uint32_t file_number, uint64_t position);

int dht_file_send_reject(DHT *dht, uint32_t friend_number, const uint32_t file_number);

int dht_file_send_pause(DHT *dht, uint32_t friend_number, const uint32_t file_number);

int dht_file_send_resume(DHT *dht, uint32_t friend_number, const uint32_t file_number);

int dht_file_send_cancel(DHT *dht, uint32_t friend_number, const uint32_t file_number);

int dht_file_send_chunk(DHT *dht, uint32_t friend_number, uint32_t file_number, uint64_t position, const uint8_t *data, int len);

#endif // __DHT_WRAPPER_H__
