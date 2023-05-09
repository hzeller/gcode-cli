# gcode-cli - Simple command line tool to send gcode
Very simple way to send a gcode file on the command line to a printer/cnc
machine that uses the common `ok` and `error` feedback lines after each
block as 'flow control'.

It sends gcode line-by-line, removing CRLF line-endings and just sends LF
(otherwise Grbl gets confused).
Waits for the acknowledging `ok` before sending the next line; provides a
simple continue/stop user interaction when it encounters an `error`-response.

The tool removes `;`-based end-of-line comments and empty lines.

No claim to be complete, just useful for my local Marlin-based 3D printers and
Grbl-based CNC as well as various machines I run with [BeagleG].

## Connection options
Connection to the machine can be done in three ways, defined by the
connection string (see `gcode-cli -h` for help).

  * Serial interface. Probably the most common way to connect to a machine.
    _Typical connection string:_ `/dev/ttyUSB0,b115200`
  * TCP connection: Giving a hostname and port, will connect to the machine
    via the network. _Typical connection string:_ `my-cnc-machine.local:4444`
  * stdin/stdout: this will write output to stdout and reads feeback from
    the machine via stdin. Use this if you wrap the communication via some
    other tool, e.g. socat. _Connection string:_ `-`.

## Flow Control
There are two levels of flow control

### Hardware flow control
This is the flow control that is implemented by the serial interface. It
allows the machine to indicate when its internal buffers are full so that
the sending side waits until new data can be sent.
The `gcode-cli` tool uses this by default in the communication with serial
machines.
This can be switched off with the `-crtscts` connection string option.

### Protocol flow control
On the gcode level, there is another protoccol that can be seen as
flow control. Whenever a block (= a line) is processed, the machine
sends back a line with `ok`, or, if there is an issue `error`.

By default, `gcode-cli` uses that feedback to moderate the data stream.

The settings are conservative by default: maximum one outstanding block, so
a block is only sent if the previous block was acknowledged with 'ok'.
You can change the number of outstanding blocks with the `-b` option.
With `-F`, you can switch off protoccol flow control entirely.

Changing `-b` or even `-F` makes sense if the machine can handle more
outstanding blocks and/or if hardware flow control is active.

```
Usage:
gcode-cli [options] <gcode-file> [<connection-string>]
Options:
        -s <millis> : Wait this time for init chatter from machine to subside.
                      Default: 2500
        -b <count>  : Number of blocks sent out buffered before
                      checking the returning flow-control 'ok'.
                      Careful, low memory machines might drop data.
                      Default: 1
        -c : Include semicolon end-of-line comments (they are stripped
             by default)
        -n : Dry-run. Read GCode but don't actually send anything.
        -q : Quiet. Don't output diagnostic messages or echo regular communication.
             Apply -q twice to even suppress non-handshake communication.
        -F : Disable waiting for 'ok'-acknowledge flow-control.

<gcode-file> is either a filename or '-' for stdin


<connection-string> is either a path to a tty device, a host:port or '-'
 * Serial connection
   A path to the device name with an optional bit-rate
   separated with a comma.
   Examples of valid connection strings:
        /dev/ttyACM0
        /dev/ttyACM0,b115200
   notice the 'b' prefix for the bit-rate. (any value allowed supported by system)
        /dev/ttyACM0,b115200,+crtscts
   Enable hardware flow control RTS/CTS handshaking.
        /dev/ttyACM0,b115200,-crtscts
   With a minus prefix, disable hardware flow control.

 * TCP connection
   For devices that receive gcode via tcp (e.g. http://beagleg.org/)
   you specify the connection string as host:port. Example:
        localhost:4444

 * stdin/stdout
   For a simple communication writing to the machine to stdout
   and read responses from stdin, use '-'
   This is useful for debugging or wiring up using e.g. socat.

Examples:
gcode-cli file.gcode /dev/ttyACM0,b115200
gcode-cli file.gcode localhost:4444
```

[BeagleG]: http://beagleg.org/
