current_dir := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))
OBJ_DIR = $(current_dir)/obj
SRC_DIR = $(current_dir)/

TARGETS = gen_fsk demod_fsk_file capture

CC_ARCH ?= native
CC      = gcc
CFLAGS  = -std=gnu11 -O2 -march=$(CC_ARCH) -Wall -Werror

all: $(OBJ_DIR) $(TARGETS)

$(OBJ_DIR):
	@mkdir -p $(OBJ_DIR)

gen_fsk: gen_fsk.c
	@echo "build $@"
	@$(CC) $(CFLAGS) $< -lm -lliquid -o $@

demod_fsk_file: demod_fsk_file.c
	@echo "build $@"
	@$(CC) $(CFLAGS) $< -lm -lliquid -o $@

capture: capture.c fobos.c
	@echo "build $@"
	@$(CC) $(CFLAGS) capture.c fobos.c -lm -lfobos_sdr -o $@

clean:
	@rm -rf $(OBJ_DIR) $(TARGETS)
