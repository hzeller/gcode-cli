/* -*- mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * (c) h.zeller@acm.org. Free Software. GNU Public License v3.0 and above
 */

#include "machine-connection.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <charconv>
#include <map>
#include <string>

#if defined(__APPLE__) && !defined(USE_TERMIOS)
#define USE_TERMIOS
#endif

#ifdef USE_TERMIOS
#include <termios.h>
#else
#include <asm/termbits.h>
#include <sys/ioctl.h>
#endif

#ifdef USE_TERMIOS
using tty_termios_t = struct termios;
#else
using tty_termios_t = struct termios2;
#endif

#ifdef USE_TERMIOS
// With termios, there is only a distinct set of available speeds.
static std::map<int, speed_t> AvailableTTYSpeeds();
#endif

static bool SetTTYSpeed(tty_termios_t *tty, int speed_number) {
    if (speed_number < 0) {
        fprintf(stderr, "Invalid speed %d\n", speed_number);
        return false;
    }
#ifdef USE_TERMIOS
    speed_t speed = B115200;
    const std::map<int, speed_t> selectable_speeds = AvailableTTYSpeeds();
    const auto found = selectable_speeds.find(speed_number);
    if (found == selectable_speeds.end()) {
        fprintf(stderr, "Invalid speed '%d'; valid speeds are [", speed_number);
        const char *sep = "";
        for (const auto &[speed, _] : selectable_speeds) {
            fprintf(stderr, "%s%d", sep, speed);
            sep = ", ";
        }
        fprintf(stderr, "]\n");
        return false;
    }
    speed = found->second;
    if (cfsetospeed(tty, speed) < 0 || cfsetispeed(tty, speed) < 0) {
        fprintf(stderr, "While attempting to set speed to %d bps\n",
                speed_number);
        perror("Issue while setting tty-speed");
        return false;
    }
#else
    /* Clear the current output baud rate and fill a new value */
    tty->c_cflag &= ~CBAUD;
    tty->c_cflag |= BOTHER;
    tty->c_ospeed = speed_number;

    /* Clear the current input baud rate and fill a new value */
    tty->c_cflag &= ~(CBAUD << IBSHIFT);
    tty->c_cflag |= BOTHER << IBSHIFT;
    tty->c_ispeed = speed_number;
#endif

    return true;
}

static bool SetTTYParams(int fd, std::string_view parameters) {
    tty_termios_t tty;

#ifdef USE_TERMIOS
    if (tcgetattr(fd, &tty) < 0) {
        perror("tcgetattr() failed. Is this a tty ?");
        return false;
    }
#else
    if (ioctl(fd, TCGETS2, &tty)) {
        perror("ioctl(TCGETS2) failed. Is this a tty ?");
        return false;
    }
#endif

    // Some generic settings, possibly overridden later
    tty.c_cflag |= (CLOCAL | CREAD);  // no modem controls
    tty.c_cflag &= ~CSIZE;            // Reset size as we want to choose ..
    tty.c_cflag |= CS8;               // 8 .. bits
    tty.c_cflag &= ~PARENB;           // N
    tty.c_cflag &= ~CSTOPB;           // 1
    tty.c_cflag |= CRTSCTS;           // Hardware flow-control

    // Terminal magic. Non-canonical mode
    tty.c_iflag &=
        ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tty.c_oflag &= ~OPOST;

    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 1;

    SetTTYSpeed(&tty, 115200);
    while (!parameters.empty()) {
        const auto pos = parameters.find(',');
        const auto part_len =
            (pos != std::string_view::npos) ? pos + 1 : parameters.size();
        std::string_view param = parameters.substr(0, part_len);
        parameters.remove_prefix(part_len);

        if (param[0] == 'b' || param[0] == 'B') {
            int s;
            if (auto r = std::from_chars(param.begin() + 1, param.end(), s);
                r.ec == std::errc()) {
                if (!SetTTYSpeed(&tty, s)) return false;
            }
            continue;
        }

        // Flags can be with optional positive or negative prefix.
        bool flag_positive = true;
        if (param[0] == '+') {
            param = param.substr(1);
        } else if (param[0] == '-') {
            flag_positive = false;
            param = param.substr(1);
        }

        if (param == "crtscts") {
            if (flag_positive) {
                tty.c_cflag |= CRTSCTS;
            } else {
                tty.c_cflag &= ~CRTSCTS;
            }
        } else {
            fprintf(stderr, "Unknown option %.*s\n", (int)param.size(),
                    param.data());
            return false;
        }
    }

#ifdef USE_TERMIOS
    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        printf("Error from tcsetattr: %s\n", strerror(errno));
        return false;
    }
#else
    if (ioctl(fd, TCSETS2, &tty)) {
        printf("ioctl(TCSETS2) failed: %s\n", strerror(errno));
        return false;
    }
#endif
    return true;
}

// Wait for input to become ready for read or timeout reached.
// If the file-descriptor becomes readable, returns number of milli-seconds
// left.
// Returns 0 on timeout (i.e. no millis left and nothing to be read).
// Returns -1 on error.
static int AwaitReadReady(int fd, int timeout_millis) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(fd, &read_fds);

    struct timeval tv;
    tv.tv_sec = timeout_millis / 1000;
    tv.tv_usec = (timeout_millis % 1000) * 1000;

    FD_SET(fd, &read_fds);
    int s = select(fd + 1, &read_fds, NULL, NULL, &tv);
    if (s < 0) return -1;
    return tv.tv_usec / 1000;
}

/*
 *
 *  Public interface functions
 *
 */

