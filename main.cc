/* -*- mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * (c) h.zeller@acm.org. Free Software. GNU Public License v3.0 and above
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include <string_view>
#include <string>

#include "machine-connection.h"

#define ALERT_ON "\033[41m\033[30m"
#define ALERT_OFF "\033[0m"

// Write buffer to fd, append newline.
static bool reliable_writeln(int fd, const char *buffer, int len) {
    while (len) {
        int w = write(fd, buffer, len);
        if (w < 0) return false;
        len -= w;
        buffer += w;
    }
    return write(fd, "\n", 1) == 1;
}

static int usage(const char *progname) {
    fprintf(stderr, "usage:\n"
            "%s <gcode-file> [connection-string]\n"
            "<gcode-file> is either a filename or '-' for stdin\n"
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

// The std::istream does handle EOF flag poorly with stdin, e.g. an
// end-of-file on /dev/stdin will not be detected properly.
// Use old-school C-style filestreams to handle this.
// Read from "in", store line in "out". Return if line-reading was successful.
// Returned string_view content is valid until the next call to read_line().
static bool read_line(FILE *in, std::string_view *out) {
    static char *buffer = nullptr;   // static: Re-use buffer between calls.
    static size_t n = 0;
    ssize_t result = getline(&buffer, &n, in);

    if (result >= 0 && buffer) {
        *out = std::string_view(buffer, result);
        return true;
    }
    return false;
}

// Very crude error handling. If this is an interactive session we can ask
// the user to decide.
static void handle_error_or_exit() {
    if (isatty(STDIN_FILENO)) {  // interactive.
        fprintf(stderr,
                ALERT_ON "[ Didn't get OK. Continue: ENTER; stop: CTRL-C ]"
                ALERT_OFF "\n");
        getchar();
    } else {
        fprintf(stderr, ALERT_ON "[ Received error. Non-interactive session "
                "does not allow for user feedback. Bailing out.]"
                ALERT_OFF "\n");
        exit(1);
    }
}

int main(int argc, char *argv[]) {
    // TODO: these configuration choices should be made command line options.
    int initial_squash_chatter_ms = 300;  // Time for initial chatter to finish
    bool use_ok_flow_control = true;   // if false, just blast out.
    bool is_dry_run = false;           // if true, don't actually send anything
    bool print_communication = true;   // if true, print line+block to stdout
    bool quiet = false;                // Don't print addtional diagnostics

    if (argc < 2)
        return usage(argv[0]);

    // Input
    const char *const filename = argv[1];
    FILE *input = filename == std::string("-") ? stdin : fopen(filename, "r");

    // Output
    const char *connect_str = (argc >= 3) ? argv[2] : "/dev/ttyACM0,b115200";
    is_dry_run |= (strcmp(connect_str, "/dev/null") == 0);

    int machine_fd = -1;
    if (!is_dry_run) {
        machine_fd = OpenMachineConnection(connect_str);
        if (machine_fd < 0) {
            fprintf(stderr, "Failed to connect to machine %s\n", connect_str);
            return 1;
        }
    }

    // In a dry-run, we also will not read anything.
    use_ok_flow_control &= !is_dry_run;

    // If there is some initial chatter, ignore it, until there is some time
    // silence on the wire. That way, we only get OK responses to our requests.
    if (use_ok_flow_control) {
        DiscardPendingInput(machine_fd, initial_squash_chatter_ms,
                            print_communication);
    }

    if (!quiet) {
        fprintf(stderr, "\n---- Sending file '%s' to '%s'%s -----\n", filename,
                connect_str, is_dry_run ? " (Dry-run)" : "");
    }

    // Even if we don't usually print the communication, unless we're not
    // quiet, we'll print error messages coming back from the machine.
    const bool print_errors = print_communication || !quiet;

    std::string_view line;
    int line_no = 0;
    int lines_sent = 0;
    while (read_line(input, &line)) {
        line_no++;

        // Strip any comments that start with ; to the end of the line
        const size_t comment_start = line.find_first_of(';');
        if (comment_start != std::string_view::npos) {
            line.remove_suffix(line.size() - comment_start);
        }

        // Now, strip away any trailing spaces
        while (!line.empty() && isspace(line[line.size()-1])) {
            line.remove_suffix(1);
        }

        // Nothing left to send: skip.
        if (line.empty()) {
            continue;
        }

        if (print_communication) {
            printf("%4d| %.*s ", line_no, (int)line.size(), line.data());
            fflush(stdout);
        }

        // We now have a line stripped of any whitespace or newline character
        // at the end.
        // Write this plus exactly one newline now. GRBL and Smoothieware
        // for instance would consider sending \r\n as two lines (and send two
        // 'ok' in response), so this makes sure we send one block for which
        // we expect exactly one 'ok' below.
        if (!is_dry_run &&
            !reliable_writeln(machine_fd, line.data(), line.size())) {
            fprintf(stderr, "Couldn't write!\n");
            return 1;
        }

        lines_sent++;

        // The OK 'flow control' used by all these serial machine controls
        if (use_ok_flow_control && !WaitForOkAck(machine_fd, print_errors)) {
            handle_error_or_exit();
        } else {
            if (print_communication)
                printf(use_ok_flow_control ? "<< OK\n" : "\n");
        }
    }

    // We don't really expect anything coming afterwards from the machine, but
    // if there is an imbalance of sent commands vs. acknowledge flow control
    // tokens, we'd see it now.
    if (!is_dry_run) {
        if (!quiet)
            fprintf(stderr, "Discarding remaining machine responses.\n");
        DiscardPendingInput(machine_fd, initial_squash_chatter_ms,
                            print_communication);
    }

    close(machine_fd);
    if (!quiet)
        fprintf(stderr, "Sent total of %d non-empty lines.\n", lines_sent);
}
