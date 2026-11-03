UNAME_S := $(shell uname -s)
PLATFORM := linux
ifneq (,$(findstring Darwin,$(UNAME_S)))
PLATFORM := macos
endif
ifneq (,$(findstring MINGW,$(UNAME_S)))
PLATFORM := windows
endif
ifneq (,$(findstring MSYS,$(UNAME_S)))
PLATFORM := windows
endif

CC = gcc
ifeq ($(PLATFORM),macos)
CC = clang
endif
ifeq ($(PLATFORM),windows)
CC = gcc
# g++ compiles .c as C++ when it is also the linker. gcc links the C++ objects.
LINK = $(CC)
GROVE = grove.exe
else
LINK = $(CC)
GROVE = grove
endif

CFLAGS = -std=c11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 \
	-fstack-protector-strong \
	-Wformat=2 -Wno-format-nonliteral -Wnull-dereference -Wshadow
ifeq ($(PLATFORM),linux)
CFLAGS += -D_FORTIFY_SOURCE=2
LDFLAGS = -Wl,-z,relro,-z,now
LIBS = -Wl,-Bstatic -lz -Wl,-Bdynamic -lm -pthread -ldl
WEBUI_LINK = -Wl,--whole-archive ui/third_party/webui/dist/libwebui-2-static.a -Wl,--no-whole-archive
WEBUI_EXTRA =
EXPORT = -rdynamic
ICON_SRC = ui/window_icon.c
else ifeq ($(PLATFORM),macos)
LDFLAGS =
LIBS = -lz -lm -pthread
WEBUI_LINK = -Wl,-force_load,ui/third_party/webui/dist/libwebui-2-static.a
WEBUI_EXTRA = -framework Cocoa -framework WebKit
EXPORT = -rdynamic
ICON_SRC =
else
CFLAGS += -D__USE_MINGW_ANSI_STDIO=1
LDFLAGS = -static -static-libgcc -static-libstdc++
LIBS = -Wl,-Bstatic -lz -lssp -lwinpthread
WEBUI_LINK = -Wl,--whole-archive ui/third_party/webui/dist/libwebui-2-static.a -Wl,--no-whole-archive
WEBUI_EXTRA = -lws2_32 -luser32 -lole32 -loleaut32 -luuid -lshell32 -lgdi32 -lstdc++
EXPORT =
ICON_SRC =
endif

ifeq ($(PLATFORM),windows)
SAN =
else
SAN = -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
endif

INCLUDES = -Ithird_party/sqlite -Isrc -Iui -Iui/third_party/webui/include

SQLITE_CFLAGS = -std=c11 -O2 -DNDEBUG \
	-DSQLITE_ENABLE_FTS4 -DSQLITE_ENABLE_FTS3_PARENTHESIS \
	-DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=1 \
	-DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0
SQLITE_OBJ = third_party/sqlite/sqlite3.o
WEBUI_LIB = ui/third_party/webui/dist/libwebui-2-static.a

CORE = src/util.c src/text.c src/expand.c src/zip.c src/pdf.c src/extract.c src/fetch.c src/persona.c src/index.c src/fts_tok.c src/os.c
SRCS = $(CORE) src/main.c

.PHONY: all test clean ui libwebui

all: $(GROVE)

ui: $(GROVE)

libwebui: $(WEBUI_LIB)

$(WEBUI_LIB):
	$(MAKE) -C ui libwebui

ui/page.h: ui/page.html ui/pack.py
	python3 ui/pack.py ui/page.html ui/page.h

$(SQLITE_OBJ): third_party/sqlite/sqlite3.c
	$(CC) $(SQLITE_CFLAGS) -c -o $@ third_party/sqlite/sqlite3.c

$(GROVE): $(SRCS) ui/host.c ui/main.c $(ICON_SRC) ui/page.h $(SQLITE_OBJ) $(WEBUI_LIB) ui/window_icon.h ui/mod.h src/cli.h
	$(LINK) $(CFLAGS) -DGROVE_EMBED $(INCLUDES) -x c -o $@ \
		ui/host.c ui/main.c $(ICON_SRC) $(SRCS) -x none $(SQLITE_OBJ) \
		$(WEBUI_LINK) $(LIBS) $(WEBUI_EXTRA) $(EXPORT) $(LDFLAGS)

grove-test: $(CORE) src/test.c $(SQLITE_OBJ)
	$(CC) $(CFLAGS) $(SAN) $(INCLUDES) -o $@ $(CORE) src/test.c $(SQLITE_OBJ) $(LIBS) $(LDFLAGS)

ifeq ($(PLATFORM),windows)
test: $(GROVE)
	./$(GROVE) --version
else
test: grove-test $(GROVE)
	./grove-test
	tests/integration.sh
endif

clean:
	rm -f grove grove.exe grove-test src/*.o third_party/sqlite/sqlite3.o
