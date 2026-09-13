#!/usr/bin/python3
"""Local UI test fixture only. Never installed; no network or real card access."""
import os
import select
import socket
import sys
import time

args = dict(zip(sys.argv[1::2], sys.argv[2::2]))
mode = args['--config'].rsplit('/', 1)[-1]
sock = socket.socket(fileno=int(args['--supervise-fd']))
pin_fd = int(args['--pin-fd'])
if mode == 'immediate':
    sock.send(b'HSPUI1 6 8 5000 5002 5004 1100 0 0 0 0 0 0 0 0 0 0')
    sys.exit(1)
state, reason, generation = 1, 0, 0
started = time.monotonic()
last = 0
pin = b''
while time.monotonic() - started < 20:
    now = time.monotonic()
    if now - last > .1 and not (mode == 'stale' and now - started > .3):
        if mode == 'malformed':
            sock.send(b'HSPUI99 invalid')
        else:
            sock.send(f'HSPUI1 {state} {reason} 5000 5002 5004 1100 {generation} 1 2 3 4 5 6 0 0 0'.encode())
        last = now
    read, _, _ = select.select([sock] + ([pin_fd] if pin_fd >= 0 else []), [], [], .02)
    for fd in read:
        if fd == sock:
            command = sock.recv(64)
            if not command or command == b'STOP':
                sys.exit(0)
            if command == b'REKEY':
                generation += 1
        else:
            data = os.read(pin_fd, 128)
            pin += data
            if b'\n' in pin:
                assert pin == b'1234\n', 'unexpected test credential'
                pin = b''
                os.close(pin_fd)
                pin_fd = -1
                if mode == 'badpin':
                    state, reason = 6, 4
                else:
                    state, generation = 5, 1
            elif not data:
                sys.exit(0)
