#!/usr/bin/env python3
"""Run QEMU's own vhost-user-blk qtest against photon's backend and report EVIDENCE.

Companion to qemu-vhost-user-peer.py: that script patches QEMU's test to accept our
backend, this one runs the six cases and says what each actually proved.

WHY THIS EXISTS INSTEAD OF A PASS/FAIL RUNNER. Two separate things can make a
verdict meaningless here, and both were observed rather than anticipated.

1. `ok` is QEMU's verdict on QEMU's assertion, not a statement about our backend.
   Measured 2026-10-06 and again 2026-10-07: nxvirtq, hotplug and multiqueue all
   report `ok` while QEMU never sends SET_MEM_TABLE, SET_FEATURES or SET_VRING_*.
   Their backend logs are the same shape every time -- the frontend connects and drops
   microseconds later, reconnects and drops tens of milliseconds later, then SIGTERM
   (the exact intervals vary run to run, so this reports the shape and not a number).
   Not one ring goes live and not one byte is served, yet three of six cases are
   green. The peer is not at fault: the same log says it is serving 8 queue(s).

2. `rc=0` does not even mean a test ran. qos-test prints the TAP plan `1..N` and
   exits 0 when N is 0, so a path that matches nothing is indistinguishable from a
   pass by exit status alone. The full paths are machine- and QEMU-version-specific
   and four of the six cases sit in a different group from the other two, so a
   hardcoded prefix silently selects nothing. This driver therefore DISCOVERS the
   paths from `qos-test -l` and counts the plan, reporting NO-TESTS as its own
   verdict rather than letting a zero-test run wear a green rc.

So the verdict below is derived from what our own backend logged plus how many tests
qos-test says it ran -- never from rc alone.

    ./qemu-vhost-user-qtest.py --qemu-build /path/to/qemu/build \
                               --qemu-src /path/to/qemu \
                               --photon-build /path/to/photon/build

Not a CTest case and not registered with add_test: it needs a hand-built QEMU tree
that CI does not have. It exits non-zero when a prerequisite is missing rather than
skipping quietly, because a suite that reports success without running is worse than
no suite -- the "0 SKIPPED reads as ok" trap this repository has already hit twice.

EXIT STATUS: 0 every case matched its recorded baseline; 1 a case drifted, or the
control case failed to produce evidence (which means this instrument is blind, not
that the backend is broken); 2 a prerequisite is missing or no test path resolved.
"""

import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile

CASES = ["basic", "indirect", "idx", "nxvirtq", "hotplug", "multiqueue"]

# The control case. `basic` is the one known to bring the device up and to make
# QEMU's libqos virtio-blk driver write-then-read-compare through our backend, so it
# is the case that can prove this instrument still works. If it comes back without
# evidence, every zero in the table is suspect and the run is reported as blind
# rather than as six results -- a control on a false premise is worse than none.
CONTROL_CASE = "basic"

# Rendered substrings of blk/vhost-user.cpp's own log lines, verified against the
# source rather than recalled: `negotiated features` is the SET_FEATURES handler's
# LOG_INFO; `serving: num` is the LOG_INFO inside vq_start(), printed once per ring
# that actually goes live; `mem table:` is the SET_MEM_TABLE handler's. photon's alog
# prefixes every non-AUDIT line with `<file>:<line>|<func>:` -- common/alog.h feeds the
# Prologue __func__ and common/alog.cpp prints it -- so the enclosing function's own
# name is a usable anchor too (`vq_start:` really does appear in the line above). These
# match message text instead, because that is the part saying what happened. Cited by
# handler and function rather than by line number, because line numbers in this
# repository have rotted twice in a single day.
PATTERNS = {
    "negotiated": "negotiated features",
    "serving": "serving: num",
    "memtable": "mem table:",
}

