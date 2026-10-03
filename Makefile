CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic
LDLIBS ?= -lrt

.PHONY: all clean rebuild

all: server client

server: src/server.cpp include/message.hpp include/reservation.hpp
	$(CXX) $(CXXFLAGS) -pthread src/server.cpp -o $@ $(LDLIBS)

client: src/client.cpp include/message.hpp
	$(CXX) $(CXXFLAGS) src/client.cpp -o $@ $(LDLIBS)

clean:
	$(RM) server client

rebuild: clean
	$(MAKE) all
