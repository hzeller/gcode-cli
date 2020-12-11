# Base directory for installation.
PREFIX?=/usr/local

CXXFLAGS=-W -Wall -Wextra -O2

gcode-cli: main.o machine-connection.o
	$(CXX) -o $@ $^

install: gcode-cli
	install -D gcode-cli $(PREFIX)/bin/gcode-cli

clean:
	rm -f *.o gcode-cli