# Measured 2026-10-06, summed over each case's backend logs. `logs` is how many peer
# logs the case produced -- hotplug and multiqueue start two instances, the rest one.
# A drift in `logs` alone means the harness changed shape, not that the backend did.
BASELINE = {
    #            negotiated  serving  memtable  logs  verdict
    "basic":      (2,         1,       1,       1,   "REAL"),
    "indirect":   (2,         1,       1,       1,   "REAL"),
    "idx":        (2,         1,       1,       1,   "REAL"),
    "nxvirtq":    (0,         0,       0,       1,   "VACUOUS"),
    "hotplug":    (0,         0,       0,       2,   "VACUOUS"),
    "multiqueue": (0,         0,       0,       2,   "VACUOUS"),
}

PEER_LOG_GLOB = "photon-vhu-*.log"
TIMEOUT_SECS = 240
TAP_PLAN = re.compile(r"^1\.\.(\d+)\s*$", re.M)


def die(msg, code=2):
    # Not sys.exit(msg): passing a string makes Python print it and exit 1, which
    # would collapse the "prerequisite missing" status documented above into the same
    # 1 as a drifted case -- and a caller cannot then tell "could not run" from "ran
    # and found something".
    print("%s: error: %s" % (os.path.basename(sys.argv[0]), msg), file=sys.stderr)
    sys.exit(code)


def base_env(args):
    env = dict(os.environ)
    # BOTH of these are load-bearing, measured rather than assumed: with either one
    # alone `qos-test -l` lists 0 vhost-user-blk test paths, with both it lists all 6.
    # An empty listing makes a run print the TAP plan "1..0" and exit 0 -- the silent
    # zero-test green this driver exists to refuse -- so discover_paths() dies loudly
    # rather than letting six NO-TESTS rows through as a result.
    env["QTEST_QEMU_BINARY"] = args.qemu_binary
    env["QTEST_QEMU_STORAGE_DAEMON_BINARY"] = args.storage_daemon
    env["PHOTON_VHU_BACKEND"] = args.backend
    # Deliberately NOT set: PHOTON_VHU_QUEUES. The patched test derives it from each
    # case's own num_queues, so exporting a value here would hide a regression in that
    # forwarding -- which is how a peer that always served one queue once shipped.
    env.pop("PHOTON_VHU_QUEUES", None)
    return env


def check_prereqs(args):
    """Fail loudly on anything missing. Never degrade into a quieter mode."""
    for label, path in (("qos-test", args.qos_test),
                        ("qemu-system-x86_64", args.qemu_binary),
                        ("qemu-storage-daemon", args.storage_daemon),
                        ("vhost-user-cli", args.backend)):
        if not os.path.isfile(path):
            die("%s not found at %s" % (label, path))
        if not os.access(path, os.X_OK):
            die("%s at %s is not executable" % (label, path))
    # The peer patch is what makes QEMU accept PHOTON_VHU_BACKEND at all. Without it
    # the test silently runs against qemu-storage-daemon and every number below would
    # describe QSD, not us -- the most dangerous possible false green.
    src = os.path.join(args.qemu_src, "tests", "qtest", "vhost-user-blk-test.c")
    if not os.path.isfile(src):
        die("QEMU source %s not found (pass --qemu-src)" % src)
    with open(src, encoding="utf-8") as f:
        if "start_photon_vhost_user_blk" not in f.read():
            die("%s is not patched; run qemu-vhost-user-peer.py on it first" % src)


def discover_paths(args):
    """Map case name -> full qos-test path, by LISTING rather than by hardcoding.

    On this tree the paths look like
      /x86_64/pc/i440FX-pcihost/pci-bus-pc/pci-bus/vhost-user-blk-pci/vhost-user-blk/vhost-user-blk-tests/basic
      /x86_64/pc/i440FX-pcihost/pci-bus-pc/pci-bus/vhost-user-blk-pci/vhost-user-blk-pci-tests/idx
    Note the two different leaf groups: basic and indirect live under
    vhost-user-blk-tests, the other four under vhost-user-blk-pci-tests. A single
    hardcoded prefix therefore selects at most two of the six, and selecting none
    still exits 0. The machine prefix is also host- and version-specific, so nothing
    here is safe to write down.
    """
    proc = subprocess.run([args.qos_test, "-l"], cwd=args.qemu_build,
                          env=base_env(args), stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=TIMEOUT_SECS)
    out = proc.stdout.decode("utf-8", "replace")
    paths = {}
    for line in out.splitlines():
        line = line.strip().lstrip("#").strip()
        if not line.startswith("/") or "vhost-user-blk" not in line:
            continue
        leaf = line.rsplit("/", 1)[-1]
        if leaf in CASES:
            paths.setdefault(leaf, line)   # first match; the chosen path is printed
    missing = [c for c in CASES if c not in paths]
    if missing:
        die("qos-test -l listed no vhost-user-blk path for: %s\n"
            "Both QTEST_QEMU_BINARY and QTEST_QEMU_STORAGE_DAEMON_BINARY have to be set "
            "and valid for this test to register its cases at all -- with either one "
            "alone the listing is empty. Check --qemu-binary and --storage-daemon.\n"
            "Listing began:\n%s" % (", ".join(missing), out[:400]))
    return paths


