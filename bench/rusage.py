"""Peak memory and CPU time of a benchmarked program, measured right on Linux (see rusage.c).

    argv, report = rusage.wrap(argv)   # run argv instead; on Linux, in a session of its own
    rss_bytes, cpu_s = rusage.read(report, ru)   # after it exits: ru is os.wait4's rusage

On macOS a child's own rusage is right, and argv runs as it is.
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "rusage.c")
BIN = os.path.join(HERE, ".build", "rusage")


def launcher():
    """The launcher, built when missing or older than its source; None where it isn't needed."""
    if not sys.platform.startswith("linux"):
        return None
    if not os.path.exists(BIN) or os.path.getmtime(BIN) < os.path.getmtime(SRC):
        os.makedirs(os.path.dirname(BIN), exist_ok=True)
        subprocess.run([os.environ.get("CC", "cc"), "-O2", "-o", BIN, SRC], check=True)
    return BIN


def wrap(argv):
    """(argv to run, report file or None). Start it with start_new_session=True and stop it with
    os.killpg: the launcher's child is in its process group."""
    exe = launcher()
    if exe is None:
        return argv, None
    fd, report = tempfile.mkstemp(prefix="rusage-")
    os.close(fd)
    return [exe, report, *argv], report


def read(report, ru):
    """(peak RSS in bytes, user + system CPU seconds) of the program: from the launcher's report,
    else from its own rusage (macOS reports ru_maxrss in bytes). (None, None) when the launcher
    was stopped before it could report."""
    if report is None:
        return ru.ru_maxrss, ru.ru_utime + ru.ru_stime
    try:
        with open(report) as f:
            kb, user, sys_ = f.read().split()
        return int(kb) * 1024, float(user) + float(sys_)
    except (OSError, ValueError):
        return None, None
    finally:
        if os.path.exists(report):
            os.unlink(report)
