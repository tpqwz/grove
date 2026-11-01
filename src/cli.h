#ifndef GROVE_CLI_H
#define GROVE_CLI_H

#include <stdio.h>

/* The command-line actions. One program runs these and the window.
 * A command word is index, search, and the rest of the manual.
 * With no command, the program opens the window. */
int grove_is_command(const char *s);
int grove_cli(int argc, char **argv);
void grove_usage(FILE *out);
int grove_open_window(int argc, char **argv);

#endif
