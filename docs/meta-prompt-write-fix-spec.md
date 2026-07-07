Read the existing specs in `docs/` to learn their style and structure
(section headers, level of detail, how scope and output format are specified,
how non-goals or constraints are expressed).

Read `docs/formal-semantics.md`, specifically Section 3 ("Backtracking
Interaction") and Section 5 ("Probe Interaction with Agenda/ChangeSet
Machinery"). Both sections document the same underlying issue: `ChangeSet`'s
`op_count` (and the `ops[]` array entries it indexes) are mutated directly by
`handle_cons_ops` and are not covered by the `ClientRegion` bump-pointer
rewind that happens on backtrack (`ClientRegion::restore`). Two consequences
follow from this:

1. If a query branch pushes one or more ops via `cons-ops` and then fails
   before completing, and a sibling branch later succeeds without pushing any
   ops itself, the successful branch's ChangeSet can still contain the stale
   ops from the failed branch.
2. Inside a `Probe` evaluated with `sandbox=true`, `cons-ops` calls made by
   the probed sub-goal write into the same outer `ChangeSet` and are not
   rolled back even when the probe's substitution bindings are correctly
   sandboxed. Sandbox isolation currently applies only to substitution
   bindings, not to ChangeSet contents.

Write a new spec file, in the same style as the other files in `docs/`, that
describes a fix for this at the level of "what should be true afterward,"
not a line-by-line patch. The spec should cover:

- The invariant to establish: `ChangeSet` contents visible after a `run_one`
  call should only reflect ops pushed by the branch that actually produced
  the returned answer, with no contribution from any abandoned branch or from
  a sandboxed `Probe` sub-goal.
- That `op_count`/`ops[]` mutations should participate in the same
  save/restore discipline as the `ClientRegion` bump pointer, whether that is
  achieved by moving the count under bump-pointer-managed memory, by tracking
  a saved `op_count` alongside `client_offset` in `State`, or by another
  mechanism the implementer judges cleanest — the spec should describe the
  required behavior, not mandate a specific mechanism.
- That the same mechanism must also cover the sandboxed-`Probe` case: ops
  pushed during a sandboxed sub-goal evaluation must not appear in the outer
  ChangeSet when the probe's bindings are sandboxed away.
- A requirement for regression tests: at minimum, (a) a test with a
  multi-branch query where an earlier branch pushes ops and fails, confirming
  the final ChangeSet contains no trace of those ops; (b) a test with a
  sandboxed `Probe` whose sub-goal calls `cons-ops`, confirming those ops do
  not leak into the outer ChangeSet; (c) confirmation that the full existing
  test suite still passes unchanged.
- A requirement to update `docs/formal-semantics.md` after the fix lands, so
  Sections 3 and 5 accurately describe the corrected behavior.
- An explicit instruction that if the correct fix requires broad structural
  changes (e.g., touching `State`'s layout in many call sites, or changing
  the `ClientRegion` interface in ways that ripple beyond `rap.hpp` and
  `core.hpp`), the implementer should stop and report the scope of the
  needed change rather than proceeding with a partial or risky fix.

Output the spec as a new file in `docs/`, following existing naming
conventions in that directory. Do not modify any implementation code as part
of this task — only produce the spec file.
