/* -*- mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * (c) h.zeller@acm.org. Free Software. GNU Public License v3.0 and above
 */

#include <stdio.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
#include <string>

#include "machine-connection.h"

#define ALERT_ON "\033[41m\033[30m"
#define ALERT_OFF "\033[0m"

static bool reliable_write(int fd, const char *buffer, int len) {
    while (len) {
        int w = write(fd, buffer, len);
        if (w < 0) return false;
        len -= w;
        buffer += w;
    }
    return true;
}

static int usage(const char *progname) {
    fprintf(stderr, "usage:\n"
            "%s <gcode-file> [connection-string]\n"
            "\nConnection string is either a path to a tty device or "
            "host:port\n"
            " * Serial connection\n"
            "   A path to the device name with an optional bit-rate\n"
            "   separated with a comma.\n"
            "   Examples of valid connection strings:\n"
            "   \t/dev/ttyACM0\n"
            "   \t/dev/ttyACM0,b115200\n"
            "  notice the 'b' prefix for the bit-rate.\n"
            "  Available bit-rates are one of [b9600, b19200, b38400, b57600, "
            "b115200, b230400, b460800]\n\n"
            " * TCP connection\n"
            "   For devices that receive gcode via tcp "
            "(e.g. http://beagleg.org/)\n"
            "   you specify the connection string as host:port. Example:\n"
            "   \tlocalhost:4444\n",
            progname);

    fprintf(stderr, "\nExamples:\n"
            "%s file.gcode /dev/ttyACM0,b115200\n"
            "%s file.gcode localhost:4444\n",
            progname, progname);
    return 1;
}

int main(int argc, char *argv[]) {
    if (argc < 2)
        return usage(argv[0]);

    const char *const filename = argv[1];
    std::fstream input(filename);

    const char *connect_str = (argc >= 3) ? argv[2] : "/dev/ttyACM0,b115200";
    const int machine_fd = OpenMachineConnection(connect_str);
    if (machine_fd < 0) {
        fprintf(stderr, "Failed to connect to machine %s\n", connect_str);
        return 1;
    }

    DiscardPendingInput(machine_fd, 3000);

    fprintf(stderr, "\n---- Start sending file '%s' -----\n", filename);
    std::string line;
    int line_no = 0;
    int lines_sent = 0;
    while (!input.eof()) {
        line_no++;
        getline(input, line);

	// Strip any comments that start with ; to the end of the line
	const size_t comment_start = line.find_first_of(';');
	if (comment_start != std::string::npos) {
            line.resize(comment_start);
	}

	// Now, strip away any trailing spaces
        while (!line.empty() && isspace(line[line.size()-1])) {
            line.resize(line.length()-1);
        }

	// If the line is empty, then skip it
	if (line.empty()) {
            continue;
	}

        fprintf(stderr, "%4d| %s ", line_no, line.c_str());
        fflush(stderr);
        line.append("\n");  // GRBL wants only newline, not CRLF
        if (!reliable_write(machine_fd, line.data(), line.length())) {
            fprintf(stderr, "Couldn't write!\n");
            return 1;
        }

        lines_sent++;

        // The OK 'flow control' used by all these serial machine controls
        if (!WaitForOkAck(machine_fd)) {
            fprintf(stderr,
                    ALERT_ON "[ Didn't get OK. Continue: ENTER; stop: CTRL-C ]"
                    ALERT_OFF "\n");
            getchar();
        } else {
            fprintf(stderr, "<< OK\n");
        }
    }
    close(machine_fd);
    fprintf(stderr, "Sent total of %d non-empty lines\n", lines_sent);
}
