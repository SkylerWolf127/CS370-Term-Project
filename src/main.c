#include <stdio.h>
#include <string.h>

#include "config.h"
#include "exit_codes.h"
#include "menu.h"
#include "monitor.h"
#include "sig.h"

int main(void)
{
    /* No explicit heap use anywhere: images go in a fixed static buffer. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (sig_install() != 0) {
        perror("plantsense: sigaction");
        return PS_EXIT_IO;
    }

    struct config cfg;
    struct soil_config soil;
    char why[160] = "";
    if (config_load_all(CONFIG_PATH, &cfg, &soil, why, sizeof why) != CONFIG_OK) {
        fprintf(stderr, "plantsense: %s: %s\n"
                        "Fix the file or delete it to use defaults.\n",
                CONFIG_PATH, why);
        return PS_EXIT_CONFIG;
    }

    struct monitor_state mon;
    monitor_init(&mon);
    int code = menu_run(&cfg, &soil, &mon);
    if (monitor_close(&mon) != 0) {
        fprintf(stderr, "plantsense: closing session log failed\n");
        if (code == PS_EXIT_OK)
            code = PS_EXIT_IO;
    }
    return code;
}
