#!/usr/bin/env python3
# A stand-in for LiveSplit's Server component (tests of runtime/livesplit.c): listens on a TCP port and logs every
# command line it receives, with the time since it started. tools/livesplit_mock.py [port] [log file]
import socket, sys, threading, time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 16834
out = open(sys.argv[2], 'a', buffering=1) if len(sys.argv) > 2 else sys.stdout
t0 = time.time()


def log(msg):
    out.write('%8.3f %s\n' % (time.time() - t0, msg))


def client(c, addr):
    log('connect %s:%d' % addr)
    buf = b''
    while True:
        d = c.recv(4096)
        if not d:
            break
        buf += d
        while b'\r\n' in buf:
            line, buf = buf.split(b'\r\n', 1)
            log('cmd ' + line.decode(errors='replace'))
    log('disconnect')
    c.close()


s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', port))
s.listen(4)
log('listening on %d' % port)
while True:
    c, addr = s.accept()
    threading.Thread(target=client, args=(c, addr), daemon=True).start()
