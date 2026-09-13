"""Run the packaged, non-elevated GUI twin on Windows with a startup deadline."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

if os.name != 'nt':
    raise SystemExit('This execution test needs a Windows runner.')
package = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory() as temp:
    screenshot = Path(temp) / 'startup.png'
    env = dict(os.environ, QT_QPA_PLATFORM='offscreen')
    subprocess.run([str(package / 'zll-startup-test.exe'), '--preview', '--screenshot', str(screenshot)],
                   env=env, check=True, timeout=20)
    assert screenshot.read_bytes().startswith(b'\x89PNG\r\n\x1a\n'), 'GUI did not render its first window'
print('PASS: WinMain, QApplication, main window, event loop and clean exit')
