CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -Wall -Wextra -std=c99
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?=

SDL2_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2 2>/dev/null)
SDL2_LIBS := $(shell $(PKG_CONFIG) --libs sdl2 2>/dev/null)
ifneq ($(strip $(SDL2_LIBS)),)
CPPFLAGS += -Iinclude $(SDL2_CFLAGS) -DCHIP8_HAVE_SDL2
LDLIBS += $(SDL2_LIBS)
else
CPPFLAGS += -Iinclude
$(warning SDL2 development files not found; live sound is unavailable until SDL2 is installed)
endif

SOURCES = src/main.c src/chip8.c src/audio.c src/scheduler.c
TARGET = nacre

all: $(TARGET)

$(TARGET): $(SOURCES) include/chip8.h include/audio.h include/scheduler.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS) -lm

tests/test_scheduler: tests/test_scheduler.c src/chip8.c src/scheduler.c include/chip8.h include/scheduler.h
	$(CC) -Iinclude $(CFLAGS) -o $@ tests/test_scheduler.c src/chip8.c src/scheduler.c

tests/test_core: tests/test_core.c src/chip8.c include/chip8.h
	$(CC) -Iinclude $(CFLAGS) -o $@ tests/test_core.c src/chip8.c

tests/test_demo: tests/test_demo.c src/chip8.c src/scheduler.c include/chip8.h include/scheduler.h assets/roms/nacre.ch8
	$(CC) -Iinclude $(CFLAGS) -o $@ tests/test_demo.c src/chip8.c src/scheduler.c

test: tests/test_scheduler tests/test_core tests/test_demo
	./tests/test_scheduler
	./tests/test_core
	./tests/test_demo

clean:
	rm -f $(TARGET) tests/test_scheduler tests/test_core tests/test_demo

.PHONY: all clean test
