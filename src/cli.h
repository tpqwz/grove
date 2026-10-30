#ifndef GROVE_CLI_H
#define GROVE_CLI_H

/* The command-line actions. grove and grove-ui both call this.
 * A command word is index, search, and the rest of the manual. */
int grove_is_command(const char *s);
int grove_cli(int argc, char **argv);

#endif
