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

#ifndef __DHT_CALLBACKS_H__
#define __DHT_CALLBACKS_H__

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct DHTCallbacks {
    void *context;

    void (*notify_connection)(bool connected, void *context);

    void (*notify_friend_desc)(uint32_t friend_number, const uint8_t *desc,
                               size_t length, void *context);

    void (*notify_friend_connection)(uint32_t friend_number, bool connected,
                                     void *context);

    void (*notify_friend_status)(uint32_t friend_number, int status,
                                 void *context);

    void (*notify_friend_request)(const uint8_t *public_key, const uint8_t *hello,
                                  size_t len, void *context);

    void (*notify_friend_message)(uint32_t friend_number, const uint8_t *message,
                                  size_t length, void *context);

    void (*notify_file_request)(uint32_t friend_number, uint32_t file_number, 
                                const uint8_t *filename, uint64_t filesize, 
                                void *context);

    void (*notify_file_accepted)(uint32_t friend_number, uint32_t file_number, 
                                 void *context);

    void (*notify_file_rejected)(uint32_t friend_number, uint32_t file_number, 
                                 void *context);

    void (*notify_file_paused)(uint32_t friend_number, uint32_t file_number, 
                               void *context);

    void (*notify_file_resumed)(uint32_t friend_number, uint32_t file_number, 
                                void *context);

    void (*notify_file_canceled)(uint32_t friend_number, uint32_t file_number, 
                                 void *context);

    void (*notify_file_chunk_request)(uint32_t friend_number, uint32_t file_number, uint64_t position,
                                      size_t length, void *context);

    void (*notify_file_chunk_receive)(uint32_t friend_number, uint32_t file_number, uint64_t position, 
                                      const uint8_t *data, size_t length, void *context);
};

typedef struct DHTCallbacks DHTCallbacks;

#ifdef __cplusplus
}
#endif

#endif
