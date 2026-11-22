#!/usr/bin/env python3
"""scripts/run.py run_game (Linux builds): a SIGTERM sent to the launcher reaches the child once and its exit code comes
back; a SIGTERM that arrives while the child is being started is passed on as soon as it exists; a child ended by a
signal gives the launcher 128 + N (exit_code)."""
import os, signal, subprocess, sys, textwrap
here = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.join(here, '..', 'scripts')
CHILD = ("import signal,sys,time\\n"
         "n = []\\n"
         "def h(*a):\\n    n.append(1)\\n    time.sleep(0.5)\\n    sys.exit(6 + len(n))\\n"     # 7: one SIGTERM, 8: two
         "signal.signal(signal.SIGTERM, h)\\nprint('up', flush=True)\\ntime.sleep(30)")


def launcher(body):
    return subprocess.Popen([sys.executable, '-c', textwrap.dedent('''
        import os, signal, subprocess, sys, time
        sys.path.insert(0, %r)
        import run
    ''' % SCRIPTS) + textwrap.dedent(body)], stdout=subprocess.PIPE, text=True)


bad = []
# 1. SIGTERM to the launcher while the game runs: the game gets it once, the launcher returns the game's code
p = launcher('''
    c = [sys.executable, "-c", %r]
    print("rc", run.run_game(c, ".", dict(os.environ), None, True).returncode, flush=True)
''' % CHILD.replace('\\n', '\n'))
if p.stdout.readline().strip() != 'up':
    bad.append('the child did not start')
p.send_signal(signal.SIGTERM)
out = p.communicate(timeout=15)[0]
if 'rc 7' not in out:
    bad.append('relay: %r (rc 7 = one SIGTERM, rc 8 = two)' % out)
# 2. SIGTERM while the game is being started (before Popen returns): passed on once it exists, launcher not killed
p = launcher('''
    real = subprocess.Popen
    def popen(*a, **k):
        os.kill(os.getpid(), signal.SIGTERM)
        return real(*a, **k)
    run.subprocess.Popen = popen
    c = [sys.executable, "-c", %r]
    print("rc", run.run_game(c, ".", dict(os.environ), None, True).returncode, flush=True)
''' % CHILD.replace('\\n', '\n'))
out = p.communicate(timeout=15)[0]
if p.returncode or 'rc ' not in out or out.split('rc ')[-1].strip() not in ('7', '-15'):
    bad.append('a SIGTERM during the start: launcher %r, %r' % (p.returncode, out))
# 3. a game ended by a signal: 128 + N, as a shell reports it
sys.path.insert(0, SCRIPTS)
import run  # noqa: E402
if (run.exit_code(-15), run.exit_code(-9), run.exit_code(0), run.exit_code(3)) != (143, 137, 0, 3):
    bad.append('exit_code: %r' % ((run.exit_code(-15), run.exit_code(-9)),))
for b in bad:
    print('FAILED: ' + b)
print('run_sigterm_test: %s' % ('ok' if not bad else 'FAILED'))
sys.exit(1 if bad else 0)
