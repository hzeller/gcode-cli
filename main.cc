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
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "buffered-line-reader.h"
#include "machine-connection.h"

static int64_t get_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000 + (int64_t)tv.tv_usec / 1000;
}

static int usage(const char *progname, const char *message) {
    fprintf(stderr,
            "%sUsage:\n"
            "%s [options] <gcode-file> [<connection-string>]\n"
            "Options:\n"
            "\t-s <millis> : Wait this time for init "
            "chatter from machine to subside.\n"
            "\t              Default: 2500\n"
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
            "\n<connection-string> is either a path to a tty device, a "
            "host:port or '-'\n"
            " * Serial connection\n"
            "   A path to the device name with an optional bit-rate and flow\n"
            "   control settings separated by comma.\n\n"
            "   If no device parameters given, default is 'b115200,+crtscts'\n"
            "\n   Examples of valid connection strings:\n"
            "   \t/dev/ttyACM0\n"
            "   \t/dev/ttyACM0,b115200\n"
            "   notice the 'b' prefix for the bit-rate"
#ifdef USE_TERMIOS
            ".\n   Common bit-rates are one of [b9600, b19200, b38400, "
            "b57600, b115200, b230400, b460800]\n"
#else
            " (any value allowed supported by system).\n"
#endif
            "\n  Serial Flow Control\n"
            "   A +crtscts enables hardware flow control RTS/CTS handshaking:\n"
            "   \t/dev/ttyACM0,b115200,+crtscts\n"
            "   With a minus prefix, disable hardware flow control:\n"
            "   \t/dev/ttyACM0,b115200,-crtscts\n"

            "\n"
            " * TCP connection\n"
            "   For devices that receive gcode via tcp "
            "(e.g. http://beagleg.org/)\n"
            "   you specify the connection string as host:port. Example:\n"
            "   \tlocalhost:4444\n\n"
            " * stdin/stdout\n"
            "   For a simple communication writing to the machine to stdout\n"
            "   and read responses from stdin, use '-'\n"
            "   This is useful for debugging or wiring up using e.g. socat.\n",
            message, progname);

    fprintf(stderr,
            "\nExamples:\n"
            "%s file.gcode /dev/ttyACM0,b115200\n"
            "%s file.gcode localhost:4444\n",
            progname, progname);
    return 1;
}

// Very crude error handling 'ui'. If this is an interactive session we can ask
// the user to decide.
static void handle_error_or_exit() {
#define ALERT_ON  "\033[41m\033[30m"
#define ALERT_OFF "\033[0m"

    if (isatty(STDIN_FILENO)) {  // interactive.
        fprintf(stderr, ALERT_ON
                "[ Didn't get OK. Continue: ENTER; stop: CTRL-C ]" ALERT_OFF
                "\n");
        getchar();
    } else {
        fprintf(stderr,
                "[ Received error. Non-interactive session "
                "does not allow for user feedback. Bailing out.]"
                "\n");
        exit(1);
    }
}

// Check for case-insensitive prefix
static bool hasPrefixIgnoreCase(std::string_view msg, std::string_view prefix) {
    return (msg.length() >= prefix.length() &&
            strncasecmp(msg.data(), prefix.data(), prefix.length()) == 0);
}

// Read and classify response from machine. Currently, we expect a line with
// 'ok' at the beginning for an acknowledged block, 'error' at the beginning
// for some kind of error, and everything else a message (e.g. output of
// current temperature values.
enum class AckResponse { kOk, kError, kMessage };
static AckResponse ReadResponseLine(bool use_flow_control,
                                    MachineConnection *machine,
                                    std::string_view *return_message) {
    if (!use_flow_control) {
        return AckResponse::kOk;  // Don't read, always assume 'ok'.
    }

    const std::string_view ack_msg = machine->ResponseLines().ReadLine();
    if (machine->ResponseLines().is_eof()) {
        *return_message = "Nothing received from machine: Connection closed";
        return AckResponse::kError;
    }

    // TODO: there might be other ways machines report ok or errors.

    // OK response.
    if (hasPrefixIgnoreCase(ack_msg, "ok")) {
        return AckResponse::kOk;
    }

    // Any non-ok messages are useful to print. Return.
    *return_message = ack_msg;

    // ERROR response.
    if (hasPrefixIgnoreCase(ack_msg, "error") ||
        hasPrefixIgnoreCase(ack_msg, "alarm")) {
        return AckResponse::kError;
    }

    // Neither one or the other, so regard this as just a line that is not
    // a completed response yet.
    return AckResponse::kMessage;
}

