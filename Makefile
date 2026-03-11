CC := cc
TARGET := syswatch
SRC_DIR := src
SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(SRCS:.c=.o)

CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O2
LDFLAGS :=

DEBUG_CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined
DEBUG_LDFLAGS := -fsanitize=address,undefined

.PHONY: all debug clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c $(SRC_DIR)/syswatch.h
	$(CC) $(CFLAGS) -c $< -o $@

debug: CFLAGS := $(DEBUG_CFLAGS)
debug: LDFLAGS := $(DEBUG_LDFLAGS)
debug: clean $(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)
