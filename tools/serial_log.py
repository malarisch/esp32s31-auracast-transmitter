#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Capture the board's UART log for a fixed time (non-interactive).

Usage: python tools/serial_log.py --port PORT --seconds N [--reset]

--reset pulses EN through the CP2102N auto-program circuit (RTS -> EN) so the
capture starts at boot. DTR stays deasserted, so the chip boots from flash.
"""
import argparse
import sys
import time

import serial


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--seconds', type=float, required=True)
    parser.add_argument('--reset', action='store_true')
    parser.add_argument('--baud', type=int, default=921600)
    args = parser.parse_args()

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.timeout = 0.2
    # Deassert both lines before opening so opening the port does not reset or strap the chip
    ser.dtr = False
    ser.rts = False
    ser.open()

    if args.reset:
        ser.dtr = False
        ser.rts = True   # EN low
        time.sleep(0.1)
        ser.rts = False  # EN high -> boot from flash
        ser.reset_input_buffer()

    deadline = time.monotonic() + args.seconds
    pending = b''
    try:
        while time.monotonic() < deadline:
            pending += ser.read(4096)
            *lines, pending = pending.split(b'\n')
            for line in lines:
                sys.stdout.write(line.decode('utf-8', errors='replace').rstrip('\r') + '\n')
            sys.stdout.flush()
    finally:
        if pending:
            sys.stdout.write(pending.decode('utf-8', errors='replace') + '\n')
        ser.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
