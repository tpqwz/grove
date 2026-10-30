CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 \
	-fstack-protector-strong -D_FORTIFY_SOURCE=2 \
	-Wformat=2 -Wno-format-nonliteral -Wnull-dereference -Wshadow
LIBS = -lz -lm -pthread -ldl -lX11
LDFLAGS = -Wl,-z,relro,-z,now

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

.PHONY: all clean live-mod

LIVE_DIR = live
MOD = $(LIVE_DIR)/module.so

all: $(ROOT)/grove-ui $(MOD)

$(WEBUI_LIB):
	@test -d $(WEBUI)/.git || git clone --depth 1 https://github.com/webui-dev/webui.git $(WEBUI)
	$(MAKE) -C $(WEBUI)

$(SQLITE_OBJ): $(ROOT)/third_party/sqlite/sqlite3.c
	$(CC) $(SQLITE_CFLAGS) -c -o $@ $(ROOT)/third_party/sqlite/sqlite3.c

page.h: page.html pack.py
	python3 pack.py page.html page.h

$(MOD): main.c page.h mod.h
	mkdir -p $(LIVE_DIR)
	$(CC) $(CFLAGS) -fPIC -shared -DGROVE_UI_MODULE $(INCLUDES) -o $@ main.c -pthread

$(ROOT)/grove-ui: host.c window_icon.c window_icon.h mod.h $(WEBUI_LIB) $(CORE) $(SQLITE_OBJ) $(ROOT)/src/main.c $(ROOT)/src/cli.h
	$(CC) $(CFLAGS) -DGROVE_EMBED $(INCLUDES) -o $@ host.c window_icon.c $(ROOT)/src/main.c $(CORE) $(SQLITE_OBJ) \
		-Wl,--whole-archive $(WEBUI_LIB) -Wl,--no-whole-archive \
		$(LIBS) -Wl,--export-dynamic $(LDFLAGS)

# OUT is an absolute path. The running host calls this when main.c is saved.
live-mod:
	@test -n "$(OUT)"
	@mkdir -p $(dir $(OUT))
	python3 pack.py page.html page.h
	$(CC) $(CFLAGS) -fPIC -shared -DGROVE_UI_MODULE $(INCLUDES) -o $(OUT) main.c -pthread

clean:
	rm -f $(ROOT)/grove-ui page.h
	rm -rf $(LIVE_DIR)
