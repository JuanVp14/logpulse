#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "coordinator.h"
#include "shm_ipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_usage(const char *prog) {
    fprintf(stderr,
            "Uso: %s [-m lote|vivo] [-t N] archivo1 [archivo2 ...]\n"
            "  -m lote|vivo   Modo de lectura (por defecto: lote)\n"
            "  -t N           Hilos parseadores por trabajador (%d a %d, por defecto %d)\n"
            "  -h             Mostrar esta ayuda\n",
            prog, LP_MIN_THREADS, LP_MAX_THREADS, LP_DEFAULT_THREADS);
}

int main(int argc, char **argv) {
    lp_mode_t mode = LP_MODE_LOTE;
    int threads_per_worker = LP_DEFAULT_THREADS;

    int opt;
    while ((opt = getopt(argc, argv, "m:t:h")) != -1) {
        switch (opt) {
            case 'm':
                if (strcmp(optarg, "lote") == 0) {
                    mode = LP_MODE_LOTE;
                } else if (strcmp(optarg, "vivo") == 0) {
                    mode = LP_MODE_VIVO;
                } else {
                    fprintf(stderr, "Error: modo invalido '%s' (use 'lote' o 'vivo').\n", optarg);
                    print_usage(argv[0]);
                    return 1;
                }
                break;
            case 't': {
                char *endptr = NULL;
                errno = 0;
                long val = strtol(optarg, &endptr, 10);
                if (errno != 0 || endptr == optarg || *endptr != '\0' ||
                    val < LP_MIN_THREADS || val > LP_MAX_THREADS) {
                    fprintf(stderr, "Error: -t debe estar entre %d y %d.\n",
                            LP_MIN_THREADS, LP_MAX_THREADS);
                    print_usage(argv[0]);
                    return 1;
                }
                threads_per_worker = (int)val;
                break;
            }
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    int num_files = argc - optind;
    if (num_files <= 0) {
        fprintf(stderr, "Error: debe especificar al menos un archivo de log.\n");
        print_usage(argv[0]);
        return 1;
    }
    if (num_files > LP_MAX_WORKERS) {
        fprintf(stderr, "Error: maximo %d archivos simultaneos soportados.\n", LP_MAX_WORKERS);
        return 1;
    }

    return coordinator_run(num_files, &argv[optind], mode, threads_per_worker);
}
