#!/usr/bin/env python3
"""Isolated GTK Broadway display: no real camera, smartcard, or desktop touched."""
import os
import pathlib
import shutil
import subprocess
import tempfile
import time

root = pathlib.Path(__file__).resolve().parent.parent
suffix = os.environ.get('UI_TEST_SUFFIX', '')
assert suffix in ('', '_sanitize')
if not shutil.which('broadwayd'):
    raise SystemExit('GTK3 broadwayd is required for check-ui')
with tempfile.TemporaryDirectory(prefix='gtk-pipe-ui-') as tmp:
    env = dict(os.environ, XDG_RUNTIME_DIR=tmp, GDK_BACKEND='broadway', BROADWAY_DISPLAY=':91',
               GSETTINGS_BACKEND='memory', NO_AT_BRIDGE='1', G_DEBUG='fatal-warnings')
    display = subprocess.Popen(['broadwayd', '--unixsocket', tmp + '/http.socket', ':91'],
                               env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        for _ in range(100):
            if pathlib.Path(tmp, 'broadway91.socket').exists():
                break
            if display.poll() is not None:
                raise RuntimeError(display.communicate())
            time.sleep(.02)
        subprocess.run([str(root / ('tests/test_secure' + suffix)), str(root / 'tests/fake-backend.py')],
                       cwd=root, env=env, check=True, timeout=30)
        subprocess.run([str(root / ('tests/test_app' + suffix))], cwd=root, env=env, check=True, timeout=15)
    finally:
        display.terminate()
        display.communicate(timeout=3)