int OpenTCPSocket(const char *host) {
    char *host_copy = NULL;
    const char *port = "8888";
    const char *colon_pos;
    if ((colon_pos = strchr(host, ':')) != NULL) {
        port = colon_pos + 1;
        host_copy = strdup(host);
        host_copy[colon_pos - host] = '\0';
        host = host_copy;
    }
    struct addrinfo addr_hints = {};
    addr_hints.ai_family = AF_INET;
    addr_hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *addr_result = NULL;
    int rc;
    if ((rc = getaddrinfo(host, port, &addr_hints, &addr_result)) != 0) {
        // We're in OpenTCPSocket(), because opening as a tty failed before,
        // so make reference of that in this error message.
        fprintf(stderr,
                "Not a tty and "
                "can't resolve as TCP endpoint '%s' (port %s): %s\n",
                host, port, gai_strerror(rc));
        free(host_copy);
        return -1;
    }
    free(host_copy);
    if (addr_result == NULL) return -1;
    int fd = socket(addr_result->ai_family, addr_result->ai_socktype,
                    addr_result->ai_protocol);
    if (fd >= 0 &&
        connect(fd, addr_result->ai_addr, addr_result->ai_addrlen) < 0) {
        perror("TCP connect()");
        close(fd);
        fd = -1;
    }

    freeaddrinfo(addr_result);
    return fd;
}

static int OpenTTY(std::string_view descriptor) {
    auto first_comma = descriptor.find(',');
    const std::string path(descriptor.substr(0, first_comma));
    const std::string_view tty_params = (first_comma != std::string_view::npos)
                                            ? descriptor.substr(first_comma + 1)
                                            : "";
    const int fd = open(path.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) {
        return -1;
    }
    if (!SetTTYParams(fd, tty_params)) {
        return -1;
    }
    return fd;
}

MachineConnection::MachineConnection(int to_machine, int from_machine)
    : output_fd_(to_machine),
      input_fd_(from_machine),
      reader_(input_fd_, (1 << 16), false) {}

MachineConnection::~MachineConnection() { close(output_fd_); }

MachineConnection *MachineConnection::Open(const char *descriptor) {
    if (descriptor == nullptr) return nullptr;
    if (strcmp(descriptor, "-") == 0) {
        return new MachineConnection(STDOUT_FILENO, STDIN_FILENO);
    }
    if (const int fd = OpenTTY(descriptor); fd >= 0) {
        return new MachineConnection(fd, fd);
    }
    if (const int fd = OpenTCPSocket(descriptor); fd >= 0) {
        return new MachineConnection(fd, fd);
    }
    return nullptr;
}

int MachineConnection::DiscardPendingInput(int timeout_ms,
                                           FILE *echo_discarded) {
    if (input_fd_ < 0) return 0;
    int total_bytes = 0;
    char buf[128];
    while (AwaitReadReady(input_fd_, timeout_ms) > 0) {
        int r = read(input_fd_, buf, sizeof(buf));
        if (r < 0) {
            perror("reading trouble");
            return -1;
        }
        total_bytes += r;
        if (r > 0 && echo_discarded) {
            fwrite(buf, r, 1, echo_discarded);
        }
    }
    return total_bytes;
}

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
bool MachineConnection::WriteBlocks(
    char *scratch_buffer, const std::vector<std::string_view> &blocks) {
    // Note: not using writev(), as snippets can be a lot and writev() has a
    // bunch of limitations (e.g. UIO_MAXIOV == 1024). Thus simply
    // reassmbling into buffer.
    char *pos = scratch_buffer;
    for (auto block : blocks) {
        memcpy(pos, block.data(), block.size());
        pos += block.size();
    }
    return reliable_write(output_fd_, scratch_buffer, pos - scratch_buffer);
}

#ifdef USE_TERMIOS
// Termios specifies speeds with macros and there is no easy way to figure
// out which exist, and if the speed_t is actually just the number itself, as
// this is hidden in the API abstraction.
// This builds a map of available speeds to the corresponding Bxxx constant.
static std::map<int, speed_t> AvailableTTYSpeeds() {
    std::map<int, speed_t> result;
    // The following are from the manpage. There are more at the lower end,
    // but they are not really used these days.
#ifdef B1200
    result[1200] = B1200;
#endif
#ifdef B1800
    result[1800] = B1800;
#endif
#ifdef B2400
    result[2400] = B2400;
#endif
#ifdef B4800
    result[4800] = B4800;
#endif
#ifdef B9600
    result[9600] = B9600;
#endif
#ifdef B19200
    result[19200] = B19200;
#endif
#ifdef B38400
    result[38400] = B38400;
#endif
#ifdef B57600
    result[57600] = B57600;
#endif
#ifdef B76800
    result[76800] = B76800;
#endif
#ifdef B115200
    result[115200] = B115200;
#endif
#ifdef B153600
    result[153600] = B153600;
#endif
#ifdef B230400
    result[230400] = B230400;
#endif
#ifdef B250000
    result[250000] = B250000;
#endif
#ifdef B307200
    result[307200] = B307200;
#endif
#ifdef B460800
    result[460800] = B460800;
#endif
#ifdef B500000
    result[500000] = B500000;
#endif
#ifdef B576000
    result[576000] = B576000;
#endif
#ifdef B614400
    result[614400] = B614400;
#endif
#ifdef B921600
    result[921600] = B921600;
#endif
#ifdef B1000000
    result[1000000] = B1000000;
#endif
#ifdef B1152000
    result[1152000] = B1152000;
#endif
#ifdef B1500000
    result[1500000] = B1500000;
#endif
#ifdef B2000000
    result[2000000] = B2000000;
#endif
#ifdef B2500000
    result[2500000] = B2500000;
#endif
#ifdef B3000000
    result[3000000] = B3000000;
#endif
#ifdef B3500000
    result[3500000] = B3500000;
#endif
#ifdef B4000000
    result[4000000] = B4000000;
#endif
    return result;
}
#endif
