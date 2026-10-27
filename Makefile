CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2
SAN = -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
LIBS = -lz -lm -pthread -ldl
INCLUDES = -Ithird_party/sqlite

# sqlite3.c is compiled on its own. -Werror and ASan stay off that file.
SQLITE_CFLAGS = -std=c11 -O2 -DNDEBUG \
	-DSQLITE_ENABLE_FTS4 -DSQLITE_ENABLE_FTS3_PARENTHESIS \
	-DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=1 \
	-DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0
SQLITE_OBJ = third_party/sqlite/sqlite3.o

CORE = src/util.c src/text.c src/zip.c src/pdf.c src/extract.c src/index.c src/fts_tok.c
SRCS = $(CORE) src/main.c

.PHONY: all test clean ui

all: grove

ui:
	$(MAKE) -C ui

$(SQLITE_OBJ): third_party/sqlite/sqlite3.c
	$(CC) $(SQLITE_CFLAGS) -c -o $@ third_party/sqlite/sqlite3.c

grove: $(SRCS) $(SQLITE_OBJ)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(SRCS) $(SQLITE_OBJ) $(LIBS)

test: $(SQLITE_OBJ)
	$(CC) $(CFLAGS) $(SAN) $(INCLUDES) -o grove-test $(CORE) src/test.c $(SQLITE_OBJ) $(LIBS)
	./grove-test
	$(CC) $(CFLAGS) $(INCLUDES) -o grove $(SRCS) $(SQLITE_OBJ) $(LIBS)
	tests/integration.sh

clean:
	rm -f grove grove-test src/*.o
