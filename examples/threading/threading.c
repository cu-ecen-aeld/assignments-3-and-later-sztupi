#include "threading.h"
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/poll.h>

// Optional: use these functions to add debug or error prints to your application
#define DEBUG_LOG(msg,...)
//#define DEBUG_LOG(msg,...) printf("threading: " msg "\n" , ##__VA_ARGS__)
#define ERROR_LOG(msg,...) printf("threading ERROR: " msg "\n" , ##__VA_ARGS__)

void* threadfunc(void* thread_param)
{
    struct thread_data* data = (struct thread_data*) thread_param;

    if (poll(NULL, 0, data->wait_to_obtain_ms) != 0) {
        perror("wait for obtain");
        return thread_param;
    }
    if (pthread_mutex_lock(data->mutex) != 0) {
        perror("mutex lock");
        return thread_param;
    }
    if (poll(NULL, 0, data->wait_to_release_ms) != 0) {
        perror("wait for release");
        pthread_mutex_unlock(data->mutex);
        return thread_param;
    }
    if (pthread_mutex_unlock(data->mutex) != 0) {
        perror("mutex unlock");
        return thread_param;
    }

    data->thread_complete_success = true;

    // TODO: wait, obtain mutex, wait, release mutex as described by thread_data structure
    // hint: use a cast like the one below to obtain thread arguments from your parameter
    //struct thread_data* thread_func_args = (struct thread_data *) thread_param;
    return thread_param;
}


bool start_thread_obtaining_mutex(pthread_t *thread, pthread_mutex_t *mutex,int wait_to_obtain_ms, int wait_to_release_ms)
{
    struct thread_data* data = (struct thread_data *)malloc(sizeof(struct thread_data));
    data->mutex = mutex;
    data->wait_to_obtain_ms = wait_to_obtain_ms;
    data->wait_to_release_ms = wait_to_release_ms;

    int rc = pthread_create(thread, NULL, threadfunc, (void *) data);
    if (rc != 0) {
        perror("start_thread");
        return false;
    }
    /**
     * TODO: allocate memory for thread_data, setup mutex and wait arguments, pass thread_data to created thread
     * using threadfunc() as entry point.
     *
     * return true if successful.
     *
     * See implementation details in threading.h file comment block
     */

    return true;
}

