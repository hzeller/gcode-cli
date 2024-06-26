# Base directory for installation.
PREFIX?=/usr/local

CXXFLAGS=-W -Wall -Wextra -Wno-unused-parameter -O2 -std=c++17 $(EXTRA_CFLAGS)

gcode-cli: main.o machine-connection.o buffered-line-reader.o
	$(CXX) -o $@ $^

install: gcode-cli
	install -D gcode-cli $(PREFIX)/bin/gcode-cli

clean:
	rm -f *.o gcode-cli
