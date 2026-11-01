CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 \
	-fstack-protector-strong -D_FORTIFY_SOURCE=2 \
	-Wformat=2 -Wno-format-nonliteral -Wnull-dereference -Wshadow

ROOT = ..
WEBUI = third_party/webui
WEBUI_REV = 2fb97ed52a28c6ddc540f78b03e1d00706fc9728
WEBUI_LIB = $(WEBUI)/dist/libwebui-2-static.a
INCLUDES = -I$(ROOT)/src -I$(ROOT)/third_party/sqlite -I$(WEBUI)/include

.PHONY: all libwebui clean live-mod

LIVE_DIR = live
MOD = $(LIVE_DIR)/module.so

all: libwebui

libwebui: $(WEBUI_LIB)

$(WEBUI_LIB):
	@if [ ! -d $(WEBUI)/.git ]; then \
		git clone --depth 1 https://github.com/webui-dev/webui.git $(WEBUI); \
	fi
	@if [ "$$(git -C $(WEBUI) rev-parse HEAD)" != "$(WEBUI_REV)" ]; then \
		git -C $(WEBUI) fetch --depth 1 origin $(WEBUI_REV); \
		git -C $(WEBUI) checkout --detach FETCH_HEAD; \
	fi
	@if [ "$$OS" = "Windows_NT" ] || uname -s | grep -Eq 'MINGW|MSYS'; then \
		mkdir -p $(WEBUI)/.grove-sh; \
		printf '%s\n' '#!/bin/sh' 'rm -f "$$@"' > $(WEBUI)/.grove-sh/del; \
		chmod +x $(WEBUI)/.grove-sh/del; \
		PATH="$(abspath $(WEBUI)/.grove-sh):$$PATH" $(MAKE) -C $(WEBUI) SHELL="$(SHELL)"; \
	else \
		$(MAKE) -C $(WEBUI); \
	fi

page.h: page.html pack.py
	python3 pack.py page.html page.h

$(MOD): main.c page.h mod.h
	mkdir -p $(LIVE_DIR)
	$(CC) $(CFLAGS) -fPIC -shared -DGROVE_UI_MODULE $(INCLUDES) -o $@ main.c -pthread

# OUT is an absolute path. The running host calls this when main.c is saved.
# It writes a new numbered module. It does not replace live/module.so.
live-mod: page.h
	@test -n "$(OUT)"
	@mkdir -p $(dir $(OUT))
	$(CC) $(CFLAGS) -fPIC -shared -DGROVE_UI_MODULE $(INCLUDES) -o $(OUT) main.c -pthread

clean:
	rm -f page.h
