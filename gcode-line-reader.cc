/* -*- mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * (c) h.zeller@acm.org. Free Software. GNU Public License v3.0 and above
 */

#include "gcode-line-reader.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

GCodeLineReader::GCodeLineReader(int fd, size_t buffer_size,
                                 bool remove_comments)
    : fd_(fd), buffer_size_(buffer_size), buffer_(new char[buffer_size]),
      remove_comments_(remove_comments) {
}

GCodeLineReader::~GCodeLineReader() { close(fd_); }

std::vector<std::string_view> GCodeLineReader::ReadNextLines() {
    std::vector<std::string_view> result;
    if (eof_) return result;
    char *buf = buffer_.get();
    if (!remainder_.empty()) {
        // get leftover to the beginning
        memmove(buf, remainder_.data(), remainder_.size());
    }
    const ssize_t r = read(fd_, buf + remainder_.size(),
                           buffer_size_ - remainder_.size());
    if (r <= 0) {
        eof_ = true;
        if (r < 0) perror("Reading input");
        // Create a full line out of the remainder.
        if (!remainder_.empty()) {
            char *end = buf + remainder_.size();
            *end = '\n';
            auto line = MakeCommentFreeLine(buf, end);
            if (!line.empty()) result.push_back(line);
        }
        return result;
    }
    const char *end = buf + remainder_.size() + r;
    char *start_line = buf;
    char *end_line;
    while ((end_line = (char*)memchr(start_line, '\n', end - start_line))) {
        auto line = MakeCommentFreeLine(start_line, end_line);
        if (!line.empty()) result.push_back(line);
        start_line = end_line + 1;
    }
    remainder_ = std::string_view(start_line, end - start_line);
    return result;
}

std::string_view GCodeLineReader::MakeCommentFreeLine(
    const char *begin, char *end)
{
    if (remove_comments_) {
        char *start_of_comment = (char*) memchr(begin, ';', end - begin);
        if (start_of_comment) end = start_of_comment - 1;
    }
    while (begin < end && isspace(*begin)) begin++;
    while (end > begin && isspace(*end)) end--;  // There is at least one
    if (end <= begin) return {};
    *++end = '\n';  // Fresh newline behind resulting new end.
    return { begin, (size_t) (end - begin + 1)};
}
