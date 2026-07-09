# Instrumentation Spec: Separate Parse vs. Evaluation Timing

**Version:** 1.0  
**Depends on:** Current `main` (CHANGESET_BACKTRACK_FIX complete)  
**Modifies:** `security/security_test.cpp` (required);
             `parse_run.cpp` (nice-to-have)  
**Status:** Implemented  
**Date:** July 2026

---

## Motivation

The security case studies (`security/security_test.cpp`) currently report a
single elapsed-time figure per policy check — the wall time for one call to
`PolicyChecker::check()`, which bundles `parse_query()` and `runN()` into an
undifferentiated total. This makes it impossible to answer a question that
matters for the paper: does string parsing dominate the per-check cost, or does
relational evaluation dominate?

The question is live because the security test constructs query strings
dynamically at runtime (`build_acl_query`, `build_network_query`), invoking
the full s-expression parser and compiler on every call. If parsing accounts for
the majority of the measured latency, that changes what can honestly be said
about the evaluator's speed — and suggests that pre-compiling queries (caching
the `ParsedQuery` across calls) would be a meaningful production optimization
worth describing. If evaluation dominates, that is the more reassuring answer
for the paper and can be stated without qualification.

A single-sample measurement is also too noisy to be meaningful at microsecond
granularity. The current output (one `(%.1f µs)` line per event) reflects JIT
effects, branch-predictor cold starts, and other one-time costs that disappear
in steady-state use.

---

## Scope

**Required:** `security/security_test.cpp`. This is the file directly relevant
to the paper's case studies.

**Nice-to-have:** The programs in `parse_run.cpp` (programs 1–13, including
`appendo` and the disequality benchmark). Adding the same instrumentation there
would give a comparison baseline — parsing overhead as a fraction of total time
across queries of varying complexity and recursion depth. This is not essential
for the paper but would strengthen any claim about parsing cost being negligible
(or non-negligible) for the evaluation benchmarks too.

Do not add instrumentation to `test_stage2.cpp`, `test_arith.cpp`,
`core_test_extension.cpp`, or `rap_test_extension.cpp`. Those are correctness
tests; their timing is irrelevant and adding benchmark harnesses would clutter
them.

---

## What to Measure

For each policy check (each call to what is currently `PolicyChecker::check()`),
measure three quantities independently:

1. **Parse time** — the wall time for `parse_query(arena, query_str.c_str())`
   alone, excluding the subsequent `runN` call and any arena reset.

2. **Evaluation time** — the wall time for the `runN(...)` call alone, after
   the query has been successfully parsed.

3. **Total time** — the sum of (1) and (2). This should match (within noise)
   the current single-number measurement, and reporting it allows readers to
   verify the split adds up.

The arena reset (`arena.reset()`) is not part of either parse or evaluation
time. It is a bookkeeping operation; do not include it in any measured window.

---

## Methodology

Run each query 100 iterations with a warmup phase, using the same approach
throughout:

- **Warmup:** 5 iterations, discarded. This lets the instruction cache and
  branch predictor reach steady state before measurement begins.
- **Measured:** 100 iterations. Accumulate total parse time and total evaluation
  time across all 100 iterations, then divide by 100 to report per-iteration
  averages.
- **Arena management within the loop:** call `arena.reset()` after each
  iteration (both in warmup and in measurement) so that each iteration starts
  from a clean arena, exactly as the current single-shot test does.

Report the **average** parse time and the **average** evaluation time (both in
µs), formatted to one decimal place, for each query. Also report the average
total so readers can verify the split. A suggested output format per query:

```
  (parse: 3.2 µs, eval: 1.1 µs, total: 4.3 µs)
```

followed by the existing `PERMITTED` / `VIOLATION` line (which must still be
printed, using the result from any single iteration — all 100 must agree).

If parse time and evaluation time are within noise of each other (e.g., both
under 2 µs and within ±0.5 µs), note this explicitly in any prose that
accompanies the benchmark results — the numbers are not meaningful beyond what
the timer resolution supports.

---

## Required Output Behavior

