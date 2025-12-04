#include "parallel.h"
#include <stdlib.h>
#include <string.h>

int queue_init(WorkQueue *q, size_t capacity) {
    memset(q, 0, sizeof(*q));
    q->capacity = capacity;
    atomic_store(&q->shutdown, false);
    atomic_store(&q->done_adding, false);

    if (pthread_mutex_init(&q->mutex, NULL) != 0) {
        return -1;
    }

    if (pthread_cond_init(&q->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&q->mutex);
        return -1;
    }

    if (pthread_cond_init(&q->not_full, NULL) != 0) {
        pthread_mutex_destroy(&q->mutex);
        pthread_cond_destroy(&q->not_empty);
        return -1;
    }

    return 0;
}

void queue_free(WorkQueue *q) {
    // Free any remaining items
    WorkItem *item = q->head;
    while (item) {
        WorkItem *next = item->next;
        free(item->path);
        free(item);
        item = next;
    }

    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

int queue_push(WorkQueue *q, const char *path) {
    WorkItem *item = malloc(sizeof(WorkItem));
    if (!item) return -1;

    item->path = strdup(path);
    if (!item->path) {
        free(item);
        return -1;
    }
    item->next = NULL;

    pthread_mutex_lock(&q->mutex);

    // Wait if queue is full (and capacity is set)
    while (q->capacity > 0 && q->count >= q->capacity &&
           !atomic_load(&q->shutdown)) {
        pthread_cond_wait(&q->not_full, &q->mutex);
    }

    if (atomic_load(&q->shutdown)) {
        pthread_mutex_unlock(&q->mutex);
        free(item->path);
        free(item);
        return -1;
    }

    // Add to tail
    if (q->tail) {
        q->tail->next = item;
    } else {
        q->head = item;
    }
    q->tail = item;
    q->count++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);

    return 0;
}

char *queue_pop(WorkQueue *q) {
    pthread_mutex_lock(&q->mutex);

    // Wait for item or shutdown
    while (q->count == 0 && !atomic_load(&q->shutdown) &&
           !atomic_load(&q->done_adding)) {
        pthread_cond_wait(&q->not_empty, &q->mutex);
    }

    // Check if we should exit
    if (q->count == 0) {
        pthread_mutex_unlock(&q->mutex);
        return NULL;
    }

    // Remove from head
    WorkItem *item = q->head;
    q->head = item->next;
    if (!q->head) {
        q->tail = NULL;
    }
    q->count--;

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);

    char *path = item->path;
    free(item);
    return path;
}

void queue_done_adding(WorkQueue *q) {
    atomic_store(&q->done_adding, true);

    // Wake all waiting consumers
    pthread_mutex_lock(&q->mutex);
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
}

void queue_shutdown(WorkQueue *q) {
    atomic_store(&q->shutdown, true);

    // Wake all waiting threads
    pthread_mutex_lock(&q->mutex);
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
}
