#ifndef PLANTSENSE_MENU_H
#define PLANTSENSE_MENU_H

#include "config.h"
#include "monitor.h"

/* Runs until Exit, EOF, or a signal. Returns a PS_EXIT_* code. */
int menu_run(struct config *cfg, struct monitor_state *mon);

#endif
