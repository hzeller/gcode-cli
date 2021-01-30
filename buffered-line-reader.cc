/* -*- mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * (c) h.zeller@acm.org. Free Software. GNU Public License v3.0 and above
 */

#include "buffered-line-reader.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <algorithm>

BufferedLineReader::BufferedLineReader(int fd, size_t buffer_size,
                                 bool remove_comments)
    : fd_(fd), buffer_size_(buffer_size), buffer_(new char[buffer_size]),
      remove_comments_(remove_comments),
      data_begin_(buffer_), data_end_(buffer_) {
}

BufferedLineReader::~BufferedLineReader() {
    delete [] buffer_;
}

bool BufferedLineReader::Refill() {
    data_begin_ = buffer_;
    data_end_ = data_begin_;
    if (eof_) return false;
    if (!remainder_.empty()) {
        // get remainder to the beginning of the buffer.
        memmove(data_begin_, remainder_.data(), remainder_.size());
        data_end_ += remainder_.size();
    }
    ssize_t r = read(fd_, data_end_, buffer_size_ - remainder_.size());
    if (r > 0) {
        data_end_ += r;
    } else {
        eof_ = true;
        if (r < 0) perror("Reading input");
        if (!remainder_.empty()) {
            // Close remainder with a newline
            *data_end_++ = '\n';
        }
    }
    remainder_ = {};
    return data_end_ > data_begin_;
}

std::vector<std::string_view> BufferedLineReader::ReadNextLines(size_t n) {
    std::vector<std::string_view> result;
    result.reserve(n);
    if (data_begin_ >= data_end_ && !Refill())
        return result;
    char *end_line;
    while ((end_line = std::find_if(data_begin_, data_end_,
                                    [](char c) { return c == '\n'||c == '\r'; }
                                    )) != data_end_) {
        auto line = MakeCommentFreeLine(data_begin_, end_line);
        if (!line.empty()) result.push_back(line);
        data_begin_ = end_line + 1;
        if (result.size() >= n) {
            return result;
        }
    }
    remainder_ = std::string_view(data_begin_, data_end_ - data_begin_);
    data_begin_ = data_end_;  // consume all.
    return result;
}

std::string_view BufferedLineReader::ReadLine() {
    while (!is_eof()) {
        auto lines = ReadNextLines(1);
        if (lines.empty()) continue;  // could happen at buffer switchover
        return lines[0];
    }
    return {};
}

// Note, 'last' points to the last character (typically the newline), not the
// last character + 1 as one would assume in iterators.
std::string_view BufferedLineReader::MakeCommentFreeLine(
    char *first, char *last) {
    if (remove_comments_) {
        char *start_of_comment = (char*) memchr(first, ';', last - first + 1);
        if (start_of_comment) last = start_of_comment - 1;
    }
    while (first <= last && isspace(*first)) first++;
    while (last >= first && isspace(*last)) last--;  // also removing newline
    if (last < first) return {};
    *++last = '\n';  // Fresh newline behind resulting new last.
    return std::string_view(first, last - first + 1);
}
