# Makefile for cc64 - a small C compiler targeting the Commodore 64.
#
# Usage:
#   make          builds ./cc64 from src/*.c
#   make clean    removes build output
#
# Portable C99; works with clang (macOS/Apple Silicon) or gcc/cc (Linux).

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra
SRC_DIR := src
SOURCES := $(wildcard $(SRC_DIR)/*.c)
HEADERS := $(wildcard $(SRC_DIR)/*.h)
TARGET  := bin/cc64

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCES)

clean:
	rm -f $(TARGET)
