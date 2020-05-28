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

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <gcode-file> [connection string]\n", argv[0]);
        fprintf(stderr, "Example: %s file.gcode /dev/ttyACM0,b115200\n", argv[0]);

        return 1;
    }
    const char *filename = argv[1];

    std::fstream input(filename);

    int machine_fd=-1;

    if (argc >= 3) {
        machine_fd = OpenMachineConnection(argv[2]);
    }
    else {
        //use default connection string if not present
        machine_fd=OpenMachineConnection("/dev/ttyACM0,b115200");
    }

    if (machine_fd < 0) {
        fprintf(stderr, "Failed to connect to machine\n");
        return 1;
    }
    DiscardPendingInput(machine_fd, 3000);

    fprintf(stderr, "\n---- Start sending file '%s' -----\n", filename);
    std::string line;
    int line_no = 0;
    while (!input.eof()) {
        line_no++;
        getline(input, line);
        while (!line.empty() && isspace(line[line.size()-1])) {
            line.resize(line.length()-1);
        }
        fprintf(stderr, "%4d| %s ", line_no, line.c_str());
        fflush(stderr);
        line.append("\n");  // GRBL wants only newline, not CRLF
        if (!reliable_write(machine_fd, line.data(), line.length())) {
            fprintf(stderr, "Couldn't write!\n");
            return 1;
        }

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
}