def collect_logs(tmpdir):
    return sorted(glob.glob(os.path.join(tmpdir, PEER_LOG_GLOB)),
                  key=os.path.getmtime, reverse=True)


def count_in(paths, needle):
    """Count lines containing needle, reading bytes not str: a backend log can hold a
    partial multibyte sequence copied out of a data buffer."""
    total = 0
    for p in paths:
        with open(p, "rb") as f:
            for line in f:
                if needle.encode() in line:
                    total += 1
    return total


def tests_run(out):
    """How many tests qos-test says it ran, from its TAP plan. None if no plan line
    appeared at all -- which is itself a failure, not a zero."""
    m = TAP_PLAN.search(out)
    return int(m.group(1)) if m else None


def verdict_for(rc, nrun, negotiated, serving, logs):
    """Derive what the case proved, from evidence rather than from rc alone."""
    if nrun == 0:
        return "NO-TESTS"        # the path matched nothing; rc=0 is meaningless
    if nrun is None:
        return "NO-PLAN"         # qos-test did not even print a TAP plan
    if logs == 0:
        return "NO-LOG"          # instrument blind: the peer never started or never logged
    if negotiated == 0:
        return "VACUOUS"         # device never came up; rc describes QEMU, not us
    if serving == 0:
        return "NEGOTIATED-ONLY"  # features agreed but no ring went live
    return "REAL" if rc == 0 else "FAIL"


