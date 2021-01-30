/* -*- mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; -*-
 * (c) h.zeller@acm.org. Free Software. GNU Public License v3.0 and above
 */

#ifndef MACHINE_CONN_H
#define MACHINE_CONN_H

// Open a connection to a machine. The "descriptor" is a string describing
// the connection to the machine. This can be different ways to connect to
// a machine.
// Supported formats
//   - terminal: path, optional speed "/dev/ttyUSB0,b115200"
//   - "hostname:port"  (in fact: not yet supported, but needed for BeagleG)
//
// Returns a bi-directional file-descriptor or -1 if opening failed.
int OpenMachineConnection(const char *descriptor);

// While there is stuff readable on the file-descriptor, discard the input
// until there is silence on the wire for "timeout_ms". Helps to get into
// a clean state. Returns number of bytes discarded.
int DiscardPendingInput(int fd, int timeout_ms, bool echo_received_data);

#endif // MACHINE_CONN_H
