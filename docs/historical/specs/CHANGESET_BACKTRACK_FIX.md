# Fix Spec: ChangeSet op_count Participation in Backtrack Save/Restore

**Version:** 1.0  
**Depends on:** STAGE_B_FIX complete (structural Rel-only invariant, `docs/STAGE_B_FIX.md`) (including all STAGE_B related fixes such as STAGE_B_AFTER_FIX_2)
**Modifies:** `core/core.hpp`, `rap/rap.hpp`, `rap/changeset.hpp` (likely);
             `rap/loop.hpp`, `core/test_extension.cpp`,
             `rap/test_stage2.cpp` (tests); `docs/formal-semantics.md`  
**Status:** Specification — not yet implemented  
**Date:** July 2026

---

## Problem Statement

`ChangeSet::op_count` and the entries it indexes in `ops[]` are mutated
directly by `RapEvaluator::handle_cons_ops` (`rap/rap.hpp:217`) but are
not covered by the `ClientRegion` bump-pointer rewind that happens on
backtrack (`ClientRegion::restore`, `core/core.hpp:149`, called at the
top of `Evaluator::step`, `core/core.hpp:1415`).

The bump pointer covers arena allocations made inside `cs->arena` (the
`deep_copy_term` results and marker bytes for each op). It does not cover
`ChangeSet::op_count` (an integer incremented in-place on the header
struct) or the `ops[]` array entries (written directly into the
`ChangeSet` header at fixed offsets below the `client_region_.base +
sizeof(ChangeSet)` boundary).

Two visible consequences follow:

**Consequence 1 — stale ops from failed branches.** If query branch A
calls `cons-ops` (pushing op X into `cs->ops[0]` and incrementing
`op_count` to 1), then branch A fails on a subsequent goal, and branch B
(a sibling branch at the same `disj`) later succeeds without calling
`cons-ops`, the final ChangeSet has `op_count == 1` with op X still in
`ops[0]`. `apply_changeset` will apply op X even though it came from the
branch that failed. This can produce spurious `Add`, `Remove`, or
`Output` effects with no counterpart in the logic of the successful
branch.

**Consequence 2 — ops leaking out of a sandboxed Probe.** `GoalTag::Probe`
evaluated with `sandbox=true` (`core/core.hpp:1506`) correctly discards
the witness substitution — `apply_k_or_yield` is called with the
pre-probe `st` rather than `witness`. However, any `cons-ops` calls made
during `probe_run`'s inner BFS loop (`core/core.hpp:1387–1401`) write
into the same outer `ChangeSet`. Because `probe_run` uses the same
`Evaluator` instance (same `client_region_`), and `st.client_offset`
in the outer call reflects the pre-probe region state, the outer call's
subsequent `client_region_.restore(st.client_offset)` rewinds the arena
bump pointer — but again does not roll back `op_count` or `ops[]`.
Sandbox isolation currently applies only to substitution bindings; it
does not apply to ChangeSet contents.

---

## Invariant to Establish

After every `run_one` call, the ChangeSet accessible via
`evaluator->get_changeset()` (and subsequently consumed by
`apply_changeset`) must satisfy:

> Every op in `ops[0..op_count-1]` was pushed by the single branch that
> produced the answer returned by `runN`. No op from any branch that
> failed before producing an answer, and no op from within a sandboxed
> `Probe` sub-goal, may appear in the final ChangeSet.

More precisely: the `op_count` visible after `runN` returns must equal
the number of `cons-ops` calls that completed on the single surviving
answer branch. If no answer was produced (the query failed entirely),
`op_count` must be 0.

---

## Required Behavior

### 1. op_count and ops[] must participate in the save/restore discipline

The mechanism that enforces this invariant is left to the implementer's
judgment. Three candidate approaches are described here to frame the
design space; any approach that achieves the invariant is acceptable.

**Approach A — track saved op_count alongside client_offset in State.**  
Add a `saved_op_count` field to `State` (`core/core.hpp:196`) alongside
`client_offset`. Whenever `step` saves a `client_offset` into a new
`State`, it also saves the current `op_count`. At the top of `step`,
alongside `client_region_.restore(st.client_offset)`, restore
`cs->op_count` from `st.saved_op_count`. On a successful
`handleUnknownRelation` call, advance `st.saved_op_count` alongside
`st.client_offset` (`core/core.hpp:1584`).

**Approach B — move op_count under bump-pointer-managed memory.**  
Rather than storing `op_count` as a plain integer in the `ChangeSet`
header, allocate it from `cs->arena` (or reserve it as a trailing word
in the header region, above the `sizeof(ChangeSet)` floor, so the bump
pointer rewind covers it). This would require restructuring `ChangeSet`'s
layout such that the count word lives at an offset that the existing
bump-pointer rewind already covers.

**Approach C — reset op_count as part of ClientRegion::restore.**  
Extend `ClientRegion` to track a parallel integer counter that advances
with each `cons-ops` call and is rewound by `restore`. This co-locates
both dimensions of mutable state (arena bump pointer + op_count) in the
single save/restore site.

**Scope check (mandatory before implementing).** Any of these approaches
touches `State`'s layout or the `ClientRegion` interface. Both are used
at many call sites throughout `core/core.hpp`. Before proceeding with
implementation, the implementer must audit all sites that construct or
copy a `State`, all sites that call `client_region_.restore`, and all
sites that save or forward `client_offset`, to determine whether the
chosen approach can be applied safely and locally. **If the correct fix
requires changes that ripple beyond `core/core.hpp`, `rap/rap.hpp`, and
`rap/changeset.hpp` in ways that are broad or risky (for example,
touching the free-function `runN` wrapper in `core/core.hpp:1749`,
security tests in `security/security_test.cpp`, or the raprunner layer
in non-trivial ways), the implementer must stop and report the full scope
of the needed change rather than proceeding with a partial or potentially
incorrect fix.**

