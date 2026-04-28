CC := cc
TARGET := syswatch
SRC_DIR := src
SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(SRCS:.c=.o)

CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
CURL_LIBS := $(shell pkg-config --libs libcurl 2>/dev/null)

CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O2 $(CURL_CFLAGS)
LDFLAGS := $(CURL_LIBS) -lpthread -lm

DEBUG_CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined $(CURL_CFLAGS)
DEBUG_LDFLAGS := -fsanitize=address,undefined $(CURL_LIBS) -lpthread -lm

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
