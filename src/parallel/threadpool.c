#include "parallel.h"
#include <stdlib.h>
#include <string.h>

static void *worker_thread(void *arg) {
    ThreadPool *pool = (ThreadPool *)arg;

    while (1) {
        char *path = queue_pop(&pool->queue);
        if (!path) {
            // No more work
            break;
        }

        atomic_fetch_add(&pool->active_workers, 1);

        // Process the file
        pool->work_fn(path, pool->user_data);

        atomic_fetch_add(&pool->files_processed, 1);
        atomic_fetch_sub(&pool->active_workers, 1);

        free(path);
    }

    return NULL;
}

int threadpool_create(ThreadPool *pool, int thread_count,
                      worker_func worker, void *user_data) {
    memset(pool, 0, sizeof(*pool));

    pool->work_fn = worker;
    pool->user_data = user_data;
    pool->thread_count = thread_count;
    atomic_store(&pool->active_workers, 0);
    atomic_store(&pool->files_processed, 0);
    atomic_store(&pool->files_matched, 0);

    // Initialize queue with bounded capacity to prevent memory issues
    if (queue_init(&pool->queue, thread_count * 100) < 0) {
        return -1;
    }

    // Allocate thread array
    pool->threads = malloc(thread_count * sizeof(pthread_t));
    if (!pool->threads) {
        queue_free(&pool->queue);
        return -1;
    }

    // Start worker threads
    for (int i = 0; i < thread_count; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker_thread, pool) != 0) {
            // Failed to create thread - shutdown and cleanup
            queue_shutdown(&pool->queue);
            for (int j = 0; j < i; j++) {
                pthread_join(pool->threads[j], NULL);
            }
            free(pool->threads);
            queue_free(&pool->queue);
            return -1;
        }
    }

    return 0;
}

int threadpool_submit(ThreadPool *pool, const char *path) {
    return queue_push(&pool->queue, path);
}

void threadpool_destroy(ThreadPool *pool) {
    // Signal no more work
    queue_done_adding(&pool->queue);

    // Wait for all threads to finish
    for (int i = 0; i < pool->thread_count; i++) {
        pthread_join(pool->threads[i], NULL);
    }

    free(pool->threads);
    queue_free(&pool->queue);
}

int threadpool_files_processed(ThreadPool *pool) {
    return atomic_load(&pool->files_processed);
}

int threadpool_files_matched(ThreadPool *pool) {
    return atomic_load(&pool->files_matched);
}

void threadpool_report_match(ThreadPool *pool) {
    atomic_fetch_add(&pool->files_matched, 1);
}
