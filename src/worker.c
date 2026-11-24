#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "worker.h"
#include "parser.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RING_CAPACITY 1024
#define PUBLISH_INTERVAL_MS 500

typedef struct {
    char *line;
    int is_poison;
} ring_item_t;

typedef struct {
    ring_item_t items[RING_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    pthread_mutex_t mtx;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
} ring_buffer_t;

typedef struct {
    int worker_idx;
    const char *filepath;
    lp_mode_t mode;
    int num_threads;

    shm_layout_t *shm;
    int status_pipe_wr;
    int ctrl_pipe_rd;

    int reader_wake_pipe[2];
    int done_pipe[2];

    ring_buffer_t ring;
    worker_stats_t stats;

    pthread_mutex_t ctrl_mtx;
    pthread_cond_t ctrl_cond;
    int paused;
    int stop_requested;

    pthread_mutex_t pub_mtx;
    pthread_cond_t pub_cond;
    int pub_stop;

    int open_failed;
    int open_errno;
} worker_ctx_t;

static volatile sig_atomic_t g_worker_sig_pipe_wr = -1;

static void worker_sig_handler(int sig) {
    (void)sig;
    int saved_errno = errno;
    if (g_worker_sig_pipe_wr >= 0) {
        char b = 'S';
        ssize_t r = write(g_worker_sig_pipe_wr, &b, 1);
        (void)r;
    }
    errno = saved_errno;
}

static int set_nonblock_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    int fdflags = fcntl(fd, F_GETFD, 0);
    if (fdflags >= 0) {
        fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC);
    }
    return 0;
}

static int write_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t w = write(fd, p + sent, len - sent);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        sent += (size_t)w;
    }
    return 0;
}

static void send_status_event(worker_ctx_t *ctx, status_evt_t evt, int err_code, const char *detail) {
    status_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.worker_idx = ctx->worker_idx;
    msg.pid = getpid();
    msg.event = evt;
    msg.err_code = err_code;
    if (detail) {
        snprintf(msg.detail, sizeof(msg.detail), "%s", detail);
    }
    (void)write_all(ctx->status_pipe_wr, &msg, sizeof(msg));
}

static int ring_init(ring_buffer_t *rb) {
    memset(rb, 0, sizeof(*rb));
    if (pthread_mutex_init(&rb->mtx, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&rb->not_full, NULL) != 0) {
        pthread_mutex_destroy(&rb->mtx);
        return -1;
    }
    if (pthread_cond_init(&rb->not_empty, NULL) != 0) {
        pthread_cond_destroy(&rb->not_full);
        pthread_mutex_destroy(&rb->mtx);
        return -1;
    }
    return 0;
}

static void ring_destroy(ring_buffer_t *rb) {
    for (size_t i = 0; i < rb->count; ++i) {
        size_t idx = (rb->head + i) % RING_CAPACITY;
        if (!rb->items[idx].is_poison && rb->items[idx].line) {
            free(rb->items[idx].line);
            rb->items[idx].line = NULL;
        }
    }
    pthread_cond_destroy(&rb->not_empty);
    pthread_cond_destroy(&rb->not_full);
    pthread_mutex_destroy(&rb->mtx);
}

static void ring_push(ring_buffer_t *rb, char *line, int is_poison) {
    pthread_mutex_lock(&rb->mtx);
    while (rb->count == RING_CAPACITY) {
        pthread_cond_wait(&rb->not_full, &rb->mtx);
    }
    rb->items[rb->tail].line = line;
    rb->items[rb->tail].is_poison = is_poison;
    rb->tail = (rb->tail + 1) % RING_CAPACITY;
    rb->count++;
    pthread_cond_signal(&rb->not_empty);
    pthread_mutex_unlock(&rb->mtx);
}

static ring_item_t ring_pop(ring_buffer_t *rb) {
    pthread_mutex_lock(&rb->mtx);
    while (rb->count == 0) {
        pthread_cond_wait(&rb->not_empty, &rb->mtx);
    }
    ring_item_t item = rb->items[rb->head];
    rb->items[rb->head].line = NULL;
    rb->items[rb->head].is_poison = 0;
    rb->head = (rb->head + 1) % RING_CAPACITY;
    rb->count--;
    pthread_cond_signal(&rb->not_full);
    pthread_mutex_unlock(&rb->mtx);
    return item;
}

