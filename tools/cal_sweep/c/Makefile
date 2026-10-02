current_dir := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))
OBJ_DIR = $(current_dir)/obj
SRC_DIR = $(current_dir)/
TARGET  = measure

SRCS  = measure.c fobos.c
OBJS  = $(addprefix $(OBJ_DIR)/,$(SRCS:.c=.o))

CC_ARCH ?= native
CC      = gcc
CFLAGS  = -std=gnu11 -O2 -march=$(CC_ARCH) -Wall -Werror
LDFLAGS = -lm -lfobos_sdr -lfftw3f

all: $(OBJ_DIR) $(TARGET)

$(OBJ_DIR):
	@mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@echo "compile $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	@echo "build $@"
	@$(CC) $^ $(LDFLAGS) -o $@

clean:
	@rm -rf $(OBJ_DIR) $(TARGET)
