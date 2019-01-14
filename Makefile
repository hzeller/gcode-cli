CXXFLAGS=-Wall -Wextra

gcode-cli: main.o machine-connection.o
	$(CXX) -o $@ $^

clean:
	rm -f *.o gcode-cli
