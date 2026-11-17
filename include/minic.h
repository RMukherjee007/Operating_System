#ifndef FULMEN_MINIC_H
#define FULMEN_MINIC_H

#include "types.h"

int minic_run_file(const char *path, int argc, char **argv);
int minic_eval_source(const char *source, const char *filename, int argc, char **argv);
int minic_compile_command(int argc, char **argv, bool is_cpp);

#endif /* FULMEN_MINIC_H */
