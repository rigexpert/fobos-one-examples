# Native C++ spectrum backend (dev build). Prefer the top-level CMake for installs.
CXX      ?= g++
CXXFLAGS ?= -O3 -std=c++17 -Wall -march=native -pthread -I/usr/local/include
LDFLAGS  ?= -L/usr/local/lib
LDLIBS   ?= -lfobos_sdr -lfftw3f -lcrypto -lz -pthread

SRCS := main.cpp routes.cpp worker.cpp dsp.cpp frame.cpp state.cpp fobos.cpp \
        broadcast.cpp calibration.cpp backend_switch.cpp wshttp.cpp json.cpp \
        log.cpp timeutil.cpp
OBJS := $(SRCS:.cpp=.o)

spectrumd: $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f spectrumd $(OBJS)
