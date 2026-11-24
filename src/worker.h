#ifndef LOGPULSE_WORKER_H
#define LOGPULSE_WORKER_H

#include "shm_ipc.h"

int worker_run(int worker_idx,
               const char *filepath,
               lp_mode_t mode,
               int num_threads,
               shm_layout_t *shm,
               int status_pipe_wr,
               int ctrl_pipe_rd);

#endif /* LOGPULSE_WORKER_H */
