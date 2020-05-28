Very simple way to send a gcode file on the command line to a printer/cnc
machine that uses the common `ok` and `error` feedback lines after each
block as 'flow control'.

It sends gcode line-by-line, removing CRLF line-endings and just sends LF
(otherwise Grbl gets confused). Waits for the acknowledging `ok` before sending
the next line; provides a simple continue/stop user interaction when it
encounters an `error`-response.

No claim to be complete, just useful for my local Marlin-based 3D printers and
Grbl-based CNC.

```
usage:
./gcode-cli <gcode-file> [connection-string]

Connection string is comprised of device-name and an optional
bit-rate directly separated with a comma.
These are valid connection strings; notice the 'b' prefix for the bit-rate:
        /dev/ttyACM0
        /dev/ttyACM0,b115200
Available bit-rates are one of [9600, 19200, 38400, 57600, 115200, 230400, 460800]

Example:
./gcode-cli file.gcode /dev/ttyACM0,b115200
```
