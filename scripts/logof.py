#!/usr/bin/env python3
"""Print the newest mod log path for a console port. Usage: logof.py 27100"""
import os, sys, glob
D = os.path.expanduser('~/es2game/ES2/Binaries/Win64/ES2Coop')
want = sys.argv[1]
logs = sorted(glob.glob(os.path.join(D, 'logs', 'es2coop-*.log')), key=os.path.getmtime, reverse=True)
for f in logs:
    pid = os.path.basename(f)[len('es2coop-'):-len('.log')]
    pf = os.path.join(D, f'console-{pid}.port')
    try:
        if open(pf).read().strip() == want:
            print(f); sys.exit(0)
    except OSError:
        pass
sys.exit(1)