static int worker_check_pause_or_stop(worker_ctx_t *ctx) {
    pthread_mutex_lock(&ctx->ctrl_mtx);
    while (ctx->paused && !ctx->stop_requested) {
        pthread_cond_wait(&ctx->ctrl_cond, &ctx->ctrl_mtx);
    }
    int stop = ctx->stop_requested;
    pthread_mutex_unlock(&ctx->ctrl_mtx);
    return stop;
}

static void push_poison_pills(worker_ctx_t *ctx) {
    for (int i = 0; i < ctx->num_threads; ++i) {
        ring_push(&ctx->ring, NULL, 1);
    }
}

static void *reader_thread_fn(void *arg) {
    worker_ctx_t *ctx = (worker_ctx_t *)arg;

    FILE *fp = fopen(ctx->filepath, "r");
    if (!fp) {
        ctx->open_errno = errno;
        ctx->open_failed = 1;
        send_status_event(ctx, EVT_ERROR, ctx->open_errno, strerror(ctx->open_errno));
        push_poison_pills(ctx);
        char d = 'E';
        (void)write_all(ctx->done_pipe[1], &d, 1);
        return NULL;
    }

    int inotify_fd = -1;
    int watch_fd = -1;
    if (ctx->mode == LP_MODE_VIVO) {
        inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (inotify_fd >= 0) {
            watch_fd = inotify_add_watch(inotify_fd, ctx->filepath,
                                          IN_MODIFY | IN_ATTRIB | IN_MOVE_SELF | IN_DELETE_SELF);
        }
    }

    char *line = NULL;
    size_t cap = 0;

    for (;;) {
        if (worker_check_pause_or_stop(ctx)) {
            break;
        }

        ssize_t nread = getline(&line, &cap, fp);
        if (nread >= 0) {
            char *copy = malloc((size_t)nread + 1);
            if (copy) {
                memcpy(copy, line, (size_t)nread + 1);
                ring_push(&ctx->ring, copy, 0);
            }
            continue;
        }

        /* Reached EOF or error */
        if (ferror(fp)) {
            ctx->open_errno = errno;
            ctx->open_failed = 1;
            send_status_event(ctx, EVT_ERROR, ctx->open_errno, "Error leyendo archivo");
            break;
        }

        if (ctx->mode == LP_MODE_LOTE) {
            break;
        }

        /* Live mode: wait without busy-waiting for file modifications or wake signal */
        clearerr(fp);

        struct stat st;
        off_t cur_pos = ftello(fp);
        if (fstat(fileno(fp), &st) == 0 && cur_pos >= 0 && st.st_size < cur_pos) {
            fseeko(fp, 0, SEEK_SET);
            continue;
        }

        struct pollfd pfds[2];
        nfds_t nfds = 0;
        pfds[nfds].fd = ctx->reader_wake_pipe[0];
        pfds[nfds].events = POLLIN;
        nfds++;

        if (inotify_fd >= 0 && watch_fd >= 0) {
            pfds[nfds].fd = inotify_fd;
            pfds[nfds].events = POLLIN;
            nfds++;
        }

        int timeout_ms = (nfds == 2) ? -1 : 500;
        int pr = poll(pfds, nfds, timeout_ms);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (pfds[0].revents & POLLIN) {
            char buf[64];
            while (read(ctx->reader_wake_pipe[0], buf, sizeof(buf)) > 0) {
            }
        }

        if (nfds == 2 && (pfds[1].revents & POLLIN)) {
            char evbuf[4096];
            while (read(inotify_fd, evbuf, sizeof(evbuf)) > 0) {
            }
        }
    }

    free(line);
    if (watch_fd >= 0 && inotify_fd >= 0) {
        inotify_rm_watch(inotify_fd, watch_fd);
    }
    if (inotify_fd >= 0) {
        close(inotify_fd);
    }
    fclose(fp);

    push_poison_pills(ctx);

    char d = ctx->open_failed ? 'E' : 'D';
    (void)write_all(ctx->done_pipe[1], &d, 1);
    return NULL;
}

