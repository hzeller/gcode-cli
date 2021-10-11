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

#include "buffered-line-reader.h"
#include "machine-connection.h"

#define ALERT_ON "\033[41m\033[30m"
#define ALERT_OFF "\033[0m"

#define EXTRA_MESSAGE_ON "\033[7m"
#define EXTRA_MESSAGE_OFF "\033[0m"

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
static bool write_blocks(int fd, char *scratch_buffer,
                         const std::vector<std::string_view> &blocks) {
    // Note: not using writev(), as snippets can be a lot and writev() has a
    // bunch of limitations (e.g. UIO_MAXIOV == 1024). Thus simply
    // reassmbling into buffer.
    char *pos = scratch_buffer;
    for (auto block : blocks) {
        memcpy(pos, block.data(), block.size());
        pos += block.size();
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
            "\t-s <millis> : Wait this time for init "
            "chatter from machine to subside.\n"
            "\t              Default: 300\n"
            "\t-b <count>  : Number of blocks sent out buffered before \n"
            "\t              checking the returning flow-control 'ok'.\n"
            "\t              Careful, low memory machines might drop data.\n"
            "\t              Default: 1\n"
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

// Very crude error handling 'ui'. If this is an interactive session we can ask
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

// Read and classify response from machine. Currently, we expect a line with
// 'ok' at the beginning for an acknowledged block, 'error' at the beginning
// for some kind of error, and everything else a message (e.g. output of
// current temperature values.
enum class AckResponse { kOk, kError, kMessage };
static AckResponse ReadResponse(bool use_flow_control,
                                BufferedLineReader &flow_input,
                                std::string_view *return_message) {
    if (!use_flow_control)
        return AckResponse::kOk;  // Don't read, always assume 'ok'.
    const std::string_view ack_msg = flow_input.ReadLine();
    if (flow_input.is_eof()) {
        *return_message = "Nothing received from machine: Connection closed";
        return AckResponse::kError;
    }
    if (ack_msg.size() >= 2 && strncasecmp(ack_msg.data(), "ok", 2) == 0)
        return AckResponse::kOk;
    *return_message = ack_msg;  // Any other ack message is useful. Return.
    // TODO: there might be other ways machines report errors.
    if (ack_msg.size() >= 5 && strncasecmp(ack_msg.data(), "error", 5) == 0)
        return AckResponse::kError;
    return AckResponse::kMessage;
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
    int initial_squash_chatter_ms = 300;  // Time for initial chatter to finish

    // No cli options yet.
    size_t buffer_size = (1 << 20);       // Input buffer in bytes.

    int opt;
    while ((opt = getopt(argc, argv, "b:cFhnqs:")) != -1) {
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
        case 's':
            initial_squash_chatter_ms = atoi(optarg);
            if (initial_squash_chatter_ms < 0) {
                return usage(argv[0], "Invalid startup squash timeout\n");
            }
            break;
        case 'h':
            return usage(argv[0], "");
        default:
            return usage(argv[0], "Invalid option\n");
        }
    }

    if (optind >= argc)
        return usage(argv[0], "Expected filename\n");

    // Input: Open GCode file
    const char *const filename = argv[optind];
    int input_fd = (filename == std::string("-"))
        ? STDIN_FILENO
        : open(filename, O_RDONLY);
    if (input_fd < 0) {
        fprintf(stderr, "Can't open input %s: %s\n", filename, strerror(errno));
        return 1;
    }

    // Output: open machine connection.
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

    BufferedLineReader gcode_reader(input_fd, buffer_size,
                                    remove_semicolon_comments);
    BufferedLineReader flow_control_input(machine_fd, (1<<16), false);
    char *scratch_buffer = new char[buffer_size];
    int line_no = 0;
    const int64_t start_time = get_time_ms();
    while (!gcode_reader.is_eof()) {
        auto lines = gcode_reader.ReadNextLines(block_buffer_count);

        // Send all block_buffer_count blocks at once.
        if (!is_dry_run && !write_blocks(machine_fd, scratch_buffer, lines)) {
            fprintf(stderr, "Couldn't write!\n");
            return 1;
        }

        // We've sent all the lines above at once, now looking at the
        // expected responses for each block and possibly print the lines
        // together with their corresponding response.
        for (auto request : lines) {
            line_no++;
            bool request_line_already_printed = false;
            AckResponse response;
            do {
                std::string_view print_msg;
                response = ReadResponse(use_ok_flow_control, flow_control_input,
                                        &print_msg);

                // Now we know enough if we should print the original
                // request. Whenever there is some unusual stuff going on, we
                // want to print the original message first before the response.
                const bool needs_printing = (
                    print_communication ||
                    response == AckResponse::kError ||
                    (print_unusual_messages && response != AckResponse::kOk));

                if (needs_printing) {
                    if (!request_line_already_printed) {
                        printf("%6d\t%.*s ", line_no,
                               (int)request.size() - 1, request.data());
                        request_line_already_printed = true;
                    }
                    if (response == AckResponse::kOk) {
                        printf(use_ok_flow_control ? "<< OK\n" : "\n");
                    } else {
                        printf("\n%s%.*s%s", EXTRA_MESSAGE_ON,
                               (int)print_msg.size(), print_msg.data(),
                               EXTRA_MESSAGE_OFF);
                    }
                    fflush(stdout);
                }

                if (response == AckResponse::kError) {
                    handle_error_or_exit();
                }
            } while (response == AckResponse::kMessage);  // more to come
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
    close(input_fd);

    if (!quiet) {
        const int64_t duration = get_time_ms() - start_time;
        fprintf(stderr, "Sent total of %d non-empty lines in "
                "%" PRId64 ".%03" PRId64 "s\n",
                line_no, duration / 1000, duration % 1000);
    }
}
