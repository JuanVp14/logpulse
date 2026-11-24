#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "coordinator.h"
#include "ui.h"
#include "worker.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t g_coord_sig_pipe_wr = -1;

static void coord_sig_handler(int sig) {
    int saved_errno = errno;
    if (g_coord_sig_pipe_wr >= 0) {
        char ch = '?';
        if (sig == SIGCHLD) {
            ch = 'C';
        } else if (sig == SIGINT) {
            ch = 'I';
        } else if (sig == SIGTERM) {
            ch = 'T';
        }
        ssize_t r = write(g_coord_sig_pipe_wr, &ch, 1);
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

static void safe_write_byte(int fd, char ch) {
    if (fd < 0) {
        return;
    }
    for (;;) {
        ssize_t w = write(fd, &ch, 1);
        if (w >= 0 || errno != EINTR) {
            break;
        }
    }
}

const char *worker_state_str(worker_state_t st) {
    switch (st) {
        case WORKER_STARTING: return "INICIANDO";
        case WORKER_ACTIVE:   return "ACTIVO";
        case WORKER_PAUSED:   return "PAUSADO";
        case WORKER_FINISHED: return "FINALIZADO";
        case WORKER_ERROR:    return "ERROR";
        case WORKER_CRASHED:  return "CAIDO";
        default:              return "DESCONOCIDO";
    }
}

void coordinator_wake_ui(coordinator_t *coord) {
    safe_write_byte(coord->ui_wake_pipe[1], 'W');
}

void coordinator_request_stop(coordinator_t *coord) {
    pthread_mutex_lock(&coord->state_mtx);
    coord->stop_requested = 1;
    pthread_mutex_unlock(&coord->state_mtx);
    safe_write_byte(coord->sig_pipe[1], 'Q');
}

static void push_event_log_locked(coordinator_t *coord, const char *fmt, ...) {
    char msg[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    int slot = (coord->evt_log_head + coord->evt_log_count) % LP_EVT_LOG_CAP;
    if (coord->evt_log_count == LP_EVT_LOG_CAP) {
        slot = coord->evt_log_head;
        coord->evt_log_head = (coord->evt_log_head + 1) % LP_EVT_LOG_CAP;
    } else {
        coord->evt_log_count++;
    }
    snprintf(coord->evt_log[slot], sizeof(coord->evt_log[slot]), "%s", msg);
}

void coordinator_push_event_log(coordinator_t *coord, const char *fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&coord->state_mtx);
    push_event_log_locked(coord, "%s", buf);
    pthread_mutex_unlock(&coord->state_mtx);
    coordinator_wake_ui(coord);
}

void coordinator_toggle_pause(coordinator_t *coord) {
    int new_paused = 0;
    int ctrl_fds[LP_MAX_WORKERS];
    int n_alive = 0;

    pthread_mutex_lock(&coord->state_mtx);
    coord->paused = !coord->paused;
    new_paused = coord->paused;
    for (int i = 0; i < coord->num_workers; ++i) {
        if (coord->workers[i].alive && coord->workers[i].ctrl_pipe_wr >= 0) {
            ctrl_fds[n_alive++] = coord->workers[i].ctrl_pipe_wr;
        }
    }
    push_event_log_locked(coord, "%s",
                          new_paused ? "Procesamiento PAUSADO por usuario"
                                     : "Procesamiento REANUDADO por usuario");
    pthread_mutex_unlock(&coord->state_mtx);

    if (lp_sem_wait(&coord->shm->ctrl_sem) == 0) {
        coord->shm->paused = new_paused;
        sem_post(&coord->shm->ctrl_sem);
    }

    char cmd = new_paused ? LP_CTRL_PAUSE : LP_CTRL_RESUME;
    for (int i = 0; i < n_alive; ++i) {
        safe_write_byte(ctrl_fds[i], cmd);
    }

    coordinator_wake_ui(coord);
}

static int cmp_top_count_desc(const void *a, const void *b) {
    const top_count_entry_t *ea = (const top_count_entry_t *)a;
    const top_count_entry_t *eb = (const top_count_entry_t *)b;
    if (eb->count > ea->count) return 1;
    if (eb->count < ea->count) return -1;
    return strcmp(ea->key, eb->key);
}

static int cmp_top_slow_desc(const void *a, const void *b) {
    const top_slow_entry_t *ea = (const top_slow_entry_t *)a;
    const top_slow_entry_t *eb = (const top_slow_entry_t *)b;
    if (eb->avg_time_ms > ea->avg_time_ms) return 1;
    if (eb->avg_time_ms < ea->avg_time_ms) return -1;
    if (eb->max_time_ms > ea->max_time_ms) return 1;
    if (eb->max_time_ms < ea->max_time_ms) return -1;
    return strcmp(ea->url, eb->url);
}

static void merge_top_count(top_count_entry_t *pool,
                            size_t *pool_len,
                            size_t pool_cap,
                            const top_count_entry_t *entries,
                            uint32_t n) {
    for (uint32_t i = 0; i < n && i < LP_TOP_N; ++i) {
        if (entries[i].key[0] == '\0') {
            continue;
        }
        int found = 0;
        for (size_t j = 0; j < *pool_len; ++j) {
            if (strcmp(pool[j].key, entries[i].key) == 0) {
                pool[j].count += entries[i].count;
                found = 1;
                break;
            }
        }
        if (!found && *pool_len < pool_cap) {
            pool[*pool_len] = entries[i];
            (*pool_len)++;
        }
    }
}

static void merge_top_slow(top_slow_entry_t *pool,
                           size_t *pool_len,
                           size_t pool_cap,
                           const top_slow_entry_t *entries,
                           uint32_t n) {
    for (uint32_t i = 0; i < n && i < LP_TOP_N; ++i) {
        if (entries[i].url[0] == '\0') {
            continue;
        }
        int found = 0;
        for (size_t j = 0; j < *pool_len; ++j) {
            if (strcmp(pool[j].url, entries[i].url) == 0) {
                pool[j].count += entries[i].count;
                pool[j].total_time_ms += entries[i].total_time_ms;
                if (entries[i].max_time_ms > pool[j].max_time_ms) {
                    pool[j].max_time_ms = entries[i].max_time_ms;
                }
                pool[j].avg_time_ms = pool[j].count > 0
                    ? (pool[j].total_time_ms / (double)pool[j].count)
                    : 0.0;
                found = 1;
                break;
            }
        }
        if (!found && *pool_len < pool_cap) {
            pool[*pool_len] = entries[i];
            (*pool_len)++;
        }
    }
}

void coordinator_get_consolidated(coordinator_t *coord, consolidated_metrics_t *out) {
    memset(out, 0, sizeof(*out));

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    pthread_mutex_lock(&coord->state_mtx);
    out->num_workers = coord->num_workers;
    out->paused = coord->paused;
    out->mode = coord->mode;
    out->threads_per_worker = coord->threads_per_worker;
    out->elapsed_sec = (double)(now.tv_sec - coord->start_ts.tv_sec) +
                       (double)(now.tv_nsec - coord->start_ts.tv_nsec) / 1e9;
    if (out->elapsed_sec < 0.001) {
        out->elapsed_sec = 0.001;
    }

    for (int i = 0; i < coord->num_workers; ++i) {
        out->workers[i].pid = coord->workers[i].pid;
        snprintf(out->workers[i].filepath, sizeof(out->workers[i].filepath),
                 "%s", coord->workers[i].filepath);
        out->workers[i].state = coord->workers[i].state;
        snprintf(out->workers[i].last_msg, sizeof(out->workers[i].last_msg),
                 "%s", coord->workers[i].last_msg);
    }

    out->evt_log_count = coord->evt_log_count;
    for (int i = 0; i < coord->evt_log_count; ++i) {
        int idx = (coord->evt_log_head + i) % LP_EVT_LOG_CAP;
        snprintf(out->evt_log[i], sizeof(out->evt_log[i]), "%s", coord->evt_log[idx]);
    }
    pthread_mutex_unlock(&coord->state_mtx);

    top_count_entry_t ip_pool[LP_MAX_WORKERS * LP_TOP_N];
    size_t ip_pool_len = 0;

    top_count_entry_t url_pool[LP_MAX_WORKERS * LP_TOP_N];
    size_t url_pool_len = 0;

    top_slow_entry_t slow_pool[LP_MAX_WORKERS * LP_TOP_N];
    size_t slow_pool_len = 0;

    for (int i = 0; i < out->num_workers; ++i) {
        worker_shm_t local_shm;
        memset(&local_shm, 0, sizeof(local_shm));

        worker_shm_t *slot = &coord->shm->workers[i];
        if (lp_sem_wait(&slot->sem) == 0) {
            memcpy(&local_shm, slot, sizeof(local_shm));
            sem_post(&slot->sem);
        }

        worker_view_t *wv = &out->workers[i];
        wv->total_requests = local_shm.total_requests;
        wv->req_per_sec = local_shm.req_per_sec;
        wv->status_2xx = local_shm.status_2xx;
        wv->status_3xx = local_shm.status_3xx;
        wv->status_4xx = local_shm.status_4xx;
        wv->status_5xx = local_shm.status_5xx;
        wv->status_other = local_shm.status_other;
        wv->total_bytes = local_shm.total_bytes;
        wv->malformed_lines = local_shm.malformed_lines;

        out->total_requests += local_shm.total_requests;
        out->total_req_per_sec += local_shm.req_per_sec;
        out->status_2xx += local_shm.status_2xx;
        out->status_3xx += local_shm.status_3xx;
        out->status_4xx += local_shm.status_4xx;
        out->status_5xx += local_shm.status_5xx;
        out->status_other += local_shm.status_other;
        out->total_bytes += local_shm.total_bytes;
        out->malformed_lines += local_shm.malformed_lines;

        merge_top_count(ip_pool, &ip_pool_len, LP_MAX_WORKERS * LP_TOP_N,
                        local_shm.top_ips, local_shm.top_ip_count);
        merge_top_count(url_pool, &url_pool_len, LP_MAX_WORKERS * LP_TOP_N,
                        local_shm.top_urls, local_shm.top_url_count);
        merge_top_slow(slow_pool, &slow_pool_len, LP_MAX_WORKERS * LP_TOP_N,
                       local_shm.top_slow_urls, local_shm.top_slow_count);
    }

    if (ip_pool_len > 0) {
        qsort(ip_pool, ip_pool_len, sizeof(ip_pool[0]), cmp_top_count_desc);
        uint32_t n = (ip_pool_len > LP_TOP_N) ? LP_TOP_N : (uint32_t)ip_pool_len;
        out->top_ip_count = n;
        for (uint32_t i = 0; i < n; ++i) {
            out->top_ips[i] = ip_pool[i];
        }
    }

    if (url_pool_len > 0) {
        qsort(url_pool, url_pool_len, sizeof(url_pool[0]), cmp_top_count_desc);
        uint32_t n = (url_pool_len > LP_TOP_N) ? LP_TOP_N : (uint32_t)url_pool_len;
        out->top_url_count = n;
        for (uint32_t i = 0; i < n; ++i) {
            out->top_urls[i] = url_pool[i];
        }
    }

    if (slow_pool_len > 0) {
        qsort(slow_pool, slow_pool_len, sizeof(slow_pool[0]), cmp_top_slow_desc);
        uint32_t n = (slow_pool_len > LP_TOP_N) ? LP_TOP_N : (uint32_t)slow_pool_len;
        out->top_slow_count = n;
        for (uint32_t i = 0; i < n; ++i) {
            out->top_slow_urls[i] = slow_pool[i];
        }
    }
}

static void csv_write_escaped(FILE *fp, const char *s) {
    fputc('"', fp);
    while (*s) {
        if (*s == '"') {
            fputc('"', fp);
        }
        fputc(*s++, fp);
    }
    fputc('"', fp);
}

int coordinator_export_csv(coordinator_t *coord, const char *csv_path) {
    consolidated_metrics_t m;
    coordinator_get_consolidated(coord, &m);

    FILE *fp = fopen(csv_path, "w");
    if (!fp) {
        coordinator_push_event_log(coord, "Error exportando CSV (%s): %s", csv_path, strerror(errno));
        return -1;
    }

    fprintf(fp, "# Resumen Consolidado\n");
    fprintf(fp, "metrica,valor\n");
    fprintf(fp, "modo,%s\n", m.mode == LP_MODE_LOTE ? "lote" : "vivo");
    fprintf(fp, "trabajadores,%d\n", m.num_workers);
    fprintf(fp, "hilos_por_trabajador,%d\n", m.threads_per_worker);
    fprintf(fp, "tiempo_transcurrido_seg,%.2f\n", m.elapsed_sec);
    fprintf(fp, "total_peticiones,%llu\n", (unsigned long long)m.total_requests);
    fprintf(fp, "req_por_seg,%.2f\n", m.total_req_per_sec);
    fprintf(fp, "http_2xx,%llu\n", (unsigned long long)m.status_2xx);
    fprintf(fp, "http_3xx,%llu\n", (unsigned long long)m.status_3xx);
    fprintf(fp, "http_4xx,%llu\n", (unsigned long long)m.status_4xx);
    fprintf(fp, "http_5xx,%llu\n", (unsigned long long)m.status_5xx);
    fprintf(fp, "http_otros,%llu\n", (unsigned long long)m.status_other);
    fprintf(fp, "bytes_transferidos,%llu\n", (unsigned long long)m.total_bytes);
    fprintf(fp, "lineas_malformadas,%llu\n", (unsigned long long)m.malformed_lines);

    fprintf(fp, "\n# Trabajadores\n");
    fprintf(fp, "id,pid,estado,archivo,peticiones,req_s,2xx,3xx,4xx,5xx,bytes,malformadas\n");
    for (int i = 0; i < m.num_workers; ++i) {
        const worker_view_t *w = &m.workers[i];
        fprintf(fp, "%d,%d,%s,", i, (int)w->pid, worker_state_str(w->state));
        csv_write_escaped(fp, w->filepath);
        fprintf(fp, ",%llu,%.2f,%llu,%llu,%llu,%llu,%llu,%llu\n",
                (unsigned long long)w->total_requests,
                w->req_per_sec,
                (unsigned long long)w->status_2xx,
                (unsigned long long)w->status_3xx,
                (unsigned long long)w->status_4xx,
                (unsigned long long)w->status_5xx,
                (unsigned long long)w->total_bytes,
                (unsigned long long)w->malformed_lines);
    }

    fprintf(fp, "\n# Top 10 IPs\n");
    fprintf(fp, "rank,ip,peticiones\n");
    for (uint32_t i = 0; i < m.top_ip_count; ++i) {
        fprintf(fp, "%u,", i + 1);
        csv_write_escaped(fp, m.top_ips[i].key);
        fprintf(fp, ",%llu\n", (unsigned long long)m.top_ips[i].count);
    }

    fprintf(fp, "\n# Top 10 URLs mas solicitadas\n");
    fprintf(fp, "rank,url,peticiones\n");
    for (uint32_t i = 0; i < m.top_url_count; ++i) {
        fprintf(fp, "%u,", i + 1);
        csv_write_escaped(fp, m.top_urls[i].key);
        fprintf(fp, ",%llu\n", (unsigned long long)m.top_urls[i].count);
    }

    fprintf(fp, "\n# Top 10 URLs mas lentas\n");
    fprintf(fp, "rank,url,peticiones,promedio_ms,max_ms\n");
    for (uint32_t i = 0; i < m.top_slow_count; ++i) {
        fprintf(fp, "%u,", i + 1);
        csv_write_escaped(fp, m.top_slow_urls[i].url);
        fprintf(fp, ",%llu,%.2f,%.2f\n",
                (unsigned long long)m.top_slow_urls[i].count,
                m.top_slow_urls[i].avg_time_ms,
                m.top_slow_urls[i].max_time_ms);
    }

    fclose(fp);
    coordinator_push_event_log(coord, "Metricas exportadas a %s", csv_path);
    return 0;
}

static void handle_status_msg(coordinator_t *coord, const status_msg_t *msg) {
    int idx = msg->worker_idx;
    if (idx < 0 || idx >= coord->num_workers) {
        return;
    }

    pthread_mutex_lock(&coord->state_mtx);
    coord_worker_t *w = &coord->workers[idx];
    switch (msg->event) {
        case EVT_INICIO:
            if (w->state == WORKER_STARTING) {
                w->state = WORKER_ACTIVE;
            }
            snprintf(w->last_msg, sizeof(w->last_msg), "INICIO");
            push_event_log_locked(coord, "W%d (PID %d): INICIO", idx, (int)msg->pid);
            break;
        case EVT_FIN:
            if (w->state != WORKER_CRASHED && w->state != WORKER_ERROR) {
                w->state = WORKER_FINISHED;
            }
            snprintf(w->last_msg, sizeof(w->last_msg), "FIN");
            break;
        case EVT_ERROR:
            w->state = WORKER_ERROR;
            snprintf(w->last_msg, sizeof(w->last_msg), "ERROR: %s", msg->detail);
            break;
        case EVT_PAUSA:
            if (w->alive) {
                w->state = WORKER_PAUSED;
            }
            snprintf(w->last_msg, sizeof(w->last_msg), "PAUSADO");
            break;
        case EVT_REANUDA:
            if (w->alive) {
                w->state = WORKER_ACTIVE;
            }
            snprintf(w->last_msg, sizeof(w->last_msg), "ACTIVO");
            break;
    }
    pthread_mutex_unlock(&coord->state_mtx);

    if (msg->event == EVT_ERROR) {
        coordinator_push_event_log(coord, "W%d (PID %d): ERROR - %s",
                                   idx, (int)msg->pid, msg->detail);
    } else if (msg->event == EVT_FIN) {
        coordinator_push_event_log(coord, "W%d (PID %d): FIN de lectura",
                                   idx, (int)msg->pid);
    } else {
        coordinator_wake_ui(coord);
    }
}

static void *status_thread_fn(void *arg) {
    coordinator_t *coord = (coordinator_t *)arg;

    unsigned char buf[LP_MAX_WORKERS][sizeof(status_msg_t)];
    size_t buf_off[LP_MAX_WORKERS];
    int pipe_open[LP_MAX_WORKERS];

    memset(buf, 0, sizeof(buf));
    memset(buf_off, 0, sizeof(buf_off));
    for (int i = 0; i < coord->num_workers; ++i) {
        pipe_open[i] = (coord->workers[i].status_pipe_rd >= 0) ? 1 : 0;
    }

    for (;;) {
        struct pollfd pfds[1 + LP_MAX_WORKERS];
        int pfd_worker[1 + LP_MAX_WORKERS];
        nfds_t nfds = 0;

        pfds[nfds].fd = coord->status_wake_pipe[0];
        pfds[nfds].events = POLLIN;
        pfd_worker[nfds] = -1;
        nfds++;

        for (int i = 0; i < coord->num_workers; ++i) {
            if (pipe_open[i]) {
                pfds[nfds].fd = coord->workers[i].status_pipe_rd;
                pfds[nfds].events = POLLIN;
                pfd_worker[nfds] = i;
                nfds++;
            }
        }

        int pr = poll(pfds, nfds, -1);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (pfds[0].revents & POLLIN) {
            char tmp[32];
            while (read(coord->status_wake_pipe[0], tmp, sizeof(tmp)) > 0) {
            }
            pthread_mutex_lock(&coord->state_mtx);
            int stopping = coord->stop_requested;
            pthread_mutex_unlock(&coord->state_mtx);
            if (stopping) {
                break;
            }
        }

        for (nfds_t k = 1; k < nfds; ++k) {
            int wi = pfd_worker[k];
            if (pfds[k].revents & POLLIN) {
                for (;;) {
                    size_t need = sizeof(status_msg_t) - buf_off[wi];
                    ssize_t r = read(pfds[k].fd, buf[wi] + buf_off[wi], need);
                    if (r > 0) {
                        buf_off[wi] += (size_t)r;
                        if (buf_off[wi] == sizeof(status_msg_t)) {
                            status_msg_t msg;
                            memcpy(&msg, buf[wi], sizeof(msg));
                            buf_off[wi] = 0;
                            handle_status_msg(coord, &msg);
                        }
                    } else if (r == 0) {
                        pipe_open[wi] = 0;
                        break;
                    } else {
                        if (errno == EINTR) {
                            continue;
                        }
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            pipe_open[wi] = 0;
                        }
                        break;
                    }
                }
            } else if (pfds[k].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                pipe_open[wi] = 0;
            }
        }
    }

    return NULL;
}

static void reap_children_nonblock(coordinator_t *coord) {
    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) {
            break;
        }

        int found_idx = -1;
        pthread_mutex_lock(&coord->state_mtx);
        int stopping = coord->stop_requested;
        for (int i = 0; i < coord->num_workers; ++i) {
            if (coord->workers[i].pid == pid) {
                found_idx = i;
                coord->workers[i].alive = 0;
                if (WIFEXITED(status)) {
                    int code = WEXITSTATUS(status);
                    coord->workers[i].exit_code = code;
                    if (code == 0) {
                        if (coord->workers[i].state != WORKER_ERROR) {
                            coord->workers[i].state = WORKER_FINISHED;
                            snprintf(coord->workers[i].last_msg,
                                     sizeof(coord->workers[i].last_msg),
                                     "Finalizado (exit 0)");
                        }
                    } else {
                        coord->workers[i].state = WORKER_ERROR;
                        snprintf(coord->workers[i].last_msg,
                                 sizeof(coord->workers[i].last_msg),
                                 "Error (exit %d)", code);
                    }
                } else if (WIFSIGNALED(status)) {
                    int sig = WTERMSIG(status);
                    coord->workers[i].term_sig = sig;
                    if (!stopping) {
                        coord->workers[i].state = WORKER_CRASHED;
                        snprintf(coord->workers[i].last_msg,
                                 sizeof(coord->workers[i].last_msg),
                                 "CAIDO (sig %d)", sig);
                    } else {
                        coord->workers[i].state = WORKER_FINISHED;
                        snprintf(coord->workers[i].last_msg,
                                 sizeof(coord->workers[i].last_msg),
                                 "Detenido (sig %d)", sig);
                    }
                }
                break;
            }
        }
        pthread_mutex_unlock(&coord->state_mtx);

        if (found_idx >= 0) {
            if (WIFSIGNALED(status) && !stopping) {
                int sig = WTERMSIG(status);
                coordinator_push_event_log(coord,
                    "ALERTA: W%d (PID %d) murio inesperadamente (senal %d: %s)",
                    found_idx, (int)pid, sig, strsignal(sig));
            } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                coordinator_push_event_log(coord,
                    "W%d (PID %d) termino con error (codigo %d)",
                    found_idx, (int)pid, WEXITSTATUS(status));
            } else {
                coordinator_wake_ui(coord);
            }
        }
    }
}

