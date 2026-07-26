#!/usr/bin/env python3
"""time_stage3.py — measure raprunner's wall-clock time running a
given .rap program to completion, across a variety of single-
character command-line arguments (harmless even if the program does
not read them), for the paper's Performance subsection.

Rewritten from an earlier bash+external-timer version specifically to
avoid spawning extra short-lived processes (date/python) per
iteration purely for timekeeping, which would add measurement
overhead on top of raprunner's own execution time. Here, one
long-lived Python process does the timing directly around each
subprocess.run() call to raprunner, using time.perf_counter() (a
monotonic, high-resolution clock), so the only per-iteration overhead
is raprunner's own process spawn -- unavoidable either way, since
raprunner is a separate binary.

This measures END-TO-END TIME TO COMPLETE THE WHOLE PROGRAM, NOT a
single run_one() call in isolation.

Each run is checked for actually having completed successfully (exit
code 0 AND the given SUCCESS_MARKER string present in stdout) before
its timing is included in the reported statistics. This guards
specifically against a run that fails fast -- e.g. main producing no
solution, or a crash -- looking artificially fast in the numbers
simply because it terminated abnormally early rather than because
raprunner is actually performing well. Any failed run is reported
separately and excluded from the timing sample, not silently
included. The success marker is a required argument (not hardcoded)
so this same script works correctly for any .rap file with its own
distinct completion signal -- e.g. "final-status" for
memory_stage3_0.rap, or "explore-ran hypB 5" (its actual last line)
for test_subsume_agenda.rap -- rather than silently miscounting a
genuinely successful run as a failure just because it doesn't happen
to print the string one particular program uses.

Usage: python3 time_stage3.py /path/to/raprunner /path/to/file.rap SUCCESS_MARKER
"""

import subprocess
import statistics
import sys
import time

def run_and_check(raprunner, rapfile, seed, success_marker):
    """Run raprunner once, returning (elapsed_seconds, ok, detail).
    ok is True only if the process exited cleanly (returncode 0) AND
    its stdout actually contains success_marker -- guarding
    specifically against a run that fails fast (e.g. main producing
    no solution, or crashing) looking artificially good in the
    timing numbers just because it terminated abnormally early."""
    start = time.perf_counter()
    result = subprocess.run(
        [raprunner, rapfile, seed],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    end = time.perf_counter()
    elapsed = end - start

    ok = (result.returncode == 0) and (success_marker in result.stdout)
    detail = None
    if not ok:
        detail = (
            f"returncode={result.returncode} "
            f"stdout_tail={result.stdout[-200:]!r} "
            f"stderr_tail={result.stderr[-200:]!r}"
        )
    return elapsed, ok, detail

def main():
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} /path/to/raprunner /path/to/file.rap SUCCESS_MARKER",
              file=sys.stderr)
        sys.exit(1)

    raprunner = sys.argv[1]
    rapfile = sys.argv[2]
    success_marker = sys.argv[3]

    # Deliberately a mix of letters and digits, more variety than the
    # four seed pairings already manually verified for correctness
    # earlier in this project, but cheap to run for a timing sample.
    seeds = list("abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOP")

    warmup = 10
    iterations = 100

    print(f"Warming up ({warmup} runs, discarded)...", file=sys.stderr)
    for i in range(warmup):
        seed = seeds[i % len(seeds)]
        run_and_check(raprunner, rapfile, seed, success_marker)

    print(f"Measuring ({iterations} runs)...", file=sys.stderr)
    times_us = []
    failures = []
    for i in range(iterations):
        seed = seeds[i % len(seeds)]
        elapsed, ok, detail = run_and_check(raprunner, rapfile, seed, success_marker)
        elapsed_us = elapsed * 1_000_000
        if ok:
            times_us.append(elapsed_us)
        else:
            failures.append((seed, detail))
            print(f"  WARNING: seed {seed!r} did not complete normally: {detail}",
                  file=sys.stderr)

    print()
    print(f"Successful runs: {len(times_us)} / {iterations}")
    if failures:
        print(f"FAILED runs: {len(failures)} -- see warnings above; these are "
              f"EXCLUDED from the timing statistics below, since a fast failure "
              f"would otherwise silently make the reported numbers look better "
              f"than they should.")
    print()

    if not times_us:
        print("No successful runs -- cannot compute timing statistics.",
              file=sys.stderr)
        sys.exit(1)

    times_us.sort()
    n = len(times_us)
    median = statistics.median(times_us)
    lo, hi = times_us[0], times_us[-1]

    print(f"n = {n} successful runs")
    print(f"median = {median:.1f} us")
    print(f"range  = {lo:.1f}-{hi:.1f} us")
    print()
    print("Full sorted sample (us), successful runs only:")
    print([f"{t:.1f}" for t in times_us])

if __name__ == "__main__":
    main()
