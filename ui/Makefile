CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2
LIBS = -lz -lm -pthread -ldl

ROOT = ..
CORE = $(ROOT)/src/util.c $(ROOT)/src/text.c $(ROOT)/src/zip.c $(ROOT)/src/pdf.c \
	$(ROOT)/src/extract.c $(ROOT)/src/index.c $(ROOT)/src/fts_tok.c
WEBUI = third_party/webui
WEBUI_LIB = $(WEBUI)/dist/libwebui-2-static.a
INCLUDES = -I$(ROOT)/src -I$(ROOT)/third_party/sqlite -I$(WEBUI)/include

SQLITE_CFLAGS = -std=c11 -O2 -DNDEBUG \
	-DSQLITE_ENABLE_FTS4 -DSQLITE_ENABLE_FTS3_PARENTHESIS \
	-DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=1 \
	-DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0
SQLITE_OBJ = $(ROOT)/third_party/sqlite/sqlite3.o

.PHONY: all clean

all: $(ROOT)/grove-ui

$(WEBUI_LIB):
	@test -d $(WEBUI)/.git || git clone --depth 1 https://github.com/webui-dev/webui.git $(WEBUI)
	$(MAKE) -C $(WEBUI)

$(SQLITE_OBJ): $(ROOT)/third_party/sqlite/sqlite3.c
	$(CC) $(SQLITE_CFLAGS) -c -o $@ $(ROOT)/third_party/sqlite/sqlite3.c

page.h: page.html pack.py
	python3 pack.py page.html page.h

$(ROOT)/grove-ui: main.c page.h $(WEBUI_LIB) $(CORE) $(SQLITE_OBJ)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ main.c $(CORE) $(SQLITE_OBJ) $(WEBUI_LIB) $(LIBS)

clean:
	rm -f $(ROOT)/grove-ui page.h
