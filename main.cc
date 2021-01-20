/* -*- mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * (c) h.zeller@acm.org. Free Software. GNU Public License v3.0 and above
 */

#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "gcode-line-reader.h"
#include "machine-connection.h"

#define ALERT_ON "\033[41m\033[30m"
#define ALERT_OFF "\033[0m"

typedef std::vector<std::string_view>::iterator SnippetIterator;

// Write buffer to fd.
static bool reliable_write(int fd, const char *buffer, int len) {
    while (len) {
        int w = write(fd, buffer, len);
        if (w < 0) return false;
        len -= w;
        buffer += w;
    }
    return true;
}

// Write the sequence of string-views to file-descriptor.
// Needs "scratch_buffer" to be large enough to contain all of them.
static bool write_snippets(int fd, char *scratch_buffer,
                           SnippetIterator begin, SnippetIterator end) {
    // Note: not using writev(), as snippets can be a lot and writev() has a
    // bunch of limitations (e.g. UIO_MAXIOV == 1024). Thus simply
    // reassmbling into buffer.
    char *pos = scratch_buffer;
    for (SnippetIterator it = begin; it != end; ++it) {
        memcpy(pos, it->data(), it->size());
        pos += it->size();
    }
    return reliable_write(fd, scratch_buffer, pos - scratch_buffer);
}

static int64_t get_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t) tv.tv_sec * 1000 + (int64_t) tv.tv_usec / 1000;
}

static int usage(const char *progname, const char *message) {
    fprintf(stderr, "%sUsage:\n"
            "%s [options] <gcode-file> [<connection-string>]\n"
            "Options:\n"
            "\t-b <count> : Number of blocks sent out buffered before \n"
            "\t     checking the returning flow-control 'ok'. Dangerous if\n"
            "\t     machine has little memory. Default: 1\n"
            "\t-c : Include semicolon end-of-line comments (they are stripped\n"
            "\t     by default)\n"
            "\t-n : Dry-run. Read GCode but don't actually send anything.\n"
            "\t-q : Quiet. Don't output diagnostic messages or "
            "echo regular communication.\n"
            "\t     Apply -q twice to even suppress "
            "non-handshake communication.\n"
            "\t-F : Disable waiting for 'ok'-acknowledge flow-control.\n"
            "\n"
            "<gcode-file> is either a filename or '-' for stdin\n"
            "\n"
            "\n<connection-string> is either a path to a tty device or "
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
            message, progname);

    fprintf(stderr, "\nExamples:\n"
            "%s file.gcode /dev/ttyACM0,b115200\n"
            "%s file.gcode localhost:4444\n",
            progname, progname);
    return 1;
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
    // Command line options.
    bool use_ok_flow_control = true;   // if false, don't wait for 'ok' response
    bool is_dry_run = false;           // if true, don't actually send anything
    bool quiet = false;                // Don't print diagnostics
    bool print_unusual_messages = true;  // messages not expected from handshake
    int block_buffer_count = 1;     // Number of blocks sent at once.
    bool remove_semicolon_comments = true;
    bool print_communication = true;   // if true, print line+block to stdout

    // No cli options yet.
    int initial_squash_chatter_ms = 300;  // Time for initial chatter to finish
    size_t buffer_size = (1 << 20);       // Input buffer in bytes.

    int opt;
    while ((opt = getopt(argc, argv, "b:cFhnq")) != -1) {
        switch (opt) {
        case 'n':
            is_dry_run = true;
            break;
        case 'q':
            print_unusual_messages = !quiet;  // squashed if -q multiple times
            quiet = true;
            print_communication = false;  // Make separate option ?
            break;
        case 'F':
            use_ok_flow_control = false;
            break;
        case 'b':
            block_buffer_count = atoi(optarg);
            if (block_buffer_count < 1)
                return usage(argv[0], "Invalid block buffer\n");
            break;
        case 'c':
            remove_semicolon_comments = false;
            break;
        case 'h':
            return usage(argv[0], "");
        default:
            return usage(argv[0], "Invalid option\n");
        }
    }

    if (optind >= argc)
        return usage(argv[0], "Expected filename\n");

    char *write_scratch_buffer = new char[buffer_size];

    // Input
    const char *const filename = argv[optind];
    int input_fd = (filename == std::string("-"))
        ? STDIN_FILENO
        : open(filename, O_RDONLY);

    GCodeLineReader reader(input_fd, buffer_size, remove_semicolon_comments);

    // Output
    const char *const connect_str = (optind < argc-1)
        ? argv[optind+1]
        : "/dev/ttyACM0,b115200";
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

    std::string_view line;
    int line_no = 0;
    const int64_t start_time = get_time_ms();
    while (!reader.is_eof()) {
        auto lines = reader.ReadNextLines();

        // We got a whole bunch of lines. Divide them into chunks of
        // block_buffer_count, the maxium number of blocks we have outstanding
        // before checking the 'ok' responses.

        const SnippetIterator overall_lines_end = lines.end();
        SnippetIterator chunk_begin = lines.begin();
        SnippetIterator chunk_end;
        while (chunk_begin < overall_lines_end) {
            chunk_end = std::min(chunk_begin + block_buffer_count,
                                 overall_lines_end);
            if (!is_dry_run && !write_snippets(machine_fd, write_scratch_buffer,
                                               chunk_begin, chunk_end)) {
                fprintf(stderr, "Couldn't write!\n");
                return 1;
            }

            // We've sent all the lines above at once, now looking at the
            // expected responses for each block.
            // Associate each line with its corresponding output.
            for (auto it = chunk_begin; it != chunk_end; ++it) {
                std::string_view line = *it;
                line_no++;
                if (print_communication) {
                    printf("%6d\t%.*s ", line_no,
                           (int)line.size() - 1,  // Excluding the newline char
                           line.data());
                    fflush(stdout);
                }

                // The OK flow control used by all these serial machine controls
                if (use_ok_flow_control &&
                    !WaitForOkAck(
                        machine_fd,
                        print_unusual_messages,  // print errors
                        print_communication)) {  // seprate with newline
                    handle_error_or_exit();
                } else {
                    if (print_communication)
                        printf(use_ok_flow_control ? "<< OK\n" : "\n");
                }
            }

            chunk_begin = chunk_end; // Next round.
        }
    }

    // We don't really expect anything coming afterwards from the machine, but
    // if there is an imbalance of sent commands vs. acknowledge flow control
    // tokens, we'd see it now.
    if (!is_dry_run) {
        if (!quiet)
            fprintf(stderr, "Discarding remaining machine responses.\n");
        DiscardPendingInput(machine_fd, initial_squash_chatter_ms,
                            print_unusual_messages);
    }

    close(machine_fd);
    if (!quiet) {
        const int64_t duration = get_time_ms() - start_time;
        fprintf(stderr, "Sent total of %d non-empty lines in "
                "%" PRId64 ".%03" PRId64 "s\n",
                line_no, duration / 1000, duration % 1000);
    }
}
