ifeq ($(origin CC), default)
CC         := gcc
endif
TARGET     := kvm_client
BUILD_DIR  := build

SRCS       := kvm_client.c kvm_video.c kvm_hid.c overlay.c
OBJS       := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS))
DEPS       := $(OBJS:.o=.d)

SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs)

CFLAGS     := -O3 -std=c11 -Wall -Wextra -MMD -MP $(SDL_CFLAGS)
LDLIBS     := $(SDL_LIBS) -lpthread -lm

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDLIBS)

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

-include $(DEPS)
