#ifndef FULMEN_SHELL_H
#define FULMEN_SHELL_H

#include "types.h"

void shell_run(void);
void shell_exec_line(const char *line);
bool shell_is_poweroff_requested(void);
void shell_request_poweroff(void);

/* Command groups */
bool cmd_linux_dispatch(int argc, char **argv);
bool cmd_demos_dispatch(int argc, char **argv);
void cmd_nano(int argc, char **argv);
void cmd_gui(int argc, char **argv);
void shell_print_help(void);

#endif /* FULMEN_SHELL_H */
