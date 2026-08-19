#!/usr/bin/env python3
"""TCP client for the ES2Coop in-game console.
Usage: console.py <port|name> <command ...>        one command, prints response
       console.py <port|name> -i                    interactive
       console.py <port|name> -f <file>             run each line of file
<name> resolves via run/<name>.port written by launch.sh or the ES2Coop/console-<pid>.port files (first match).
"""
import socket, sys, os, glob, time

def resolve_port(spec):
    if spec.isdigit(): return int(spec)
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    # name -> we stored the port in the launch: run/<name>.port? launch.sh exports ES2COOP_CONSOLE_PORT; mirror it here
    p = os.path.join(root, 'run', spec + '.port')
    if os.path.exists(p): return int(open(p).read().strip())
    raise SystemExit(f'unknown console spec {spec}')

def read_reply(sock):
    buf = b''
    while b'<<END>>\n' not in buf:
        chunk = sock.recv(65536)
        if not chunk: break
        buf += chunk
    txt = buf.decode('utf-8', 'replace')
    return txt.split('<<END>>\n', 1)[0]

def main():
    if len(sys.argv) < 3: print(__doc__); sys.exit(1)
    port = resolve_port(sys.argv[1])
    s = socket.create_connection(('127.0.0.1', port), timeout=60)
    read_reply(s)  # banner
    def run(cmd):
        s.sendall((cmd.strip() + '\n').encode())
        return read_reply(s)
    if sys.argv[2] == '-i':
        while True:
            try: line = input('es2> ')
            except EOFError: break
            if not line.strip(): continue
            print(run(line), end='')
    elif sys.argv[2] == '-f':
        for line in open(sys.argv[3]):
            if line.strip() and not line.startswith('#'):
                print('> ' + line.strip()); print(run(line), end='')
    else:
        print(run(' '.join(sys.argv[2:])), end='')

if __name__ == '__main__': main()
