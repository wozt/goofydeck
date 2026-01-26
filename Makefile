SHELL := /bin/bash

CC ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra

ZLIB_LIBS := -lz
PNG_LIBS := -lpng
PTHREAD_LIBS := -lpthread
MATH_LIBS := -lm
HID_LIBS := -lhidapi-libusb
OPENSSL_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null)
OPENSSL_LIBS := $(shell pkg-config --libs openssl 2>/dev/null)
ifeq ($(strip $(OPENSSL_LIBS)),)
OPENSSL_LIBS := -lssl -lcrypto
endif
YAML_CFLAGS := $(shell pkg-config --cflags yaml-0.1 2>/dev/null)
YAML_LIBS := $(shell pkg-config --libs yaml-0.1 2>/dev/null)
ifeq ($(strip $(YAML_LIBS)),)
YAML_LIBS := -lyaml
endif

FFMPEG_CFLAGS := $(shell pkg-config --cflags libavformat libavcodec libavutil libswscale 2>/dev/null)
FFMPEG_LIBS := $(shell pkg-config --libs libavformat libavcodec libavutil libswscale 2>/dev/null)
ifeq ($(strip $(FFMPEG_LIBS)),)
FFMPEG_LIBS := -lavformat -lavcodec -lavutil -lswscale
endif

MDI_CFLAGS := $(shell pkg-config --cflags cairo librsvg-2.0 2>/dev/null)
MDI_LIBS := $(shell pkg-config --libs cairo librsvg-2.0 2>/dev/null)
HAVE_MDI := 0
ifneq ($(strip $(MDI_LIBS)),)
HAVE_MDI := 1
endif

.PHONY: all daemon tools icons standalone clean dir_bin dir_icons dir_standalone

all: daemon tools icons standalone

daemon: ulanzi_d200_daemon

ulanzi_d200_daemon: ulanzi_d200_daemon.c
	$(CC) $(CFLAGS) -o $@ $< $(HID_LIBS) $(ZLIB_LIBS) $(PNG_LIBS)

tools: bin/send_image_page bin/send_video_page_wrapper bin/paging_daemon bin/ha_daemon

bin/paging_daemon: src/lib/paging.c | dir_bin
	$(CC) $(CFLAGS) $(YAML_CFLAGS) -o $@ $< $(PNG_LIBS) $(ZLIB_LIBS) $(YAML_LIBS)

bin/ha_daemon: src/lib/ha_daemon.c | dir_bin
	$(CC) $(CFLAGS) $(OPENSSL_CFLAGS) -o $@ $< $(PTHREAD_LIBS) $(OPENSSL_LIBS)

bin/send_image_page: src/lib/send_image_page.c | dir_bin
	$(CC) $(CFLAGS) -o $@ $< $(PNG_LIBS) $(PTHREAD_LIBS) $(MATH_LIBS)

bin/send_video_page_wrapper: src/lib/send_video_page_wrapper.c | dir_bin
	$(CC) $(CFLAGS) $(FFMPEG_CFLAGS) -o $@ $< $(FFMPEG_LIBS) $(PNG_LIBS) $(ZLIB_LIBS) $(PTHREAD_LIBS) $(MATH_LIBS)

icons: icons/draw_border icons/draw_optimize icons/draw_over icons/draw_square icons/draw_text
ifeq ($(HAVE_MDI),1)
icons: icons/draw_mdi
else
icons:
	@echo "Skipping icons/draw_mdi (missing cairo/librsvg dev libs or pkg-config)"
endif

icons/draw_%: src/icons/draw_%.c | dir_icons
	$(CC) $(CFLAGS) -o $@ $< $(ZLIB_LIBS)

icons/draw_mdi: src/icons/draw_mdi.c | dir_icons
ifeq ($(HAVE_MDI),1)
	$(CC) $(CFLAGS) $(MDI_CFLAGS) -o $@ $< $(ZLIB_LIBS) $(MDI_LIBS)
else
	@echo "Cannot build $@: missing cairo/librsvg dev libs or pkg-config" >&2
	@exit 1
endif

standalone: standalone/draw_optimize_std

standalone/draw_optimize_std: src/standalone/draw_optimize_std.c | dir_standalone
	$(CC) $(CFLAGS) -o $@ $< $(ZLIB_LIBS)

dir_bin:
	mkdir -p bin

dir_icons:
	mkdir -p icons

dir_standalone:
	mkdir -p standalone

clean:
	rm -f ulanzi_d200_daemon
	rm -f bin/paging_daemon
	rm -f bin/ha_daemon
	rm -f bin/send_video_page_wrapper
	rm -f bin/send_image_page
	rm -f icons/draw_border icons/draw_mdi icons/draw_optimize icons/draw_over icons/draw_square icons/draw_text
	rm -f standalone/draw_optimize_std
