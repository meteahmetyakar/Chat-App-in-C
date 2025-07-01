/* file_queue.h */

#ifndef FILE_QUEUE_H
#define FILE_QUEUE_H

#include <stddef.h>     // For size_t
#include <stdbool.h>    // For bool type
#include <pthread.h>    // For pthread_mutex_t, pthread_cond_t

/* We need USERNAME_LEN from chatserver.h in order to size the sender/target arrays. */
#include "chatserver.h"

#define MAX_FILENAME 256  /* Maximum length for a filename (including terminating '\0') */

/**
 * file_item_t
 *
 * Represents a single file that needs to be delivered from one user to another.
 * - filename:   Name of the file (up to MAX_FILENAME-1 characters + '\0')
 * - size:       Size of the file data in bytes
 * - data:       Pointer to a heap-allocated buffer containing exactly 'size' bytes of file content
 * - sender:     The username of the sender (up to USERNAME_LEN-1 characters + '\0')
 * - target:     The username of the intended recipient (up to USERNAME_LEN-1 characters + '\0')
 * - is_sentinel:Is this a “poison pill” to tell worker threads to exit? 1 = yes, 0 = no
 *
 * Note: We perform a shallow copy of this structure when enqueuing. That means
 *       'data' is not deep-copied; the pointer itself is copied, and the worker
 *       thread will be responsible for freeing item.data once done.
 */
typedef struct {
    char     filename[MAX_FILENAME];
    size_t   size;
    char    *data;
    char     sender[USERNAME_LEN];
    char     target[USERNAME_LEN];
    int      is_sentinel;
} file_item_t;

/**
 * file_queue_t
 *
 * A fixed-capacity, thread-safe circular buffer (FIFO) for file_item_t entries.
 * Producers (client-handler threads) enqueue new file_item_t objects when clients
 * request to send a file to another user. Worker threads dequeue items and
 * perform the actual file transfer.
 *
 * Fields:
 * - buffer:    Dynamically allocated array of file_item_t objects, length = capacity
 * - capacity:  Maximum number of file_item_t items this queue can hold
 * - head:      Index of the next item to be dequeued
 * - tail:      Index where the next item will be enqueued
 * - count:     Current number of items in the queue
 * - mutex:     Protects all access to head, tail, count, and buffer[]
 * - not_full:  Condition variable signaled whenever an item is dequeued (making space)
 * - not_empty: Condition variable signaled whenever an item is enqueued (making data available)
 */
struct file_queue {
    file_item_t    *buffer;
    size_t          capacity;
    size_t          head;
    size_t          tail;
    size_t          count;
    pthread_mutex_t mutex;
    pthread_cond_t  not_full;
    pthread_cond_t  not_empty;
};

typedef struct file_queue file_queue_t;

/**
 * file_queue_init
 *   Allocate and initialize a new file_queue_t with a fixed capacity.
 *   Returns NULL on allocation failure, or a pointer to the new queue.
 *
 *   - capacity: Maximum number of file_item_t elements that can be stored concurrently.
 */
file_queue_t *file_queue_init(size_t capacity);

/**
 * file_queue_destroy
 *   Free all resources associated with a file_queue_t.
 *   Also frees any file_item_t.data pointers still present in the buffer.
 *
 *   After calling this, the pointer should not be used again.
 */
void file_queue_destroy(file_queue_t *q);

/**
 * file_queue_is_full
 *   Returns true if the queue is currently at capacity. Otherwise, returns false.
 *   This is a non-blocking, read-only check (locks just long enough to read count).
 */
bool file_queue_is_full(file_queue_t *q);

/**
 * file_queue_try_enqueue
 *   Attempt to add 'item' to the queue without blocking.
 *   If the queue is already full, returns false immediately.
 *   Otherwise, performs a shallow copy of *item into the queue,
 *   signals not_empty, and returns true.
 */
bool file_queue_try_enqueue(file_queue_t *q, const file_item_t *item);

/**
 * file_queue_enqueue
 *   Blocking enqueue: If the queue is full, this call will wait (block) until space becomes available.
 *   Once there is space, performs a shallow copy of *item into the queue, signals not_empty, and returns.
 */
void file_queue_enqueue(file_queue_t *q, const file_item_t *item);

/**
 * file_queue_dequeue
 *   Blocking dequeue: If the queue is empty, this call will wait (block) until an item is enqueued.
 *   Returns a copy of the file_item_t that was stored at the head index. Updates head & count, and signals not_full.
 *
 *   Caller takes ownership of the returned file_item_t.data buffer and is responsible for calling free() on it when done.
 */
file_item_t file_queue_dequeue(file_queue_t *q);

#endif // FILE_QUEUE_H
