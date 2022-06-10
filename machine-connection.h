/* -*- mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * (c) h.zeller@acm.org. Free Software. GNU Public License v3.0 and above
 */

#ifndef MACHINE_CONN_H
#define MACHINE_CONN_H

#include <stdio.h>

#include <string_view>
#include <vector>

#include "buffered-line-reader.h"

class MachineConnection {
   public:
    // Open a connection to a machine. The "descriptor" is a string describing
    // the connection to the machine. This can be different ways to connect to
    // a machine.
    // Supported formats
    //   - terminal: path, optional speed "/dev/ttyUSB0,b115200"
    //   - "hostname:port"  (in fact: not yet supported, but needed for BeagleG)
    // Can return nullptr on failure.
    static MachineConnection *Open(const char *descriptor);

    ~MachineConnection();

    // While there is stuff readable on the input, discard the input
    // until there is silence on the wire for "timeout_ms".
    // Helps to get into a clean initial state as many machines have some
    // initial chatter they produce.
    // If "echo_discarded" is no null, prints out what it sees on that stream.
    // Returns number of bytes discarded.
    int DiscardPendingInput(int timeout_ms, FILE *echo_discarded);

    // Write all provided blocks to the machine.
    // Needs "scratch_buffer" to be large enough to contain all of them.
    // (TODO: remove scratch buffer, that looks like leaking implementation
    // details)
    bool WriteBlocks(char *scratch_buffer,
                     const std::vector<std::string_view> &blocks);

    // Get a line buffer reader returning responses from the machine.
    BufferedLineReader &ResponseLines() { return reader_; }

   private:
    MachineConnection(int to_machine, int from_machine);
    const int output_fd_;
    const int input_fd_;
    BufferedLineReader reader_;
};

#endif  // MACHINE_CONN_H
