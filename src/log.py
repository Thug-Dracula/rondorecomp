#!/usr/bin/env python3
"""Logging for the recompiler and its tools.

Every entry point calls `announce()` first, so the log path is never a mystery.
Writes are line-buffered *and* flushed per line: a log that only lands on disk at
clean exit is useless for diagnosing a hang, which is the case that matters.

Env:
  XLATE_LOG       override path
  XLATE_VERBOSE   1 = echo each line to stdout as well
"""
import os
import sys
import time

DEFAULT_PATH = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "logs", "xlate.log"))
MAX_BYTES = 4 * 1024 * 1024


class Logger:
    def __init__(self, path=None, verbose=None):
        self.path = path or os.environ.get("XLATE_LOG") or DEFAULT_PATH
        if verbose is None:
            verbose = os.environ.get("XLATE_VERBOSE", "") not in ("", "0")
        self.verbose = verbose
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        self._rotate_if_needed()
        # buffering=1 => line buffered; we still flush explicitly per line so a
        # hard kill cannot lose the last event.
        self.fh = open(self.path, "a", encoding="utf-8", buffering=1)

    def _rotate_if_needed(self):
        try:
            if os.path.getsize(self.path) > MAX_BYTES:
                os.replace(self.path, self.path + ".1")
        except FileNotFoundError:
            pass

    def announce(self):
        msg = "log file: %s" % self.path
        print(msg)
        sys.stdout.flush()
        self.log("session start (%s)" % time.strftime("%Y-%m-%d %H:%M:%S"))
        return self.path

    def log(self, msg):
        line = "%s  %s" % (time.strftime("%H:%M:%S"), msg)
        if self.fh:
            self.fh.write(line + "\n")
            self.fh.flush()
        if self.verbose:
            print(line)
            sys.stdout.flush()

    def close(self):
        if self.fh:
            self.fh.close()
            self.fh = None


_DEFAULT = None


def announce(path=None):
    """Convenience: one shared logger per process, announced once."""
    global _DEFAULT
    if _DEFAULT is None:
        _DEFAULT = Logger(path)
        _DEFAULT.announce()
    return _DEFAULT


def log(msg):
    announce().log(msg)


if __name__ == "__main__":
    lg = announce()
    lg.log("self-test of the logger: this line must be on disk before exit")
    print("verifying the line is readable from disk while the process is alive...")
    with open(lg.path, encoding="utf-8") as f:
        tail = f.read().strip().splitlines()[-1]
    print("read back:", tail)