def run_case(args, case, path, tmpdir):
    for stale in glob.glob(os.path.join(tmpdir, "photon-vhu-*")):
        os.remove(stale)          # per-case isolation: a leftover log is a false count

    cmd = [args.qos_test, "-p", path]
    print("  $ (cd %s && %s)" % (args.qemu_build, " ".join(cmd)))
    try:
        proc = subprocess.run(cmd, cwd=args.qemu_build, env=base_env(args),
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=TIMEOUT_SECS)
        rc, out = proc.returncode, proc.stdout
    except subprocess.TimeoutExpired as e:
        rc, out = 124, (e.stdout or b"") + b"\n[timed out at %ds]" % TIMEOUT_SECS

    with open(os.path.join(args.out_dir, "qtest-%s.log" % case), "wb") as f:
        f.write(out)

    logs = collect_logs(tmpdir)
    for i, p in enumerate(logs):      # keep the peer logs; they are the only evidence
        with open(p, "rb") as src, \
             open(os.path.join(args.out_dir, "backend-%s-%d.log" % (case, i)), "wb") as df:
            df.write(src.read())

    text = out.decode("utf-8", "replace")
    counts = {k: count_in(logs, v) for k, v in PATTERNS.items()}
    return rc, tests_run(text), len(logs), counts


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--qemu-build", required=True,
                    help="QEMU BUILD dir, the one holding tests/qtest/qos-test")
    ap.add_argument("--qemu-src", required=True,
                    help="QEMU SOURCE dir, to check the peer patch is applied")
    ap.add_argument("--photon-build", required=True,
                    help="photon build dir; the backend is read from its output/")
    ap.add_argument("--backend", help="override the vhost-user-cli path")
    ap.add_argument("--qemu-binary", help="override qemu-system-x86_64 path")
    ap.add_argument("--storage-daemon", help="override qemu-storage-daemon path")
    ap.add_argument("--out-dir", help="where to keep logs (default: a new dir under tmp)")
    ap.add_argument("--cases", nargs="*", default=CASES, choices=CASES)
    args = ap.parse_args()

    args.qos_test = os.path.join(args.qemu_build, "tests", "qtest", "qos-test")
    args.backend = args.backend or os.path.join(args.photon_build, "output", "vhost-user-cli")
    args.qemu_binary = args.qemu_binary or os.path.join(args.qemu_build, "qemu-system-x86_64")
    args.storage_daemon = args.storage_daemon or os.path.join(
        args.qemu_build, "storage-daemon", "qemu-storage-daemon")
    args.out_dir = args.out_dir or tempfile.mkdtemp(prefix="photon-qtest-")
    os.makedirs(args.out_dir, exist_ok=True)

    check_prereqs(args)
    paths = discover_paths(args)
    tmpdir = tempfile.gettempdir()   # the peer's g_get_tmp_dir() resolves the same

    print("backend:  %s" % args.backend)
    print("qos-test: %s" % args.qos_test)
    print("logs:     %s" % args.out_dir)
    print()

    rows = {}
    for case in args.cases:
        print("=== %s ===" % case)
        rc, nrun, nlogs, counts = run_case(args, case, paths[case], tmpdir)
        rows[case] = (rc, nrun, nlogs, counts)
        print("  rc=%d tests=%s logs=%d %s" %
              (rc, nrun, nlogs, " ".join("%s=%d" % (k, counts[k]) for k in PATTERNS)))

    print()
    print("%-11s %4s %6s %5s %10s %8s %9s  %-15s %s" %
          ("CASE", "rc", "tests", "logs", "negotiated", "serving", "memtable",
           "VERDICT", "vs BASELINE"))
    drift = []
    for case in args.cases:
        rc, nrun, nlogs, counts = rows[case]
        v = verdict_for(rc, nrun, counts["negotiated"], counts["serving"], nlogs)
        b = BASELINE.get(case)
        if b is None:
            note = "no baseline"
        else:
            same = (counts["negotiated"] == b[0] and counts["serving"] == b[1]
                    and counts["memtable"] == b[2] and nlogs == b[3] and v == b[4])
            note = "same" if same else "DRIFT was (%d,%d,%d,logs=%d,%s)" % b
            if not same:
                drift.append(case)
        print("%-11s %4d %6s %5d %10d %8d %9d  %-15s %s" %
              (case, rc, nrun, nlogs, counts["negotiated"], counts["serving"],
               counts["memtable"], v, note))

    print()
    # The control is what makes every zero above trustworthy. Without it, a peer that
    # never started, a path that matched nothing, and a backend that never negotiates
    # all produce the same table.
    if CONTROL_CASE in rows:
        rc, nrun, nlogs, counts = rows[CONTROL_CASE]
        cv = verdict_for(rc, nrun, counts["negotiated"], counts["serving"], nlogs)
        if cv != "REAL":
            print("CONTROL FAILED: %s came back %s (tests=%s logs=%d negotiated=%d). This "
                  "instrument is blind, so every zero in the table above is "
                  "uninterpretable -- do NOT read any case as VACUOUS until this is fixed."
                  % (CONTROL_CASE, cv, nrun, nlogs, counts["negotiated"]))
            return 1
        print("CONTROL OK: %s ran %s test(s) and produced evidence (negotiated=%d "
              "serving=%d), so a zero elsewhere means the device really did not come up."
              % (CONTROL_CASE, nrun, counts["negotiated"], counts["serving"]))
    else:
        print("CONTROL NOT RUN: --cases excluded %s, so no zero in the table above is "
              "interpretable." % CONTROL_CASE)
        return 1

    if drift:
        print()
        print("Drifted from the 2026-10-06 baseline: %s" % ", ".join(drift))
        print("Drift is not automatically bad -- a VACUOUS case turning REAL is the fix "
              "this table exists to detect. Read the backend-*.log files before believing "
              "either direction, and update BASELINE only with the run that convinced you.")
        return 1
    print("Every case matched its recorded baseline.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
