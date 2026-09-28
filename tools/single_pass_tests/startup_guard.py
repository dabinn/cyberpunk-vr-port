"""Reject the known tiny-window startup before arming renderer experiments."""
import json
from pathlib import Path
import subprocess
import sys

def require_normal_window(pid):
    root=Path(__file__).resolve().parents[2]
    process=subprocess.run([sys.executable,'-B',str(root/'tools/quickboot_tests/windows.py'),'--pid',str(pid)],
        capture_output=True,text=True,check=True)
    state=json.loads(process.stdout)
    windows=[w for w in state['windows'] if w['class']=='W2ViewportClass']
    if not windows or state['launcherVisible']:
        raise RuntimeError('Game window is not ready; no renderer probe was armed')
    if state['smallGameWindow']:
        raise RuntimeError('Known small-window startup: close this game PID and relaunch before testing')
    return [{'client':w['client'],'minimized':w['minimized']} for w in windows]
