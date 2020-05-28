/* -*- mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * (c) h.zeller@acm.org. Free Software. GNU Public License v3.0 and above
 */

#include <stdio.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
#include <string>

using std::cerr;
using std::cout;
using std::endl;

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
        cerr<<"Usage: "<<argv[0]<<" <file> [connection string]"<<endl;
        cerr<<endl;
        cerr<<"If connection string is not specified, defaults to /dev/ttyACM0,b115200"<<endl;
        return 1;
    }
    const char *filename = argv[1];

    std::string connection_string="/dev/ttyACM0,b115200";

    if(argc >= 3) {
      connection_string=std::string(argv[2]);
    }

    std::fstream input(filename);

    const int machine_fd = OpenMachineConnection(connection_string.c_str());
    if (machine_fd < 0) {
        cerr<<"Failed to open connection to: "<<connection_string<<endl;
        return 1;
    }
    DiscardPendingInput(machine_fd, 3000);

    cerr<<endl;
    cerr<<"----- Start sending file '"<<filename<<"' -----"<<endl;

    std::string line;
    int line_no = 0;
    while (!input.eof()) {
        line_no++;
        getline(input, line);
        while (!line.empty() && isspace(line[line.size()-1])) {
            line.resize(line.length()-1);
        }
        cerr<<line_no<<"| "<<line<<" ";
        line.append("\n");  // GRBL wants only newline, not CRLF
        if (!reliable_write(machine_fd, line.data(), line.length())) {
            cerr<<"Couldn't write"<<endl;
            return 1;
        }

        // The OK 'flow control' used by all these serial machine controls
        if (!WaitForOkAck(machine_fd)) {
            cerr<<ALERT_ON<<"[ Didn't get OK. Continue: ENTER; stop: CTRL-C ]"<<ALERT_OFF<<endl;
            getchar();
        } else {
            cerr<<" << OK"<<endl;
        }
    }
    close(machine_fd);
}
