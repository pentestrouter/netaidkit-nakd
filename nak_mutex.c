#include <time.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include "nak_mutex.h"
#include "log.h"

void _nakd_mutex_lock(pthread_mutex_t *lock, const char *lock_name,
                                      const char *file, int line) {
    for (;;) {
        struct timespec timeout;
        clock_gettime(CLOCK_MONOTONIC, &timeout);
        timeout.tv_sec += NAKD_MUTEX_TIMEOUT;
        int lock_status = pthread_mutex_timedlock(lock, &timeout);
        if (lock_status == ETIMEDOUT) {
            nakd_log(L_CRIT, "mutex timeout: %s [%s:%d]", lock_name, file,
                                                                    line);
        } else if (lock_status) {
            nakd_log(L_CRIT, "error while locking mutex: %s",
                                      strerror(lock_status));
        } else {
            break;
        }
    }
}

void _nakd_mutex_unlock(pthread_mutex_t *lock, const char *lock_name,
                                        const char *file, int line) {
    pthread_mutex_unlock(lock);
}