static void *parser_thread_fn(void *arg) {
    worker_ctx_t *ctx = (worker_ctx_t *)arg;
    parsed_log_t rec;

    for (;;) {
        ring_item_t item = ring_pop(&ctx->ring);
        if (item.is_poison) {
            break;
        }
        int valid = (parse_combined_line(item.line, &rec) == 0);
        worker_stats_record(&ctx->stats, &rec, valid);
        free(item.line);
    }

    return NULL;
}

static void publish_snapshot_to_shm(worker_ctx_t *ctx) {
    worker_shm_t local;
    memset(&local, 0, sizeof(local));
    worker_stats_snapshot(&ctx->stats, &local);

    worker_shm_t *slot = &ctx->shm->workers[ctx->worker_idx];
    if (lp_sem_wait(&slot->sem) == 0) {
        slot->total_requests = local.total_requests;
        slot->req_per_sec = local.req_per_sec;
        slot->status_2xx = local.status_2xx;
        slot->status_3xx = local.status_3xx;
        slot->status_4xx = local.status_4xx;
        slot->status_5xx = local.status_5xx;
        slot->status_other = local.status_other;
        slot->total_bytes = local.total_bytes;
        slot->malformed_lines = local.malformed_lines;

        slot->top_ip_count = local.top_ip_count;
        memcpy(slot->top_ips, local.top_ips, sizeof(slot->top_ips));

        slot->top_url_count = local.top_url_count;
        memcpy(slot->top_urls, local.top_urls, sizeof(slot->top_urls));

        slot->top_slow_count = local.top_slow_count;
        memcpy(slot->top_slow_urls, local.top_slow_urls, sizeof(slot->top_slow_urls));

        slot->seq++;
        sem_post(&slot->sem);
    }
}

static void *publisher_thread_fn(void *arg) {
    worker_ctx_t *ctx = (worker_ctx_t *)arg;

    pthread_mutex_lock(&ctx->pub_mtx);
    while (!ctx->pub_stop) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_nsec += PUBLISH_INTERVAL_MS * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }

        int rc = pthread_cond_timedwait(&ctx->pub_cond, &ctx->pub_mtx, &ts);
        int stopping = ctx->pub_stop;
        pthread_mutex_unlock(&ctx->pub_mtx);

        if (rc == 0 || rc == ETIMEDOUT) {
            publish_snapshot_to_shm(ctx);
        }

        pthread_mutex_lock(&ctx->pub_mtx);
        if (stopping) {
            break;
        }
    }
    pthread_mutex_unlock(&ctx->pub_mtx);

    /* Final snapshot flush after parsers finish */
    publish_snapshot_to_shm(ctx);
    return NULL;
}

static void set_worker_shm_state(worker_ctx_t *ctx, worker_state_t st) {
    worker_shm_t *slot = &ctx->shm->workers[ctx->worker_idx];
    if (lp_sem_wait(&slot->sem) == 0) {
        slot->state = st;
        sem_post(&slot->sem);
    }
}

