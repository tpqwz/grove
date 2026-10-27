#ifndef GROVE_FTS_TOK_H
#define GROVE_FTS_TOK_H

#include "util.h"

#include "sqlite3.h"

/* Register the grove tokenizer for this connection. Required before any
 * statement that creates or queries the FTS4 table. */
int grove_fts_register(sqlite3 *db, Err *e);

#endif
