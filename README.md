Very simple way to send a gcode file on the command line to a printer/cnc
machine that uses the common `ok` and `error` feedback lines after each
block as 'flow control'.

It sends gcode line-by-line, removing CRLF line-endings and just sends LF
(otherwise Grbl gets confused). Waits for the acknowledging `ok` before sending
the next line; provides a simple continue/stop user interaction when it
encounters an `error`-response.

No claim to be complete, just useful for my local Marlin-based 3D printers and
Grbl-based CNC.

Usage:
```
 ./gcode-cli mygcodefile.gcode
```

Currently, this connects to `/dev/ttyACM0` with 115200 speed, but should
probably be made a flag :)