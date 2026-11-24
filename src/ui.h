#ifndef LOGPULSE_UI_H
#define LOGPULSE_UI_H

#include "coordinator.h"

void *ui_thread_fn(void *arg);
void ui_install_winch_handler(coordinator_t *coord);

#endif /* LOGPULSE_UI_H */
