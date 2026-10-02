current_dir:=$(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))

OBJ_DIR = $(current_dir)/obj
SRC_DIR = $(current_dir)/

TARGET = demod_fsk

SRCS = main.c dsp.c fobos.c ringbufer.c receiver.c
OBJS = $(addprefix $(OBJ_DIR)/,$(SRCS:.c=.o))

CC_ARCH?=native

CC=gcc
CFLAGS=-std=gnu11 -g3 -Ofast -march=$(CC_ARCH) -mtune=$(CC_ARCH) -Wall -Werror $(FLAGS)
LDFLAGS=-lm -lpthread -lliquid -lfobos_sdr

all: $(OBJ_DIR) $(TARGET) test_demod_fsk test_fsk_e2e

$(OBJ_DIR):
	@echo "create $@"
	@mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@echo "compile $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	@echo "build $@"
	@$(CC) $^ $(LDFLAGS) -o $@

test_demod_fsk: test_demod_fsk.c
	@echo "build $@"
	@$(CC) $(CFLAGS) $< -lm -o $@

test_fsk_e2e: test_fsk_e2e.c
	@echo "build $@"
	@$(CC) $(CFLAGS) $< -lliquid -lm -o $@

clean:
	@echo cleaning...
	@rm -fR $(OBJ_DIR) $(TARGET) test_demod_fsk test_fsk_e2e