int main(int argc, char *argv[]) {
    // -- Command line options.
    bool is_dry_run = false;                // Don't send anything if enabled.
    bool use_ok_flow_control = true;        // wait for 'ok' response
    int block_buffer_count = 1;             // Number of blocks sent at once.
    bool remove_semicolon_comments = true;  // Not all machines understand them
    int initial_squash_chatter_ms = 2500;   // Start after start machine prompt.

    bool print_communication = true;     // print line+block to $log_gcode
    bool print_unusual_messages = true;  // messages outside handshake

    // No cli options for the following yet. Make configurable ?
    const size_t buffer_size = (1 << 20);  // Input buffer in bytes
    FILE *const log_gcode = stderr;        // Log gcode communication here.
    FILE *log_info = stderr;               // info log, switched off with -q

    const char *EXTRA_MESSAGE_ON = "\033[7m";
    const char *EXTRA_MESSAGE_OFF = "\033[0m";
    if (!isatty(STDERR_FILENO)) {
        EXTRA_MESSAGE_ON = EXTRA_MESSAGE_OFF = "";
    }

    int opt;
    while ((opt = getopt(argc, argv, "b:cFhnqs:")) != -1) {
        switch (opt) {
        case 'n': is_dry_run = true; break;
        case 'q':
            log_info = nullptr;
            // unusual messages squashed if -q multiple times
            print_unusual_messages = print_communication;
            print_communication = false;  // Make separate option ?
            break;
        case 'F': use_ok_flow_control = false; break;
        case 'b':
            block_buffer_count = atoi(optarg);
            if (block_buffer_count < 1)
                return usage(argv[0], "Invalid block buffer\n");
            break;
        case 'c': remove_semicolon_comments = false; break;
        case 's':
            initial_squash_chatter_ms = atoi(optarg);
            if (initial_squash_chatter_ms < 0) {
                return usage(argv[0], "Invalid startup squash timeout\n");
            }
            break;
        case 'h': return usage(argv[0], "");
        default: return usage(argv[0], "Invalid option\n");
        }
    }

    if (optind >= argc) {
        return usage(argv[0], "Expected filename\n");
    }

    // Input: Open GCode file
    const char *const filename = argv[optind];
    const int input_fd = (filename == std::string("-"))
                             ? STDIN_FILENO
                             : open(filename, O_RDONLY);
    if (input_fd < 0) {
        fprintf(stderr, "Can't open input %s: %s\n", filename, strerror(errno));
        return 1;
    }

    // Output: open machine connection.
    const char *const connect_str = (optind < argc - 1)  // destination as arg
                                        ? argv[optind + 1]
                                        : "/dev/ttyACM0,b115200";
    is_dry_run |= (strcmp(connect_str, "/dev/null") == 0);

    std::unique_ptr<MachineConnection> machine;
    if (!is_dry_run) {
        machine.reset(MachineConnection::Open(connect_str));
        if (!machine) {
            fprintf(stderr, "Failed to connect to machine %s\n", connect_str);
            return 1;
        }
    }

    // In a dry-run, we also will not read anything.
    use_ok_flow_control &= !is_dry_run;

    // If there is some initial chatter, ignore it, until there is some time
    // silence on the wire.
    // That way, we only get OK responses to our requests.
    // Even without OK flow control, we need to wait as machine might
    // just reset on connect.
    if (machine) {
        machine->DiscardPendingInput(initial_squash_chatter_ms,
                                     print_communication ? log_gcode : nullptr);
    }

    if (log_info) {
        fprintf(log_info, "\n---- Sending file '%s' to '%s'%s -----\n",
                filename, connect_str, is_dry_run ? " (Dry-run)" : "");
    }

    BufferedLineReader gcode_reader(input_fd, buffer_size,
                                    remove_semicolon_comments);
    char *scratch_buffer = new char[buffer_size];
    int line_no = 0;
    const int64_t start_time = get_time_ms();
    while (!gcode_reader.is_eof()) {
        const auto lines = gcode_reader.ReadNextLines(block_buffer_count);

        // Send all block_buffer_count blocks at once.
        if (!is_dry_run && !machine->WriteBlocks(scratch_buffer, lines)) {
            fprintf(stderr, "Couldn't write!\n");
            return 1;
        }

        // We've sent all the lines above at once, now looking at the
        // expected responses for each block to confirm success.
        // Respons to a gcode-block can be multiple lines and are expected
        // to finish with either "ok" or "error".
        // If communication printing requested, print the lines together with
        // their corresponding response.
        for (const auto request : lines) {
            line_no++;
            bool request_line_already_printed = false;
            AckResponse response;
            do {
                std::string_view print_msg;
                response = ReadResponseLine(use_ok_flow_control, machine.get(),
                                            &print_msg);

                // Now we know enough if we should print the original
                // request. Whenever there is some unusual stuff going on, we
                // want to print the original message first before the response.
                const bool needs_printing =
                    (print_communication ||              // regular chatter
                     response == AckResponse::kError ||  // always print error
                     (print_unusual_messages && response != AckResponse::kOk));

                if (needs_printing) {
                    if (!request_line_already_printed) {
                        fprintf(log_gcode, "%6d\t%.*s ", line_no,
                                (int)request.size() - 1, request.data());
                        request_line_already_printed = true;
                    }
                    if (response == AckResponse::kOk) {
                        fprintf(log_gcode,
                                use_ok_flow_control ? "<< OK\n" : "\n");
                    } else {
                        while (!print_msg.empty() &&
                               isspace(*(print_msg.end() - 1))) {
                            print_msg.remove_suffix(1);
                        }
                        fprintf(log_gcode, "\n%s%.*s%s", EXTRA_MESSAGE_ON,
                                (int)print_msg.size(), print_msg.data(),
                                EXTRA_MESSAGE_OFF);
                    }
                    fflush(log_gcode);
                }

                if (response == AckResponse::kError) {
                    handle_error_or_exit();
                }
            } while (response == AckResponse::kMessage);  // more to come
        }
    }

    const int64_t duration = get_time_ms() - start_time;

    if (log_info) {
        fprintf(log_info, "---- Finished file '%s' -----\n", filename);
    }

    // We don't really expect anything coming afterwards from the machine, but
    // if there is an imbalance of sent commands vs. acknowledge flow control
    // tokens, we'd see it now.
    if (!is_dry_run) {
        if (log_info)
            fprintf(log_info, "Discarding remaining machine responses.\n");
        machine->DiscardPendingInput(
            initial_squash_chatter_ms,
            print_unusual_messages ? log_gcode : nullptr);
    }

    close(input_fd);

    if (log_info) {
        fprintf(log_info,
                "Sent total of %d non-empty lines in "
                "%" PRId64 ".%03" PRId64 "s\n",
                line_no, duration / 1000, duration % 1000);
    }
}