The test must still print `PERMITTED` or `VIOLATION` (or `ERROR`) for every
event, exactly as it does now. The benchmark loop must not suppress or
restructure this output; add the timing line alongside it.

All 100 iterations of each query must produce the same `Result`
(`Permitted` / `Violation` / `Error`). If any iteration produces a different
result from the first, the test should print a diagnostic and abort. This
guards against arena aliasing or other bugs introduced by the loop.

The overall structure of the printed output (the two case study headers, the
per-event blocks) must remain recognizable. A Reviewer who read the earlier
single-shot output should be able to read the new output without confusion.

---

## Implementation Notes

The function-level split is straightforward. Inside `PolicyChecker::check()`,
the parse and evaluation steps are already sequential:

```cpp
ParsedQuery pq = parse_query(arena, query_str.c_str());   // step 1
// ...
runN(arena, pq.n, pq.goal, pq.qvar, pq.vars_used,         // step 2
     pq.outcome_syms, [&](...){ ... });
```

Wrap each step in `std::chrono::high_resolution_clock::now()` calls.
The cleanest approach is probably to refactor `check()` to accept accumulator
references for parse and evaluation nanoseconds, or to return a struct with
both durations, so the calling loop can accumulate across iterations. Whether
to restructure `check()` or to open-code the loop at the call site is left to
the implementer's judgment; either is acceptable as long as the measurement
boundaries are parse-only and eval-only as defined above.

Use `std::chrono::duration_cast<std::chrono::nanoseconds>` for accumulation
and divide at the end, converting to µs for display. Accumulating in
nanoseconds prevents rounding loss when averaging 100 short-duration samples.

---

## Nice-to-Have: parse_run.cpp Instrumentation

If adding the same instrumentation to `parse_run.cpp`, the methodology is the
same (5 warmup + 100 measured iterations, per-program). The interesting
comparison there is parse time vs. evaluation time for programs of varying
complexity: program 1 (trivial two-branch `disj`) vs. program 8 (`appendo`
with recursion) vs. program 10 (mutual recursion via `fresh`) vs. program 13
(disequality constraint). If parsing cost is roughly constant across programs
while evaluation time scales with query complexity, that would confirm that the
parser overhead is fixed and predictable.

`parse_run.cpp` currently prints diagnostic information (`print_query`,
`print_term` for each answer). In benchmarking mode, these prints should be
suppressed for the 100 measured iterations and restored only for a single
representative run (e.g., run the program once with printing enabled, then run
it 100 times without). Alternatively, suppress all printing during the
benchmark and state this in a comment. Do not mix printing and timing in the
hot loop — `printf` cost would dominate.

Before entering the warmup or measurement loop for any program, verify that
`parse_query` returns a valid `ParsedQuery` (i.e., `pq.goal != nullptr`). If
the initial parse fails, skip that program's benchmark entirely and print a
diagnostic — do not start the loop against a null goal. The security test
handles this implicitly through the existing `Result::Error` path in `check()`,
but `parse_run.cpp` has a different structure (the parse is exposed directly at
the call site) and requires an explicit pre-loop check. A silently-looping
null-goal run would produce meaningless timing numbers with no indication of
the failure.

Whether to add `parse_run.cpp` instrumentation at the same time as
`security_test.cpp`, or defer it to a follow-on, is left to the implementer.

---

## What This Is Not

- Not a change to the query-string construction (`build_acl_query`,
  `build_network_query`) — the queries are generated the same way as before,
  with no pre-compilation or caching
- Not a change to the policy logic, fact base, or expected results
- Not a performance optimization — this spec does not ask for any speed
  improvement, only for visibility into where the time currently goes
- Not a permanent restructuring of `PolicyChecker` — if the cleanest
  measurement approach requires a temporary refactor of `check()`, that is
  acceptable, but no new abstractions are required beyond what the measurement
  task needs
- Not a change to the `make test` output format: `security_test` is not run
  by `make test` (it prints prose, not PASS/FAIL lines), so there is no test
  suite output to maintain

---

*v1.0 July 2026 — initial specification*
