#ifndef LOGPULSE_SHM_IPC_H
#define LOGPULSE_SHM_IPC_H

#include <errno.h>
#include <semaphore.h>
#include <stdint.h>
#include <sys/types.h>

#define LP_MAX_WORKERS     32
#define LP_TOP_N           10
#define LP_MAX_PATH        256
#define LP_MAX_IP          64
#define LP_MAX_URL         256
#define LP_MAX_DETAIL      128

#define LP_MIN_THREADS     1
#define LP_MAX_THREADS     16
#define LP_DEFAULT_THREADS 4

#define LP_CTRL_PAUSE      'P'
#define LP_CTRL_RESUME     'R'
#define LP_CTRL_QUIT       'Q'

typedef enum {
    LP_MODE_LOTE = 0,
    LP_MODE_VIVO = 1
} lp_mode_t;

typedef enum {
    WORKER_STARTING = 0,
    WORKER_ACTIVE   = 1,
    WORKER_PAUSED   = 2,
    WORKER_FINISHED = 3,
    WORKER_ERROR    = 4,
    WORKER_CRASHED  = 5
} worker_state_t;

typedef enum {
    EVT_INICIO  = 0,
    EVT_FIN     = 1,
    EVT_ERROR   = 2,
    EVT_PAUSA   = 3,
    EVT_REANUDA = 4
} status_evt_t;

typedef struct {
    int worker_idx;
    pid_t pid;
    status_evt_t event;
    int err_code;
    char detail[LP_MAX_DETAIL];
} status_msg_t;

typedef struct {
    char key[LP_MAX_URL];
    uint64_t count;
} top_count_entry_t;

typedef struct {
    char url[LP_MAX_URL];
    uint64_t count;
    double total_time_ms;
    double avg_time_ms;
    double max_time_ms;
} top_slow_entry_t;

typedef struct {
    sem_t sem;
    pid_t pid;
    worker_state_t state;
    char filepath[LP_MAX_PATH];

    uint64_t total_requests;
    double req_per_sec;
    uint64_t status_2xx;
    uint64_t status_3xx;
    uint64_t status_4xx;
    uint64_t status_5xx;
    uint64_t status_other;
    uint64_t total_bytes;
    uint64_t malformed_lines;

    uint32_t top_ip_count;
    top_count_entry_t top_ips[LP_TOP_N];

    uint32_t top_url_count;
    top_count_entry_t top_urls[LP_TOP_N];

    uint32_t top_slow_count;
    top_slow_entry_t top_slow_urls[LP_TOP_N];

    uint64_t seq;
} worker_shm_t;

typedef struct {
    sem_t ctrl_sem;
    int shutdown_requested;
    int paused;
    lp_mode_t mode;
    int threads_per_worker;
    int num_workers;
    worker_shm_t workers[LP_MAX_WORKERS];
} shm_layout_t;

static inline int lp_sem_wait(sem_t *sem) {
    while (sem_wait(sem) != 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return 0;
}

#endif /* LOGPULSE_SHM_IPC_H */
