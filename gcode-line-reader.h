/* -*- mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * (c) h.zeller@acm.org. Free Software. GNU Public License v3.0 and above
 */

#ifndef GCODE_LINE_READER_H
#define GCODE_LINE_READER_H

#include <string_view>
#include <memory>
#include <vector>

// Reader of gcode input yielding preprocessed blocks without comments or
// unnecesary whitespace.
//
// This reads in larger chunks (up to "buffer_size") from the file-descriptor
// and provides an array of pre-tokenized lines.
//
// If "remove_comments" is set, semicolon end-of-block comments are removed.
// Leading and trailing whitespace removed.
// Line-endings '\r\n' or '\n' are canonicalized to be exactly one '\n'.
// Empty lines are removed.
class GCodeLineReader {
public:
    GCodeLineReader(int fd, size_t buffer_size, bool remove_comments);
    ~GCodeLineReader();

    bool is_eof() const { return eof_; }

    // Read next lines (= GCode blocks) from the input.
    // The lines returned are cleaned from end-of-line comments after ';' and
    // unnecessary whitespace at beginning and end.
    // Invalidates string_views from previous calls.
    std::vector<std::string_view> ReadNextLines();

private:
    // May modify the content of the buffer to place a fresh newline.
    std::string_view MakeCommentFreeLine(const char *begin, char *end);

    const int fd_;
    const size_t buffer_size_;
    std::unique_ptr<char[]> const buffer_;
    const bool remove_comments_;

    bool eof_ = false;
    std::string_view remainder_;  // incomplete line at the end.
};

#endif // GCODE_LINE_READER_H
