CC ?= cc
PKG_CFLAGS := $(shell pkg-config --cflags libheif libavformat libavcodec libavutil libswscale)
PKG_LIBS := $(shell pkg-config --libs libheif libavformat libavcodec libavutil libswscale)

CFLAGS ?= -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE $(PKG_CFLAGS)
LDFLAGS := -lpthread -lz -lm -ljpeg $(PKG_LIBS)

SRC_DIR := src
BUILD_DIR := build

SRCS := \
    main.c \
    engine/decoder_registry.c \
    engine/progress.c \
    engine/resize.c \
    engine/stats.c \
    engine/uri.c \
    engine/util.c \
    engine/workqueue.c \
    engine/xdg.c \
    third_party/md5.c \
    third_party/png_writer.c \
    third_party/stb_image_impl.c \
    formats/stb_generic.c \
    formats/jpeg.c \
    formats/tiff_ifd.c \
    formats/exif_apply.c \
    formats/isobmff.c \
    formats/heic.c \
    formats/mp3.c \
    formats/flac.c \
    formats/ogg.c \
    formats/m4a.c \
    formats/raw.c \
    formats/video.c

OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS))

TARGET := thumbnailer

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(TARGET)
