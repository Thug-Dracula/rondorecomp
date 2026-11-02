#!/usr/bin/env python3
"""Line-buffered logger for the analysis tools.

Writes `logs/re.log` at the repository root, flushing every line so the file is
readable while a job is still running.  RE_LOG overrides the path; RE_VERBOSE=1
adds debug-level detail.
"""
import os, sys, time, threading

HERE = os.path.dirname(os.path.abspath(__file__))
LOG_PATH = os.environ.get("RE_LOG") or os.path.normpath(
    os.path.join(HERE, "..", "logs", "re.log"))
VERBOSE = os.environ.get("RE_VERBOSE", "") not in ("", "0")
_lock = threading.Lock()
_announced = False


def _open():
    global _announced
    os.makedirs(os.path.dirname(LOG_PATH), exist_ok=True)
    f = open(LOG_PATH, "a", encoding="utf-8", buffering=1)  # line buffered
    if not _announced:
        _announced = True
        f.write("#" * 70 + "\n")
        f.write("# re.log  pid=%d  verbose=%s\n" % (os.getpid(), VERBOSE))
        f.write("#" * 70 + "\n")
        f.flush()
        sys.stderr.write("[re] logging to %s\n" % LOG_PATH)
        sys.stderr.flush()
    return f


_fh = _open()


def log(msg, level="info"):
    if level == "debug" and not VERBOSE:
        return
    with _lock:
        _fh.write("%s %-5s %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), level.upper(), msg))
        _fh.flush()


def echo(msg):
    """Log and also print to stdout (for interactive use)."""
    log(msg)
    print(msg)
    sys.stdout.flush()


def debug(msg): log(msg, "debug")
def warn(msg): log(msg, "warn")
def err(msg): log(msg, "error")


def tail(n=20):
    with open(LOG_PATH, encoding="utf-8", errors="replace") as f:
        return f.readlines()[-n:]
