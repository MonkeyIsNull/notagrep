#ifndef NOTAGREP_PARALLEL_H
#define NOTAGREP_PARALLEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>

// Work item: a file path to process
typedef struct WorkItem {
    char *path;                 // File path (owned)
    struct WorkItem *next;      // Next item in queue
} WorkItem;

// Thread-safe work queue
typedef struct {
    WorkItem *head;
    WorkItem *tail;
    size_t count;
    size_t capacity;            // Max items (0 = unlimited)

    pthread_mutex_t mutex;
    pthread_cond_t not_empty;   // Signal when items available
    pthread_cond_t not_full;    // Signal when space available

    atomic_bool shutdown;       // Signal threads to exit
    atomic_bool done_adding;    // No more items will be added
} WorkQueue;

// Initialize work queue
int queue_init(WorkQueue *q, size_t capacity);

// Free work queue (also frees any remaining items)
void queue_free(WorkQueue *q);

// Add item to queue (blocks if full)
// Returns 0 on success, -1 if shutdown
int queue_push(WorkQueue *q, const char *path);

// Get item from queue (blocks if empty)
// Returns path on success, NULL if shutdown and empty
char *queue_pop(WorkQueue *q);

// Signal that no more items will be added
void queue_done_adding(WorkQueue *q);

// Signal shutdown (wake all waiting threads)
void queue_shutdown(WorkQueue *q);

// Worker function signature
typedef void (*worker_func)(const char *path, void *user_data);

// Thread pool
typedef struct {
    pthread_t *threads;
    int thread_count;

    WorkQueue queue;
    worker_func work_fn;
    void *user_data;

    atomic_int active_workers;  // Number of workers currently processing
    atomic_int files_processed;
    atomic_int files_matched;
} ThreadPool;

// Create thread pool
// worker: function called for each file path
// user_data: passed to worker function
int threadpool_create(ThreadPool *pool, int thread_count,
                      worker_func worker, void *user_data);

// Submit a file path to be processed
int threadpool_submit(ThreadPool *pool, const char *path);

// Wait for all work to complete and destroy pool
void threadpool_destroy(ThreadPool *pool);

// Get number of files processed
int threadpool_files_processed(ThreadPool *pool);

// Get number of files with matches
int threadpool_files_matched(ThreadPool *pool);

// Increment match count (called by worker)
void threadpool_report_match(ThreadPool *pool);

#endif // NOTAGREP_PARALLEL_H
