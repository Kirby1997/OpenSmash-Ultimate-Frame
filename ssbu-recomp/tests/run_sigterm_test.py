#!/usr/bin/env python3
"""scripts/run.py run_game (Linux builds): SIGTERM sent to the launcher reaches the child, and its exit code comes back."""
import os, signal, subprocess, sys, time, textwrap
here = os.path.dirname(os.path.abspath(__file__))
code = textwrap.dedent('''
    import os, signal, sys, time
    sys.path.insert(0, %r)
    import run
    c = [sys.executable, "-c", "import signal,sys,time\\nsignal.signal(signal.SIGTERM, lambda *a: sys.exit(7))\\nprint('up', flush=True)\\ntime.sleep(30)"]
    print("rc", run.run_game(c, ".", dict(os.environ), None, True).returncode, flush=True)
''' % os.path.join(here, '..', 'scripts'))
p = subprocess.Popen([sys.executable, '-c', code], stdout=subprocess.PIPE, text=True)
time.sleep(2)
p.send_signal(signal.SIGTERM)
out = p.communicate(timeout=15)[0]
assert 'rc 7' in out, out
print('run_sigterm_test: ok')
