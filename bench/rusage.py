"""Peak memory and CPU time of a benchmarked program, measured right on Linux (see rusage.c).

    argv, report = rusage.wrap(argv)   # run argv instead; on Linux, in a session of its own
    rss_bytes, cpu_s = rusage.read(report, ru)   # after it exits: ru is os.wait4's rusage

On macOS a child's own rusage is right, and argv runs as it is. For a process that's still
running: cpu(pid) and resident_mb(pid).
"""
import os, re, subprocess, sys, tempfile

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


def cpu(pid):
    """A running process's own CPU seconds (user + system), to the clock tick on Linux (procps's
    `ps` shows whole seconds there)."""
    if sys.platform.startswith("linux"):
        with open(f"/proc/{pid}/stat") as f:
            fields = f.read().rsplit(")", 1)[1].split()
        return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")
    t = subprocess.run(["ps", "-o", "time=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
    secs = 0.0
    for x in t.replace("-", ":").split(":"):
        secs = secs * 60 + float(x)
    return secs


def resident_mb(pid, peak=False):
    """A running Linux process's resident set in MiB (VmRSS), or its peak so far (VmHWM)."""
    with open(f"/proc/{pid}/status") as f:
        m = re.search(r"^VmHWM:\s+(\d+) kB" if peak else r"^VmRSS:\s+(\d+) kB", f.read(), re.M)
    return int(m.group(1)) / 1024 if m else None
