#!/usr/bin/env python3
"""time_stage3.py — measure raprunner's wall-clock time running
memory_stage3_0.rap to completion, across a variety of single-
character seed arguments, for the paper's Performance subsection.

Rewritten from an earlier bash+external-timer version specifically to
avoid spawning extra short-lived processes (date/python) per
iteration purely for timekeeping, which would add measurement
overhead on top of raprunner's own execution time. Here, one
long-lived Python process does the timing directly around each
subprocess.run() call to raprunner, using time.perf_counter() (a
monotonic, high-resolution clock), so the only per-iteration overhead
is raprunner's own process spawn -- unavoidable either way, since
raprunner is a separate binary.

This measures END-TO-END TIME TO COMPLETE THE WHOLE GAME (all steps
of a full run), NOT a single run_one() call in isolation like the
existing strengthen-agendao figure in the paper -- the Detailed
Example's natural unit of comparison is "how long does one full
adversarial game take to resolve," not one isolated step, since the
number of steps varies per seed.

Each run is checked for actually having completed successfully (exit
code 0 AND a "final-status" line present in stdout) before its timing
is included in the reported statistics. This guards specifically
against a run that fails fast -- e.g. main producing no solution, or
a crash -- looking artificially fast in the numbers simply because it
terminated abnormally early rather than because raprunner is actually
performing well. Any failed run is reported separately and excluded
from the timing sample, not silently included.

Usage: python3 time_stage3.py /path/to/raprunner /path/to/memory_stage3_0.rap
"""

import subprocess
import statistics
import sys
import time

def run_and_check(raprunner, rapfile, seed):
    """Run raprunner once, returning (elapsed_seconds, ok, detail).
    ok is True only if the process exited cleanly (returncode 0) AND
    its stdout actually contains a final-status line -- guarding
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

    ok = (result.returncode == 0) and ("final-status" in result.stdout)
    detail = None
    if not ok:
        detail = (
            f"returncode={result.returncode} "
            f"stdout_tail={result.stdout[-200:]!r} "
            f"stderr_tail={result.stderr[-200:]!r}"
        )
    return elapsed, ok, detail

def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} /path/to/raprunner /path/to/memory_stage3_0.rap",
              file=sys.stderr)
        sys.exit(1)

    raprunner = sys.argv[1]
    rapfile = sys.argv[2]

    # Deliberately a mix of letters and digits, more variety than the
    # four seed pairings already manually verified for correctness
    # earlier in this project, but cheap to run for a timing sample.
    seeds = list("abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOP")

    warmup = 10
    iterations = 100

    print(f"Warming up ({warmup} runs, discarded)...", file=sys.stderr)
    for i in range(warmup):
        seed = seeds[i % len(seeds)]
        run_and_check(raprunner, rapfile, seed)

    print(f"Measuring ({iterations} runs)...", file=sys.stderr)
    times_us = []
    failures = []
    for i in range(iterations):
        seed = seeds[i % len(seeds)]
        elapsed, ok, detail = run_and_check(raprunner, rapfile, seed)
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