int coordinator_run(int num_files, char **files, lp_mode_t mode, int threads_per_worker) {
    if (num_files <= 0 || num_files > LP_MAX_WORKERS) {
        fprintf(stderr, "Error: numero de archivos debe estar entre 1 y %d\n", LP_MAX_WORKERS);
        return 1;
    }

    coordinator_t coord;
    memset(&coord, 0, sizeof(coord));
    coord.num_workers = num_files;
    coord.mode = mode;
    coord.threads_per_worker = threads_per_worker;
    clock_gettime(CLOCK_MONOTONIC, &coord.start_ts);

    coord.sig_pipe[0] = -1;
    coord.sig_pipe[1] = -1;
    coord.status_wake_pipe[0] = -1;
    coord.status_wake_pipe[1] = -1;
    coord.ui_wake_pipe[0] = -1;
    coord.ui_wake_pipe[1] = -1;
    for (int i = 0; i < LP_MAX_WORKERS; ++i) {
        coord.workers[i].status_pipe_rd = -1;
        coord.workers[i].ctrl_pipe_wr = -1;
    }

    if (pthread_mutex_init(&coord.state_mtx, NULL) != 0) {
        perror("pthread_mutex_init");
        return 1;
    }

    if (pipe(coord.sig_pipe) != 0 ||
        pipe(coord.status_wake_pipe) != 0 ||
        pipe(coord.ui_wake_pipe) != 0) {
        perror("pipe");
        pthread_mutex_destroy(&coord.state_mtx);
        return 1;
    }
    set_nonblock_cloexec(coord.sig_pipe[0]);
    set_nonblock_cloexec(coord.sig_pipe[1]);
    set_nonblock_cloexec(coord.status_wake_pipe[0]);
    set_nonblock_cloexec(coord.status_wake_pipe[1]);
    set_nonblock_cloexec(coord.ui_wake_pipe[0]);
    set_nonblock_cloexec(coord.ui_wake_pipe[1]);

    /* Create POSIX shared memory segment in /dev/shm */
    snprintf(coord.shm_name, sizeof(coord.shm_name), "/logpulse_%d", (int)getpid());
    shm_unlink(coord.shm_name);
    int shm_fd = shm_open(coord.shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (shm_fd < 0) {
        perror("shm_open");
        close(coord.sig_pipe[0]); close(coord.sig_pipe[1]);
        close(coord.status_wake_pipe[0]); close(coord.status_wake_pipe[1]);
        close(coord.ui_wake_pipe[0]); close(coord.ui_wake_pipe[1]);
        pthread_mutex_destroy(&coord.state_mtx);
        return 1;
    }

    if (ftruncate(shm_fd, (off_t)sizeof(shm_layout_t)) != 0) {
        perror("ftruncate");
        close(shm_fd);
        shm_unlink(coord.shm_name);
        close(coord.sig_pipe[0]); close(coord.sig_pipe[1]);
        close(coord.status_wake_pipe[0]); close(coord.status_wake_pipe[1]);
        close(coord.ui_wake_pipe[0]); close(coord.ui_wake_pipe[1]);
        pthread_mutex_destroy(&coord.state_mtx);
        return 1;
    }

    coord.shm = mmap(NULL, sizeof(shm_layout_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    close(shm_fd);
    if (coord.shm == MAP_FAILED) {
        perror("mmap");
        coord.shm = NULL;
        shm_unlink(coord.shm_name);
        close(coord.sig_pipe[0]); close(coord.sig_pipe[1]);
        close(coord.status_wake_pipe[0]); close(coord.status_wake_pipe[1]);
        close(coord.ui_wake_pipe[0]); close(coord.ui_wake_pipe[1]);
        pthread_mutex_destroy(&coord.state_mtx);
        return 1;
    }

    memset(coord.shm, 0, sizeof(shm_layout_t));
    coord.shm->mode = mode;
    coord.shm->threads_per_worker = threads_per_worker;
    coord.shm->num_workers = num_files;

    if (sem_init(&coord.shm->ctrl_sem, 1, 1) != 0) {
        perror("sem_init ctrl_sem");
        munmap(coord.shm, sizeof(shm_layout_t));
        shm_unlink(coord.shm_name);
        pthread_mutex_destroy(&coord.state_mtx);
        return 1;
    }
    for (int i = 0; i < num_files; ++i) {
        if (sem_init(&coord.shm->workers[i].sem, 1, 1) != 0) {
            perror("sem_init worker");
            for (int j = 0; j < i; ++j) {
                sem_destroy(&coord.shm->workers[j].sem);
            }
            sem_destroy(&coord.shm->ctrl_sem);
            munmap(coord.shm, sizeof(shm_layout_t));
            shm_unlink(coord.shm_name);
            pthread_mutex_destroy(&coord.state_mtx);
            return 1;
        }
        snprintf(coord.shm->workers[i].filepath, sizeof(coord.shm->workers[i].filepath),
                 "%s", files[i]);
        coord.shm->workers[i].state = WORKER_STARTING;
    }

    /* Create pipes and fork workers while coordinator is single-threaded */
    for (int i = 0; i < num_files; ++i) {
        int status_pipe[2] = {-1, -1};
        int ctrl_pipe[2] = {-1, -1};
        if (pipe(status_pipe) != 0 || pipe(ctrl_pipe) != 0) {
            perror("pipe worker");
            if (status_pipe[0] >= 0) { close(status_pipe[0]); close(status_pipe[1]); }
            if (ctrl_pipe[0] >= 0) { close(ctrl_pipe[0]); close(ctrl_pipe[1]); }
            coord.workers[i].state = WORKER_ERROR;
            snprintf(coord.workers[i].last_msg, sizeof(coord.workers[i].last_msg),
                     "Error en pipe()");
            continue;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            close(status_pipe[0]); close(status_pipe[1]);
            close(ctrl_pipe[0]); close(ctrl_pipe[1]);
            coord.workers[i].state = WORKER_ERROR;
            snprintf(coord.workers[i].last_msg, sizeof(coord.workers[i].last_msg),
                     "Error en fork()");
            continue;
        }

        if (pid == 0) {
            /* Child worker process */
            close(coord.sig_pipe[0]); close(coord.sig_pipe[1]);
            close(coord.status_wake_pipe[0]); close(coord.status_wake_pipe[1]);
            close(coord.ui_wake_pipe[0]); close(coord.ui_wake_pipe[1]);
            for (int j = 0; j < i; ++j) {
                if (coord.workers[j].status_pipe_rd >= 0) {
                    close(coord.workers[j].status_pipe_rd);
                }
                if (coord.workers[j].ctrl_pipe_wr >= 0) {
                    close(coord.workers[j].ctrl_pipe_wr);
                }
            }
            close(status_pipe[0]);
            close(ctrl_pipe[1]);

            pthread_mutex_destroy(&coord.state_mtx);

            int rc = worker_run(i, files[i], mode, threads_per_worker,
                                coord.shm, status_pipe[1], ctrl_pipe[0]);
            exit(rc);
        }

        /* Parent coordinator */
        close(status_pipe[1]);
        close(ctrl_pipe[0]);
        set_nonblock_cloexec(status_pipe[0]);
        set_nonblock_cloexec(ctrl_pipe[1]);

        coord.workers[i].pid = pid;
        coord.workers[i].status_pipe_rd = status_pipe[0];
        coord.workers[i].ctrl_pipe_wr = ctrl_pipe[1];
        coord.workers[i].alive = 1;
        coord.workers[i].state = WORKER_STARTING;
        snprintf(coord.workers[i].filepath, sizeof(coord.workers[i].filepath), "%s", files[i]);
        snprintf(coord.workers[i].last_msg, sizeof(coord.workers[i].last_msg), "Iniciado");
        coord.shm->workers[i].pid = pid;
    }

    /* Install signal handlers via sigaction */
    g_coord_sig_pipe_wr = coord.sig_pipe[1];

    struct sigaction sa_ign;
    memset(&sa_ign, 0, sizeof(sa_ign));
    sa_ign.sa_handler = SIG_IGN;
    sigemptyset(&sa_ign.sa_mask);
    sigaction(SIGPIPE, &sa_ign, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = coord_sig_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    /* Start status thread and ncurses UI thread */
    pthread_t status_tid;
    pthread_t ui_tid;
    int status_started = (pthread_create(&status_tid, NULL, status_thread_fn, &coord) == 0);
    int ui_started = (pthread_create(&ui_tid, NULL, ui_thread_fn, &coord) == 0);

    /* Main coordinator loop: handles SIGCHLD reaping and shutdown triggers via poll() */
    int running = 1;
    while (running) {
        struct pollfd pfd;
        pfd.fd = coord.sig_pipe[0];
        pfd.events = POLLIN;

        int pr = poll(&pfd, 1, -1);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (pfd.revents & POLLIN) {
            char ch;
            while (read(coord.sig_pipe[0], &ch, 1) > 0) {
                if (ch == 'C') {
                    reap_children_nonblock(&coord);
                } else if (ch == 'I' || ch == 'T' || ch == 'Q') {
                    running = 0;
                }
            }
        }
    }

    /* Orderly shutdown sequence */
    pthread_mutex_lock(&coord.state_mtx);
    coord.stop_requested = 1;
    coord.ui_stop = 1;
    pthread_mutex_unlock(&coord.state_mtx);

    if (lp_sem_wait(&coord.shm->ctrl_sem) == 0) {
        coord.shm->shutdown_requested = 1;
        coord.shm->paused = 0;
        sem_post(&coord.shm->ctrl_sem);
    }

    /* Ask all living workers to quit cleanly and wait for them */
    for (int i = 0; i < num_files; ++i) {
        pid_t wpid = 0;
        int w_alive = 0;
        int ctrl_fd = -1;

        pthread_mutex_lock(&coord.state_mtx);
        w_alive = coord.workers[i].alive;
        wpid = coord.workers[i].pid;
        ctrl_fd = coord.workers[i].ctrl_pipe_wr;
        coord.workers[i].ctrl_pipe_wr = -1;
        pthread_mutex_unlock(&coord.state_mtx);

        if (ctrl_fd >= 0) {
            safe_write_byte(ctrl_fd, LP_CTRL_QUIT);
            close(ctrl_fd);
        }
        if (w_alive && wpid > 0) {
            kill(wpid, SIGTERM);
        }
    }

    for (int i = 0; i < num_files; ++i) {
        pid_t wpid = 0;
        int w_alive = 0;

        pthread_mutex_lock(&coord.state_mtx);
        w_alive = coord.workers[i].alive;
        wpid = coord.workers[i].pid;
        pthread_mutex_unlock(&coord.state_mtx);

        if (w_alive && wpid > 0) {
            int st = 0;
            while (waitpid(wpid, &st, 0) < 0) {
                if (errno != EINTR) {
                    break;
                }
            }
            pthread_mutex_lock(&coord.state_mtx);
            coord.workers[i].alive = 0;
            pthread_mutex_unlock(&coord.state_mtx);
        }
    }

    /* Wake and join UI and status threads */
    safe_write_byte(coord.ui_wake_pipe[1], 'Q');
    safe_write_byte(coord.status_wake_pipe[1], 'Q');

    if (ui_started) {
        pthread_join(ui_tid, NULL);
    }
    if (status_started) {
        pthread_join(status_tid, NULL);
    }

    /* Close remaining descriptors */
    g_coord_sig_pipe_wr = -1;
    for (int i = 0; i < num_files; ++i) {
        if (coord.workers[i].status_pipe_rd >= 0) {
            close(coord.workers[i].status_pipe_rd);
        }
        if (coord.workers[i].ctrl_pipe_wr >= 0) {
            close(coord.workers[i].ctrl_pipe_wr);
        }
    }
    close(coord.sig_pipe[0]);
    close(coord.sig_pipe[1]);
    close(coord.status_wake_pipe[0]);
    close(coord.status_wake_pipe[1]);
    close(coord.ui_wake_pipe[0]);
    close(coord.ui_wake_pipe[1]);

    /* Clean up POSIX semaphores and /dev/shm segment */
    for (int i = 0; i < num_files; ++i) {
        sem_destroy(&coord.shm->workers[i].sem);
    }
    sem_destroy(&coord.shm->ctrl_sem);
    munmap(coord.shm, sizeof(shm_layout_t));
    shm_unlink(coord.shm_name);

    pthread_mutex_destroy(&coord.state_mtx);
    return 0;
}
