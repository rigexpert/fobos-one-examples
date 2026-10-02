current_dir:=$(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))

OBJ_DIR = $(current_dir)/obj
SRC_DIR = $(current_dir)/

SRCS = $(wildcard $(SRC_DIR)/*.c)  
SRCS_C += $(wildcard $(SRC_DIR)/*.cpp)                                                                                                                             
OBJS = $(addprefix $(OBJ_DIR)/,$(notdir $(SRCS:.c=.o)))
OBJS += $(addprefix $(OBJ_DIR)/,$(notdir $(SRCS_C:.cpp=.o)))  
DEP = $(OBJS:%.o=%.d)

TARGET = fft

CC_ARCH?=native

CC=gcc
CPP=g++
CPPFLAGS=-std=gnu++0x -g3 -Ofast -funsafe-math-optimizations -D_GNU_SOURCE=1 -march=$(CC_ARCH) -mtune=$(CC_ARCH) -Wall -Werror $(FLAGS)
CFLAGS=-std=gnu11 -g3 -Ofast -march=$(CC_ARCH) -mtune=$(CC_ARCH) -Wall -Werror $(FLAGS)
LDFLAGS= -lm -lpthread -lfftw3 -lfftw3f -lfftw3_threads -lfftw3f_threads -lfobos_sdr -lpng

#.PHONY: all FORCE

all:  $(OBJ_DIR) $(TARGET)

 $(OBJ_DIR):
	@echo "create $@"
	@mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@echo "compile $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@echo "compile $<"
	@$(CC) $(CPPFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	@echo "build $@"
	@$(CC) $^ $(LDFLAGS) -o $@

clean:
	@echo cleaning...
	@rm -fR $(OBJ_DIR) $(TARGET) $(OBJS) $(DEP)

