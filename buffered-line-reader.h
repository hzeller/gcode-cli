/* -*- mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * (c) h.zeller@acm.org. Free Software. GNU Public License v3.0 and above
 */

#ifndef GCODE_LINE_READER_H
#define GCODE_LINE_READER_H

#include <string_view>
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
class BufferedLineReader {
public:
    BufferedLineReader(int fd, size_t buffer_size, bool remove_comments);
    ~BufferedLineReader();

    // Read at most 'n' next lines (= GCode blocks) from the input.
    // Might return less.
    // The lines returned are cleaned from end-of-line comments after ';' and
    // unnecessary whitespace at beginning and end.
    // Invalidates string_views returned by previous calls.
    std::vector<std::string_view> ReadNextLines(size_t n);

    // Convenience: read a single line.
    std::string_view ReadLine();

    // Return if the full file has been processed.
    inline bool is_eof() const { return eof_; }

private:
    // May modify the content of the buffer to place a fresh newline.
    std::string_view MakeCommentFreeLine(char *first, char *last);
    bool Refill();

    const int fd_;
    const size_t buffer_size_;
    char *const buffer_;
    const bool remove_comments_;

    bool eof_ = false;
    char *data_begin_;
    char *data_end_;
    std::string_view remainder_;  // incomplete line at end of buffer
};

#endif // GCODE_LINE_READER_H
