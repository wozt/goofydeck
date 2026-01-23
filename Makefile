SHELL := /bin/bash

CC ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra

ZLIB_LIBS := -lz
PNG_LIBS := -lpng
PTHREAD_LIBS := -lpthread
MATH_LIBS := -lm
HID_LIBS := -lhidapi-libusb

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

.PHONY: all daemon tools icons standalone clean dir_lib dir_icons dir_standalone

all: daemon tools icons standalone

daemon: ulanzi_d200_demon

ulanzi_d200_demon: ulanzi_d200.c
	$(CC) $(CFLAGS) -o $@ $< $(HID_LIBS) $(ZLIB_LIBS) $(PNG_LIBS)

tools: lib/send_image_page lib/send_video_page_wrapper lib/pagging_demon

lib/pagging_demon: src/lib/pagging.c | dir_lib
	$(CC) $(CFLAGS) -o $@ $< $(PNG_LIBS) $(ZLIB_LIBS)

lib/send_image_page: src/lib/send_image_page.c | dir_lib
	$(CC) $(CFLAGS) -o $@ $< $(PNG_LIBS) $(PTHREAD_LIBS) $(MATH_LIBS)

lib/send_video_page_wrapper: src/lib/send_video_page_wrapper.c | dir_lib
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

dir_lib:
	mkdir -p lib

dir_icons:
	mkdir -p icons

dir_standalone:
	mkdir -p standalone

clean:
	rm -f ulanzi_d200_demon
	rm -f lib/pagging_demon
	rm -f lib/send_video_page_wrapper
	rm -f lib/send_image_page
	rm -f icons/draw_border icons/draw_mdi icons/draw_optimize icons/draw_over icons/draw_square icons/draw_text
	rm -f standalone/draw_optimize_std
