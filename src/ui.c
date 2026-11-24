#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "ui.h"

#include <errno.h>
#include <ncurses.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define COLOR_PAIR_HEADER  1
#define COLOR_PAIR_ACTIVE  2
#define COLOR_PAIR_PAUSED  3
#define COLOR_PAIR_ERROR   4
#define COLOR_PAIR_INFO    5
#define COLOR_PAIR_DIM     6

static volatile sig_atomic_t g_ui_wake_wr = -1;

static void ui_sigwinch_handler(int sig) {
    (void)sig;
    int saved_errno = errno;
    if (g_ui_wake_wr >= 0) {
        char b = 'R';
        ssize_t r = write(g_ui_wake_wr, &b, 1);
        (void)r;
    }
    errno = saved_errno;
}

void ui_install_winch_handler(coordinator_t *coord) {
    g_ui_wake_wr = coord->ui_wake_pipe[1];
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = ui_sigwinch_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &sa, NULL);
}

static void format_bytes(uint64_t bytes, char *buf, size_t cap) {
    if (bytes < 1024ULL) {
        snprintf(buf, cap, "%llu B", (unsigned long long)bytes);
    } else if (bytes < 1024ULL * 1024ULL) {
        snprintf(buf, cap, "%.1f KB", (double)bytes / 1024.0);
    } else if (bytes < 1024ULL * 1024ULL * 1024ULL) {
        snprintf(buf, cap, "%.2f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        snprintf(buf, cap, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    }
}

static int color_for_state(worker_state_t st) {
    switch (st) {
        case WORKER_ACTIVE:   return COLOR_PAIR_ACTIVE;
        case WORKER_PAUSED:   return COLOR_PAIR_PAUSED;
        case WORKER_CRASHED:
        case WORKER_ERROR:    return COLOR_PAIR_ERROR;
        case WORKER_FINISHED: return COLOR_PAIR_INFO;
        default:              return COLOR_PAIR_DIM;
    }
}

static void draw_hline_at(int y, int cols) {
    if (cols <= 0) return;
    move(y, 0);
    hline(ACS_HLINE, cols);
}

static void render_dashboard(const consolidated_metrics_t *m, int has_color, const char *flash_msg) {
    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);
    erase();

    if (rows < 10 || cols < 40) {
        mvaddnstr(0, 0, "Terminal demasiado pequena para LogPulse", cols);
        refresh();
        return;
    }

    int y = 0;
    char line[512];
    char bytes_buf[32];
    format_bytes(m->total_bytes, bytes_buf, sizeof(bytes_buf));

    /* Header bar */
    if (has_color) attron(COLOR_PAIR(COLOR_PAIR_HEADER) | A_BOLD);
    snprintf(line, sizeof(line),
             " LogPulse | Modo: %s | Hilos/trab: %d | Workers: %d | Tiempo: %.1fs | %s ",
             m->mode == LP_MODE_LOTE ? "LOTE" : "VIVO",
             m->threads_per_worker,
             m->num_workers,
             m->elapsed_sec,
             m->paused ? "[PAUSADO]" : "[ACTIVO]");
    mvaddnstr(y, 0, line, cols);
    for (int cx = (int)strlen(line); cx < cols; ++cx) {
        mvaddch(y, cx, ' ');
    }
    if (has_color) attroff(COLOR_PAIR(COLOR_PAIR_HEADER) | A_BOLD);
    y++;

    /* Controls bar */
    if (has_color) attron(COLOR_PAIR(COLOR_PAIR_INFO));
    snprintf(line, sizeof(line),
             " Controles: [q] Salir   [p] Pausar/Reanudar   [e] Exportar CSV%s%s",
             (flash_msg && flash_msg[0]) ? "   | " : "",
             (flash_msg && flash_msg[0]) ? flash_msg : "");
    mvaddnstr(y, 0, line, cols);
    if (has_color) attroff(COLOR_PAIR(COLOR_PAIR_INFO));
    y++;

    draw_hline_at(y++, cols);

    /* Consolidated summary */
    if (y < rows) {
        attron(A_BOLD);
        snprintf(line, sizeof(line),
                 " RESUMEN: Peticiones=%llu | Req/s=%.1f | Datos=%s | Malformadas=%llu",
                 (unsigned long long)m->total_requests,
                 m->total_req_per_sec,
                 bytes_buf,
                 (unsigned long long)m->malformed_lines);
        mvaddnstr(y++, 0, line, cols);
        attroff(A_BOLD);
    }

    if (y < rows) {
        snprintf(line, sizeof(line), " HTTP: ");
        mvaddnstr(y, 0, line, cols);
        int x = 7;

        if (has_color) attron(COLOR_PAIR(COLOR_PAIR_ACTIVE));
        snprintf(line, sizeof(line), "2xx=%llu  ", (unsigned long long)m->status_2xx);
        mvaddnstr(y, x, line, cols - x);
        x += (int)strlen(line);
        if (has_color) attroff(COLOR_PAIR(COLOR_PAIR_ACTIVE));

        if (has_color) attron(COLOR_PAIR(COLOR_PAIR_INFO));
        snprintf(line, sizeof(line), "3xx=%llu  ", (unsigned long long)m->status_3xx);
        if (x < cols) mvaddnstr(y, x, line, cols - x);
        x += (int)strlen(line);
        if (has_color) attroff(COLOR_PAIR(COLOR_PAIR_INFO));

        if (has_color) attron(COLOR_PAIR(COLOR_PAIR_PAUSED));
        snprintf(line, sizeof(line), "4xx=%llu  ", (unsigned long long)m->status_4xx);
        if (x < cols) mvaddnstr(y, x, line, cols - x);
        x += (int)strlen(line);
        if (has_color) attroff(COLOR_PAIR(COLOR_PAIR_PAUSED));

        if (has_color) attron(COLOR_PAIR(COLOR_PAIR_ERROR));
        snprintf(line, sizeof(line), "5xx=%llu  ", (unsigned long long)m->status_5xx);
        if (x < cols) mvaddnstr(y, x, line, cols - x);
        x += (int)strlen(line);
        if (has_color) attroff(COLOR_PAIR(COLOR_PAIR_ERROR));

        snprintf(line, sizeof(line), "Otros=%llu", (unsigned long long)m->status_other);
        if (x < cols) mvaddnstr(y, x, line, cols - x);
        y++;
    }

    draw_hline_at(y++, cols);

    /* Workers table */
    if (y < rows) {
        attron(A_BOLD);
        mvaddnstr(y++, 0, " ID  PID      ESTADO       PETICIONES    REQ/S    MALF    ARCHIVO / ESTADO", cols);
        attroff(A_BOLD);
    }

    int max_worker_rows = rows - y - 14;
    if (max_worker_rows < 2) max_worker_rows = 2;
    int shown_workers = m->num_workers < max_worker_rows ? m->num_workers : max_worker_rows;

    for (int i = 0; i < shown_workers && y < rows - 2; ++i) {
        const worker_view_t *w = &m->workers[i];
        int cp = color_for_state(w->state);

        snprintf(line, sizeof(line), " W%-2d %-8d ", i, (int)w->pid);
        mvaddnstr(y, 0, line, cols);
        int x = (int)strlen(line);

        if (has_color) attron(COLOR_PAIR(cp) | A_BOLD);
        snprintf(line, sizeof(line), "%-12s", worker_state_str(w->state));
        if (x < cols) mvaddnstr(y, x, line, cols - x);
        x += (int)strlen(line);
        if (has_color) attroff(COLOR_PAIR(cp) | A_BOLD);

        snprintf(line, sizeof(line), " %-13llu %-8.1f %-7llu %s",
                 (unsigned long long)w->total_requests,
                 w->req_per_sec,
                 (unsigned long long)w->malformed_lines,
                 w->filepath);
        if (x < cols) mvaddnstr(y, x, line, cols - x);
        y++;
    }
    if (m->num_workers > shown_workers && y < rows - 2) {
        snprintf(line, sizeof(line), " ... (%d trabajadores adicionales)", m->num_workers - shown_workers);
        mvaddnstr(y++, 0, line, cols);
    }

    draw_hline_at(y++, cols);

    /* Top 10 sections */
    int evt_reserve = 5;
    int top_avail = rows - y - evt_reserve;
    if (top_avail >= 4) {
        if (cols >= 115) {
            /* 3 columns side by side */
            int col_w = cols / 3;
            int header_y = y;
            attron(A_BOLD);
            mvaddnstr(header_y, 0, " TOP 10 IPs", col_w - 1);
            mvaddnstr(header_y, col_w, " TOP 10 URLs", col_w - 1);
            mvaddnstr(header_y, col_w * 2, " TOP 10 URLs MAS LENTAS (ms)", cols - col_w * 2);
            attroff(A_BOLD);

            int max_top = top_avail - 1;
            if (max_top > LP_TOP_N) max_top = LP_TOP_N;

            for (int r = 0; r < max_top; ++r) {
                int ry = header_y + 1 + r;
                if (r < (int)m->top_ip_count) {
                    snprintf(line, sizeof(line), " %2d. %-16s (%llu)",
                             r + 1, m->top_ips[r].key,
                             (unsigned long long)m->top_ips[r].count);
                    mvaddnstr(ry, 0, line, col_w - 1);
                }
                if (r < (int)m->top_url_count) {
                    snprintf(line, sizeof(line), " %2d. (%llu) %s",
                             r + 1, (unsigned long long)m->top_urls[r].count,
                             m->top_urls[r].key);
                    mvaddnstr(ry, col_w, line, col_w - 1);
                }
                if (r < (int)m->top_slow_count) {
                    snprintf(line, sizeof(line), " %2d. avg=%.1f max=%.1f %s",
                             r + 1,
                             m->top_slow_urls[r].avg_time_ms,
                             m->top_slow_urls[r].max_time_ms,
                             m->top_slow_urls[r].url);
                    mvaddnstr(ry, col_w * 2, line, cols - col_w * 2);
                }
            }
            y = header_y + 1 + max_top;
        } else {
            /* Stacked compact Top 5/10 for narrow terminals */
            int col_w = cols / 2;
            int header_y = y;
            attron(A_BOLD);
            mvaddnstr(header_y, 0, " TOP IPs", col_w - 1);
            mvaddnstr(header_y, col_w, " TOP URLs LENTAS (avg ms)", cols - col_w);
            attroff(A_BOLD);

            int max_top = top_avail - 1;
            if (max_top > LP_TOP_N) max_top = LP_TOP_N;
            for (int r = 0; r < max_top; ++r) {
                int ry = header_y + 1 + r;
                if (r < (int)m->top_ip_count) {
                    snprintf(line, sizeof(line), " %2d. %-15s (%llu)",
                             r + 1, m->top_ips[r].key,
                             (unsigned long long)m->top_ips[r].count);
                    mvaddnstr(ry, 0, line, col_w - 1);
                }
                if (r < (int)m->top_slow_count) {
                    snprintf(line, sizeof(line), " %2d. %.1fms %s",
                             r + 1, m->top_slow_urls[r].avg_time_ms,
                             m->top_slow_urls[r].url);
                    mvaddnstr(ry, col_w, line, cols - col_w);
                }
            }
            y = header_y + 1 + max_top;
        }
    }

    /* Event log footer */
    if (y < rows - 1) {
        draw_hline_at(y++, cols);
        if (y < rows) {
            attron(A_BOLD);
            mvaddnstr(y++, 0, " EVENTOS DEL COORDINADOR:", cols);
            attroff(A_BOLD);
        }
        int avail_evts = rows - y;
        int start = (m->evt_log_count > avail_evts) ? (m->evt_log_count - avail_evts) : 0;
        for (int i = start; i < m->evt_log_count && y < rows; ++i) {
            snprintf(line, sizeof(line), "  * %s", m->evt_log[i]);
            mvaddnstr(y++, 0, line, cols);
        }
    }

    refresh();
}

void *ui_thread_fn(void *arg) {
    coordinator_t *coord = (coordinator_t *)arg;

    if (initscr() == NULL) {
        coordinator_request_stop(coord);
        return NULL;
    }

    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);

    int has_color = has_colors();
    if (has_color) {
        start_color();
        use_default_colors();
        init_pair(COLOR_PAIR_HEADER, COLOR_BLACK, COLOR_CYAN);
        init_pair(COLOR_PAIR_ACTIVE, COLOR_GREEN, -1);
        init_pair(COLOR_PAIR_PAUSED, COLOR_YELLOW, -1);
        init_pair(COLOR_PAIR_ERROR, COLOR_RED, -1);
        init_pair(COLOR_PAIR_INFO, COLOR_CYAN, -1);
        init_pair(COLOR_PAIR_DIM, COLOR_WHITE, -1);
    }

    ui_install_winch_handler(coord);

    consolidated_metrics_t cached;
    coordinator_get_consolidated(coord, &cached);
    char flash_msg[128] = "";

    render_dashboard(&cached, has_color, flash_msg);

    for (;;) {
        pthread_mutex_lock(&coord->state_mtx);
        int should_stop = coord->ui_stop;
        int is_paused = coord->paused;
        pthread_mutex_unlock(&coord->state_mtx);
        if (should_stop) {
            break;
        }

        struct pollfd pfds[2];
        pfds[0].fd = STDIN_FILENO;
        pfds[0].events = POLLIN;
        pfds[1].fd = coord->ui_wake_pipe[0];
        pfds[1].events = POLLIN;

        int pr = poll(pfds, 2, 1000);
        if (pr < 0 && errno != EINTR) {
            break;
        }

        int need_resize = 0;
        if (pfds[1].revents & POLLIN) {
            char buf[64];
            ssize_t nr;
            while ((nr = read(coord->ui_wake_pipe[0], buf, sizeof(buf))) > 0) {
                for (ssize_t bi = 0; bi < nr; ++bi) {
                    if (buf[bi] == 'R') {
                        need_resize = 1;
                    }
                }
            }
        }

        pthread_mutex_lock(&coord->state_mtx);
        should_stop = coord->ui_stop;
        is_paused = coord->paused;
        pthread_mutex_unlock(&coord->state_mtx);
        if (should_stop) {
            break;
        }

        if (need_resize) {
            struct winsize ws;
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
                resizeterm(ws.ws_row, ws.ws_col);
            }
            clear();
        }

        int quit_pressed = 0;
        if (pfds[0].revents & POLLIN) {
            int ch;
            while ((ch = getch()) != ERR) {
                if (ch == 'q' || ch == 'Q') {
                    quit_pressed = 1;
                    break;
                } else if (ch == 'p' || ch == 'P') {
                    coordinator_toggle_pause(coord);
                    is_paused = !is_paused;
                    snprintf(flash_msg, sizeof(flash_msg), "%s",
                             is_paused ? "Pausado" : "Reanudado");
                } else if (ch == 'e' || ch == 'E') {
                    if (coordinator_export_csv(coord, "metricas_logpulse.csv") == 0) {
                        snprintf(flash_msg, sizeof(flash_msg),
                                 "CSV exportado: metricas_logpulse.csv");
                    } else {
                        snprintf(flash_msg, sizeof(flash_msg),
                                 "Error exportando CSV");
                    }
                } else if (ch == KEY_RESIZE) {
                    struct winsize ws;
                    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
                        resizeterm(ws.ws_row, ws.ws_col);
                    }
                    clear();
                }
            }
        }

        if (quit_pressed) {
            coordinator_request_stop(coord);
            break;
        }

        /* When paused, freeze metric counters while still refreshing worker/event status */
        if (!is_paused) {
            coordinator_get_consolidated(coord, &cached);
        } else {
            consolidated_metrics_t fresh;
            coordinator_get_consolidated(coord, &fresh);
            cached.paused = fresh.paused;
            cached.evt_log_count = fresh.evt_log_count;
            memcpy(cached.evt_log, fresh.evt_log, sizeof(cached.evt_log));
            for (int i = 0; i < fresh.num_workers; ++i) {
                cached.workers[i].state = fresh.workers[i].state;
            }
        }

        render_dashboard(&cached, has_color, flash_msg);
    }

    struct sigaction sa_dfl;
    memset(&sa_dfl, 0, sizeof(sa_dfl));
    sa_dfl.sa_handler = SIG_DFL;
    sigemptyset(&sa_dfl.sa_mask);
    sigaction(SIGWINCH, &sa_dfl, NULL);
    g_ui_wake_wr = -1;
    endwin();
    return NULL;
}
