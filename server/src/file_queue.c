/* file_queue.c */

#include "file_queue.h"
#include <stdlib.h>   // For malloc, calloc, free
#include <string.h>   // For memcpy (used indirectly during shallow copy)

/**
 * file_queue_init
 *
 * Allocate and set up a new file_queue_t of the requested capacity.
 * - Allocates memory for both the file_queue_t struct and its internal buffer of file_item_t.
 * - Initializes head, tail, count to 0.
 * - Initializes the mutex and condition variables.
 * - Returns NULL if any allocation fails.
 */
file_queue_t *file_queue_init(size_t capacity) {
    // Allocate the queue structure
    file_queue_t *q = malloc(sizeof(*q));
    if (!q) {
        return NULL;
    }
    // Allocate the circular buffer to hold 'capacity' file_item_t objects
    q->buffer   = calloc(capacity, sizeof(file_item_t));
    if (!q->buffer) {
        free(q);
        return NULL;
    }
    q->capacity = capacity;
    q->head     = 0;
    q->tail     = 0;
    q->count    = 0;

    // Initialize the mutex and condition variables
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_full, NULL);
    pthread_cond_init(&q->not_empty, NULL);

    return q;
}

/**
 * file_queue_destroy
 *
 * - Destroys mutex and condition variables.
 * - Iterates through the entire buffer and frees any data pointers still present.
 * - Frees the buffer array itself.
 * - Frees the queue struct.
 * - After this call, the queue pointer should not be used again.
 */
void file_queue_destroy(file_queue_t *q) {
    if (!q) {
        return;
    }
    // Destroy synchronization primitives
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);

    // Free any file_item_t.data still in the buffer
    for (size_t i = 0; i < q->capacity; ++i) {
        if (q->buffer[i].data) {
            free(q->buffer[i].data);
            q->buffer[i].data = NULL;
        }
    }
    // Free buffer array
    free(q->buffer);
    // Free queue struct
    free(q);
}

/**
 * file_queue_is_full
 *
 * Returns true if count == capacity, meaning no more items can be enqueued until one is dequeued.
 * Locks the mutex only briefly to read 'count' safely.
 */
bool file_queue_is_full(file_queue_t *q) {
    bool full;
    pthread_mutex_lock(&q->mutex);
    full = (q->count == q->capacity);
    pthread_mutex_unlock(&q->mutex);
    return full;
}

/**
 * file_queue_try_enqueue
 *
 * Attempt to enqueue an item without blocking.
 * - If (count < capacity), copy *item into buffer[tail], update tail & count, signal not_empty, return true.
 * - Otherwise, return false immediately.
 *
 * Note: We perform a shallow copy of the entire file_item_t. The 'data' pointer
 *       is copied as-is; it is not deep-copied. The owner remains responsible
 *       for freeing item.data exactly once (after the worker thread processes it).
 */
bool file_queue_try_enqueue(file_queue_t *q, const file_item_t *item) {
    bool ok = false;
    pthread_mutex_lock(&q->mutex);
    if (q->count < q->capacity) {
        // Copy the file_item_t structure into the queue slot
        q->buffer[q->tail] = *item;
        q->tail = (q->tail + 1) % q->capacity;
        q->count++;
        ok = true;
        // Signal one waiting consumer that there's data available
        pthread_cond_signal(&q->not_empty);
    }
    pthread_mutex_unlock(&q->mutex);
    return ok;
}

/**
 * file_queue_enqueue
 *
 * Blocking enqueue: Wait until space is available, then enqueue.
 * - Lock the mutex.
 * - While (count == capacity), wait on not_full.
 * - Once space exists, copy *item into buffer[tail], update tail & count.
 * - Signal not_empty in case any thread is waiting to dequeue.
 * - Unlock the mutex.
 *
 * Because we wait on not_full, this call will block if the queue is full until another thread dequeues.
 */
void file_queue_enqueue(file_queue_t *q, const file_item_t *item) {
    pthread_mutex_lock(&q->mutex);
    // Wait for space if queue is full
    while (q->count == q->capacity) {
        pthread_cond_wait(&q->not_full, &q->mutex);
    }
    // Copy the file_item_t structure into the queue slot
    q->buffer[q->tail] = *item;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    // Signal one waiting consumer that there is now at least one item
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
}

/**
 * file_queue_dequeue
 *
 * Blocking dequeue: Wait until an item is available, then remove and return it.
 * - Lock the mutex.
 * - While (count == 0), wait on not_empty.
 * - Copy the item from buffer[head] into a local file_item_t.
 * - Increment head (with wrap-around), decrement count.
 * - Signal not_full in case any thread is waiting to enqueue.
 * - Unlock the mutex.
 * - Return the local file_item_t. Caller becomes responsible for freeing item.data.
 */
file_item_t file_queue_dequeue(file_queue_t *q) {
    pthread_mutex_lock(&q->mutex);
    // Wait for data if queue is empty
    while (q->count == 0) {
        pthread_cond_wait(&q->not_empty, &q->mutex);
    }
    // Copy the item from the head of the queue
    file_item_t item = q->buffer[q->head];
    // Advance head index with wrap-around
    q->head = (q->head + 1) % q->capacity;
    // Decrement count
    q->count--;
    // Signal one waiting producer that space is now available
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
    return item;
}
