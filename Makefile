CC      := gcc
CFLAGS  := -std=c11 -O2 -Wall -Wextra
LDFLAGS := -pthread
LIBS    := -lncurses -lrt

SRC_DIR := src
OBJ_DIR := build

LP_SRCS := $(SRC_DIR)/logpulse.c \
           $(SRC_DIR)/coordinator.c \
           $(SRC_DIR)/worker.c \
           $(SRC_DIR)/parser.c \
           $(SRC_DIR)/ui.c

LP_OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(LP_SRCS))

.PHONY: all clean tsan debug

all: logpulse loggen

logpulse: $(LP_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

loggen: $(OBJ_DIR)/loggen.o
	$(CC) $(CFLAGS) -o $@ $^

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

tsan: CFLAGS := -std=c11 -O1 -g -Wall -Wextra -fsanitize=thread
tsan: LDFLAGS := -pthread -fsanitize=thread
tsan: clean all

debug: CFLAGS := -std=c11 -O0 -g -Wall -Wextra
debug: clean all

clean:
	rm -rf $(OBJ_DIR) logpulse loggen