### 2. The same mechanism must cover the sandboxed-Probe case

The `probe_run` inner BFS loop (`core/core.hpp:1349–1402`) calls `step`
on the same `Evaluator` instance, which means `cons-ops` calls inside
a probe write into the same `ChangeSet`. Whatever save/restore mechanism
is chosen must restore `op_count` (and `ops[]`) to their pre-probe values
when control returns from `probe_run` to the `GoalTag::Probe` handler,
regardless of whether `sandbox=true` or `sandbox=false`.

Concretely: the `st` captured at the point `GoalTag::Probe` is entered
in the outer `step` call (`core/core.hpp:1478`) encodes the pre-probe
`client_offset` (and, after this fix, the pre-probe `op_count`). When
`probe_run` returns, the fix must restore the ChangeSet to that pre-probe
state before the outer call continues. The current code already calls
`client_region_.restore(st.client_offset)` at the top of each `step`
call; the analogous restore of `op_count` must happen at the same point
and under the same conditions.

Note: this requirement applies identically for `sandbox=false`. Even
when the probe's witness substitution is propagated back to the outer
state, ops constructed inside the probe should not appear in the outer
ChangeSet unless the outer query's own `cons-ops` chain explicitly
includes them. A probe is a meta-level check; its internal relational
side-effects are not part of the outer query's ops.

### 3. The ChangeSet header floor must not be violated

The existing invariant — `client_region_.offset` never goes below
`sizeof(ChangeSet)`, enforced by `init_changeset` setting it to exactly
`sizeof(ChangeSet)` and `runN` capturing this value as `st0.client_offset`
(`core/core.hpp:1718`) — must be preserved. Whatever field stores the
saved op_count must not be placed at an address that would fall below
this floor, and the restore mechanism must not corrupt the `ChangeSet`
header itself.

---

## Regression Tests Required

The following tests must be added. They must live in an existing test
file (`core/test_extension.cpp` or `rap/test_stage2.cpp`) alongside the
existing suite, not in a standalone program that is not run by `make test`.

### Test A — stale ops from failed branch do not appear in ChangeSet

Construct a query of the form:

```
(defrel (test-rel agenda ops)
  (disj
    (conj
      (== #fail-sentinel #not-fail-sentinel)   ; always fails
      (cons-ops (output stale-term) (no-ops) ops))
    (cons-ops (output correct-term) (no-ops) ops)))
```

After `run_one` completes, assert that the ChangeSet contains exactly
one op: `Output(correct-term)`. Assert that `op_count == 1` and that no
op with `output_term == stale-term` is present. Any stale op from the
first (failing) branch is a test failure.

### Test B — sandboxed Probe ops do not appear in outer ChangeSet

Construct a query whose body contains a `GoalTag::Probe` with
`sandbox=true`, where the probed sub-goal calls `cons-ops`:

```
(defrel (inner-goal agenda ops)
  (cons-ops (output probe-term) (no-ops) ops))

(defrel (outer-rel agenda ops)
  (probe inner-goal 'true 10 'true 'false)  ; sandbox=true, want=true
  (cons-ops (output outer-term) (no-ops) ops))
```

After `run_one` completes, assert that the ChangeSet contains exactly
one op: `Output(outer-term)`. Assert that no op with `output_term ==
probe-term` is present. Any probe-internal op in the outer ChangeSet is
a test failure.

### Test C — existing test suite passes unchanged

Run `make test` in full. All previously passing tests must continue to
pass with identical results. No existing test's expected output may
change as a result of this fix.

---

## Update to docs/formal-semantics.md

After the fix lands and all tests pass, update `docs/formal-semantics.md`
as follows:

**Section 3 (Backtracking Interaction):** Remove the description of the
asymmetry between bump-pointer rewind and `op_count` persistence. Replace
with a description of the corrected invariant: the chosen mechanism ensures
that `op_count` and `ops[]` are restored in step with `client_offset`,
so no op from a failed branch can survive into the final ChangeSet. The
paragraph beginning "Ops already accumulated before failure" and the
paragraph beginning "Can a successful ChangeSet include effects from an
abandoned branch?" should both be replaced with accurate post-fix
descriptions. Cite the specific mechanism chosen and where it is
implemented (file and function).

**Section 5 (Probe Interaction with Agenda/ChangeSet Machinery):** Remove
the statement "sandbox isolation currently applies only to substitution
bindings, not to ChangeSet contents." Replace with a description of the
corrected behavior: `probe_run` now restores `op_count` on return, so
`cons-ops` calls inside a Probe sub-goal do not persist into the outer
ChangeSet regardless of the sandbox flag. Cite the mechanism and
implementation location.

---

## What This Is Not

- Not a change to how `no-ops` or `cons-ops` work at the relational
  level — the surface language semantics are unchanged
- Not a change to the `ClientRegion` bump-pointer mechanism as it applies
  to `cs->arena` allocations — those already rewind correctly
- Not a change to Probe's sandbox behavior with respect to substitution
  bindings — the substitution isolation is already correct
- Not a change to the ChangeSet header floor invariant
  (`client_region_.offset >= sizeof(ChangeSet)` always)
- Not a change to the structural Rel-only invariant from `STAGE_B_FIX.md`
  or any other agenda/changeset design decision — this fix is narrowly
  scoped to the save/restore discipline for `op_count`/`ops[]`

---

*v1.0 July 2026 — initial specification*
