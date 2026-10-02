current_dir:=$(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))

OBJ_DIR = $(current_dir)/obj
SRC_DIR = $(current_dir)/

TARGET = detector

SRCS = main.c detector.c fft.c draw.c fobos.c receiver.c ringbufer.c sgram.c
OBJS = $(addprefix $(OBJ_DIR)/,$(SRCS:.c=.o))

CC_ARCH?=native

CC=gcc
CFLAGS=-std=gnu11 -g3 -Ofast -march=$(CC_ARCH) -mtune=$(CC_ARCH) -Wall -Werror $(FLAGS)
LDFLAGS=-lm -lpthread -lfftw3 -lfftw3f -lfftw3_threads -lfftw3f_threads -lfobos_sdr -lpng

all: $(OBJ_DIR) $(TARGET) test_detector

$(OBJ_DIR):
	@echo "create $@"
	@mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@echo "compile $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	@echo "build $@"
	@$(CC) $^ $(LDFLAGS) -o $@

test_detector: test_detector.c detector.c
	@echo "build $@"
	@$(CC) $(CFLAGS) $^ -lm -o $@

clean:
	@echo cleaning...
	@rm -fR $(OBJ_DIR) $(TARGET) test_detector