int worker_run(int worker_idx,
               const char *filepath,
               lp_mode_t mode,
               int num_threads,
               shm_layout_t *shm,
               int status_pipe_wr,
               int ctrl_pipe_rd) {
    worker_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.worker_idx = worker_idx;
    ctx.filepath = filepath;
    ctx.mode = mode;
    ctx.num_threads = num_threads;
    ctx.shm = shm;
    ctx.status_pipe_wr = status_pipe_wr;
    ctx.ctrl_pipe_rd = ctrl_pipe_rd;
    ctx.reader_wake_pipe[0] = -1;
    ctx.reader_wake_pipe[1] = -1;
    ctx.done_pipe[0] = -1;
    ctx.done_pipe[1] = -1;

    int sig_pipe[2] = {-1, -1};
    if (pipe(sig_pipe) != 0 || pipe(ctx.reader_wake_pipe) != 0 || pipe(ctx.done_pipe) != 0) {
        send_status_event(&ctx, EVT_ERROR, errno, "pipe() fallo en worker");
        if (sig_pipe[0] >= 0) { close(sig_pipe[0]); close(sig_pipe[1]); }
        if (ctx.reader_wake_pipe[0] >= 0) { close(ctx.reader_wake_pipe[0]); close(ctx.reader_wake_pipe[1]); }
        if (ctx.done_pipe[0] >= 0) { close(ctx.done_pipe[0]); close(ctx.done_pipe[1]); }
        return 1;
    }

    set_nonblock_cloexec(sig_pipe[0]);
    set_nonblock_cloexec(sig_pipe[1]);
    set_nonblock_cloexec(ctx.reader_wake_pipe[0]);
    set_nonblock_cloexec(ctx.reader_wake_pipe[1]);
    set_nonblock_cloexec(ctx.done_pipe[0]);
    set_nonblock_cloexec(ctx.done_pipe[1]);
    set_nonblock_cloexec(ctx.ctrl_pipe_rd);

    g_worker_sig_pipe_wr = sig_pipe[1];

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = worker_sig_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct sigaction sa_ign;
    memset(&sa_ign, 0, sizeof(sa_ign));
    sa_ign.sa_handler = SIG_IGN;
    sigemptyset(&sa_ign.sa_mask);
    sigaction(SIGPIPE, &sa_ign, NULL);
    sigaction(SIGCHLD, &sa_ign, NULL);

    if (ring_init(&ctx.ring) != 0 || worker_stats_init(&ctx.stats) != 0) {
        send_status_event(&ctx, EVT_ERROR, ENOMEM, "Error inicializando estructuras");
        close(sig_pipe[0]); close(sig_pipe[1]);
        close(ctx.reader_wake_pipe[0]); close(ctx.reader_wake_pipe[1]);
        close(ctx.done_pipe[0]); close(ctx.done_pipe[1]);
        return 1;
    }

    pthread_mutex_init(&ctx.ctrl_mtx, NULL);
    pthread_cond_init(&ctx.ctrl_cond, NULL);
    pthread_mutex_init(&ctx.pub_mtx, NULL);

    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    pthread_cond_init(&ctx.pub_cond, &cattr);
    pthread_condattr_destroy(&cattr);

    set_worker_shm_state(&ctx, WORKER_ACTIVE);
    send_status_event(&ctx, EVT_INICIO, 0, filepath);

    pthread_t *parser_tids = calloc((size_t)num_threads, sizeof(pthread_t));
    if (!parser_tids) {
        send_status_event(&ctx, EVT_ERROR, ENOMEM, "Sin memoria para hilos");
        worker_stats_destroy(&ctx.stats);
        ring_destroy(&ctx.ring);
        return 1;
    }

    pthread_t reader_tid;
    pthread_t pub_tid;
    int started_parsers = 0;

    for (int i = 0; i < num_threads; ++i) {
        if (pthread_create(&parser_tids[i], NULL, parser_thread_fn, &ctx) == 0) {
            started_parsers++;
        }
    }
    pthread_create(&pub_tid, NULL, publisher_thread_fn, &ctx);
    pthread_create(&reader_tid, NULL, reader_thread_fn, &ctx);

    /* Worker main loop monitors control pipe, signals, and reader completion */
    int reader_done = 0;
    while (!reader_done) {
        struct pollfd pfds[3];
        pfds[0].fd = ctx.ctrl_pipe_rd;
        pfds[0].events = POLLIN;
        pfds[1].fd = sig_pipe[0];
        pfds[1].events = POLLIN;
        pfds[2].fd = ctx.done_pipe[0];
        pfds[2].events = POLLIN;

        int pr = poll(pfds, 3, -1);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (pfds[0].revents & POLLIN) {
            char cmd;
            while (read(ctx.ctrl_pipe_rd, &cmd, 1) > 0) {
                if (cmd == LP_CTRL_PAUSE) {
                    pthread_mutex_lock(&ctx.ctrl_mtx);
                    ctx.paused = 1;
                    pthread_mutex_unlock(&ctx.ctrl_mtx);
                    set_worker_shm_state(&ctx, WORKER_PAUSED);
                    send_status_event(&ctx, EVT_PAUSA, 0, "Pausado");
                } else if (cmd == LP_CTRL_RESUME) {
                    pthread_mutex_lock(&ctx.ctrl_mtx);
                    ctx.paused = 0;
                    pthread_cond_broadcast(&ctx.ctrl_cond);
                    pthread_mutex_unlock(&ctx.ctrl_mtx);
                    set_worker_shm_state(&ctx, WORKER_ACTIVE);
                    send_status_event(&ctx, EVT_REANUDA, 0, "Reanudado");
                } else if (cmd == LP_CTRL_QUIT) {
                    pthread_mutex_lock(&ctx.ctrl_mtx);
                    ctx.stop_requested = 1;
                    ctx.paused = 0;
                    pthread_cond_broadcast(&ctx.ctrl_cond);
                    pthread_mutex_unlock(&ctx.ctrl_mtx);
                    char w = 'Q';
                    (void)write_all(ctx.reader_wake_pipe[1], &w, 1);
                }
            }
        }

        if (pfds[0].revents & (POLLHUP | POLLERR)) {
            pthread_mutex_lock(&ctx.ctrl_mtx);
            ctx.stop_requested = 1;
            ctx.paused = 0;
            pthread_cond_broadcast(&ctx.ctrl_cond);
            pthread_mutex_unlock(&ctx.ctrl_mtx);
            char w = 'Q';
            (void)write_all(ctx.reader_wake_pipe[1], &w, 1);
        }

        if (pfds[1].revents & POLLIN) {
            char buf[32];
            while (read(sig_pipe[0], buf, sizeof(buf)) > 0) {
            }
            pthread_mutex_lock(&ctx.ctrl_mtx);
            ctx.stop_requested = 1;
            ctx.paused = 0;
            pthread_cond_broadcast(&ctx.ctrl_cond);
            pthread_mutex_unlock(&ctx.ctrl_mtx);
            char w = 'Q';
            (void)write_all(ctx.reader_wake_pipe[1], &w, 1);
        }

        if (pfds[2].revents & POLLIN) {
            char buf[16];
            while (read(ctx.done_pipe[0], buf, sizeof(buf)) > 0) {
            }
            reader_done = 1;
        }
    }

    pthread_join(reader_tid, NULL);
    for (int i = 0; i < started_parsers; ++i) {
        pthread_join(parser_tids[i], NULL);
    }
    free(parser_tids);

    pthread_mutex_lock(&ctx.pub_mtx);
    ctx.pub_stop = 1;
    pthread_cond_signal(&ctx.pub_cond);
    pthread_mutex_unlock(&ctx.pub_mtx);
    pthread_join(pub_tid, NULL);

    if (ctx.open_failed) {
        set_worker_shm_state(&ctx, WORKER_ERROR);
    } else {
        set_worker_shm_state(&ctx, WORKER_FINISHED);
        send_status_event(&ctx, EVT_FIN, 0, "Lectura finalizada");
    }

    pthread_cond_destroy(&ctx.pub_cond);
    pthread_mutex_destroy(&ctx.pub_mtx);
    pthread_cond_destroy(&ctx.ctrl_cond);
    pthread_mutex_destroy(&ctx.ctrl_mtx);

    worker_stats_destroy(&ctx.stats);
    ring_destroy(&ctx.ring);

    g_worker_sig_pipe_wr = -1;
    close(sig_pipe[0]);
    close(sig_pipe[1]);
    close(ctx.reader_wake_pipe[0]);
    close(ctx.reader_wake_pipe[1]);
    close(ctx.done_pipe[0]);
    close(ctx.done_pipe[1]);
    close(ctx.status_pipe_wr);
    close(ctx.ctrl_pipe_rd);

    munmap(shm, sizeof(*shm));
    return ctx.open_failed ? 1 : 0;
}
