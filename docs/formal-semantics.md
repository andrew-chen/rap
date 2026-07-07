# RAP Formal Operational Semantics

This document describes the precise operational semantics of the RAP execution
model as implemented in the current codebase. Every claim cites a source file,
function, and line number so that the document can be verified against source
and kept in sync as the code evolves. Where behavior is ambiguous or
undocumented in comments, this document describes what the code actually does
rather than what may have been intended.

---

## 1. Agenda Selection Semantics

**Data structure.** Queue 2 is a non-wrapping, linear FIFO byte buffer
(`Agenda`, `rap/agenda.hpp:33`). It has a single 64 KiB backing array
(`QUEUE2_ARENA_SIZE = 64 * 1024`, `agenda.hpp:7`), two cursors (`tail` — oldest
unread entry, `head` — next write position), a count, and a monotonically
increasing `next_id` counter. There is no priority field; the only ordering
property is insertion order.

**Normal dequeue.** `Agenda::dequeue` (`agenda.hpp:83`) removes and returns the
entry at `tail` (FIFO). `dequeue_runnable` (`agenda.hpp:107`) does a linear
scan from `tail` forward and selects the **first** entry whose `args.tag ==
TermTag::Nil`. Entries with non-`Nil` args are "state-holder" entries that are
passive data: they persist until explicitly removed via a `Remove` op and are
never selected by `dequeue_runnable`.

**Selection rule.** The `run_one` method in `RapLoop` (`rap/loop.hpp:192`)
calls `agenda.dequeue_runnable(entry)` to pick the next query. The rule is
therefore: select the **oldest runnable entry** (i.e., the one closest to
`tail` with `args == Nil`). This is FIFO among runnable entries; state-holders
are invisible to the scheduler.

**When Queue 2 is empty.** `run_until_empty` (`loop.hpp:186`) loops while
`agenda.has_runnable()` returns true. When no runnable entries exist (either
the agenda is entirely empty or all remaining entries are state-holders),
`has_runnable` returns false (`agenda.hpp:95`) and the loop terminates. There
is no blocking or waiting; the reactive loop simply stops.

**Visibility of newly added entries during execution of step N.** A query's
`run_one` call applies the ChangeSet (`apply_changeset`, `loop.hpp:260`) only
**after** `runN` returns and `eval_arena` is reset (`loop.hpp:245,248`). Any
`Add` ops inside the ChangeSet call `agenda.enqueue(...)` (`loop.hpp:265`),
which appends to `head`. Because `dequeue_runnable` was already called at the
start of `run_one` and extracted the current query, and because the newly added
entries are appended at `head` (after all existing entries), a query added
during the execution of step N **cannot** run before any query that was already
in the agenda prior to step N. New entries are enqueued at the back and will
not be selected until all previously queued runnable entries have been
consumed.

**Reset on empty.** `enqueue` (`agenda.hpp:51`) resets both `head` and `tail`
to 0 when `count == 0` before each enqueue. This prevents head-of-buffer waste
when the agenda empties and refills.

---

## 2. ChangeSet Application Semantics

**Construction.** A ChangeSet is built inside a query's BFS run by calls to
the `cons-ops` virtual relation (`RapEvaluator::handle_cons_ops`,
`rap/rap.hpp:156`). Each call to `cons-ops` pushes exactly one `Op` into
`cs->ops[cs->op_count++]` (`rap/changeset.hpp`, `ChangeSet::push`) and stores
a marker byte in `cs->arena`. The `ops` array is a fixed-size flat array
(`MAX_CHANGESET_OPS`, `changeset.hpp:~line 20`). Ops accumulate in the order
`cons-ops` is called, which is determined by the BFS evaluation order within
Queue 1.

**Application order.** `RapLoop::apply_changeset` (`loop.hpp:260`) iterates
`cs.ops[0..cs.op_count-1]` in strictly sequential, **left-to-right** index
order:

```cpp
for (std::uint32_t i = 0; i < cs.op_count; ++i) { ... }
```

Operations are applied one at a time in the order they were pushed by
`cons-ops`. There is no batching, reordering, or grouping by op type.

**`Remove` for a non-existent QueryId.** `apply_changeset` calls
`agenda.remove(op.query_id)` (`loop.hpp:267`). `Agenda::remove`
(`agenda.hpp:138`) performs a linear scan; if no entry with the given `id` is
found it returns `false`. `apply_changeset` ignores the return value and
continues processing the remaining ops. A `Remove` targeting a QueryId that no
longer exists is silently a no-op.

**Malformed `add` operations.** Malformed ops (e.g., `add` with a `rel_term`
that does not resolve to a `Rel` in the current `rel_env`) are detected at
**enqueue time** inside `apply_changeset` → `agenda.enqueue(op.add.rel_term,
op.add.args)`. `Agenda::enqueue` (`agenda.hpp:46`) immediately rejects any
`rel_term` whose `tag != TermTag::Rel`, returning 0. The `rel_term` was already
stored in the ChangeSet as a `deep_copy_term` of whatever term the query
produced for the `Op::Add::rel_term` field (`rap.hpp:198`). If the query
produced a non-`Rel` term (e.g., an unresolved symbol), `enqueue` silently
rejects it. There is no validation at ChangeSet construction time or at
execution time of the added query.

**Atomicity.** Each `Op` within a ChangeSet is applied independently in the
`for` loop. There is no transaction boundary around the full ChangeSet: if the
second `Add` op in a ChangeSet of three ops fails (e.g., returns 0 from
`enqueue`), the first op has already been applied and the third op will still
be attempted. The granularity is per-op, not per-ChangeSet.

**`Output` deep-copy.** For `Output` ops, the term is `deep_copy_term`'d into
`intern_arena` (which is never reset) so that it remains valid after
`eval_arena.reset()` (`loop.hpp:273-275`). The original copy in `cs->arena`
(inside the `rap_buf` / `rap_arena` region) would survive the `eval_arena`
reset, but the deep copy into `intern_arena` provides a stable long-lived
reference independent of the client region.

---

## 3. Backtracking Interaction

**The ClientRegion bump-pointer mechanism.** `ClientRegion`
(`core/core.hpp:136`) is a struct holding a `base` pointer, a `capacity`, and
a mutable `offset`. The `RapEvaluator` constructor (`rap/rap.hpp:35-40`)
allocates `MAX_CHANGESET_ARENA` bytes from `rap_arena` (the permanent,
never-reset arena) and installs them as the client region. At the start of each
`run_one` call, `evaluator->init_changeset()` (`loop.hpp:204`) is called;
this placement-news a `ChangeSet` at `client_region_.base` and sets
`client_region_.offset = sizeof(ChangeSet)` (`rap.hpp:66`). All subsequent
`cons-ops` and `no-ops` calls append into this region (`cs->arena`, which is an
`Arena` wrapping the space after the `ChangeSet` header).

**What "saved" means.** Each `State` struct carries a `client_offset` field
(`core/core.hpp:201`). When `runN` creates the initial work item
(`core/core.hpp:1718`), `st0.client_offset = client_region_.offset`, which is
`sizeof(ChangeSet)` immediately after `init_changeset()`. This value is the
**floor** that backtracking can never go below: it corresponds to the end of the
`ChangeSet` header, preserving the header's fields even under complete
backtracking.

**Backtracking restore.** At the top of `Evaluator::step`
(`core/core.hpp:1422`), before executing any goal:

```cpp
client_region_.restore(st.client_offset, st.saved_client_count);
```

`ClientRegion::restore` (`core/core.hpp:153`) sets both `offset = saved_offset`
and `client_count = saved_count`. This rewinds the bump pointer to wherever it
was when this branch's choice point was captured and simultaneously restores the
`op_count` tracker to the count at the same point. `ChangeSet::ops[]` entries
above the restored `client_count` are effectively invisible: `apply_changeset`
reads only `cs->ops[0..op_count-1]`, and `op_count` is synchronized from
`client_region_.client_count` by `sync_changeset_op_count()` (`rap/rap.hpp:82`)
before `apply_changeset` is called. Allocations in `cs->arena` (e.g., the
`deep_copy_term` of op arguments and the marker bytes) are also rewound by the
bump-pointer half of the restore.

**Commit on `handleUnknownRelation` success.** After a successful call to
`handleUnknownRelation` (i.e., `cons-ops` or `no-ops` returns `Yield`), the
caller in `step` updates both fields:

```cpp
st.client_offset      = client_region_.offset;       // core/core.hpp:1598
st.saved_client_count = client_region_.client_count; // core/core.hpp:1599
```

This advances both `st.client_offset` and `st.saved_client_count` to include
the new allocation and the incremented op count, so subsequent BFS steps —
including successors of this branch — will not rewind past the new op. Both
values are then propagated forward in the `State` passed to `apply_k_or_yield`.

**`handle_cons_ops` sync at entry.** `RapEvaluator::handle_cons_ops`
(`rap/rap.hpp:178`) begins with:

```cpp
cs->op_count = client_region_.client_count;
```

This is the critical correction: before pushing a new op, the physical
`cs->op_count` is overwritten from the save/restore-managed
`client_region_.client_count`. This prevents a stale `cs->op_count` (left by
a sibling branch that failed after pushing ops) from causing the new branch's
op to be placed at the wrong index. After a successful push, `client_count` is
updated to match:

```cpp
client_region_.client_count = cs->op_count;
```

**No ops from failed branches can survive.** If a query calls `cons-ops` to push
op A, then the branch fails on a subsequent goal, the choice point before op A
is popped from the BFS queue with its saved `saved_client_count = 0`. The next
`step` call restores `client_region_.client_count = 0`, so any later
`cons-ops` call by the surviving branch will sync `cs->op_count` to 0 before
pushing. The `ops[]` entries written by the failed branch remain in memory but
are unreachable because `op_count` (as seen by `apply_changeset`) reflects only
the successful branch's count.

**Can a successful ChangeSet include effects from an abandoned branch?** No —
after this fix, it cannot. `apply_changeset` reads `cs->op_count` only after
`sync_changeset_op_count()` (`rap/rap.hpp:82`) is called by `run_one`, which
sets `cs->op_count = client_region_.client_count`. The value of
`client_region_.client_count` at that point reflects the winning branch's
`saved_client_count` as restored by the final `step` call, not any stale value
from a failed sibling. Regression test A in `rap/test_stage2.cpp` verifies
this: a `disj` where the first branch pushes a stale op then fails and the
second branch pushes the correct op must yield exactly one op.

---

## 4. Fairness / Search Strategy Interaction with the Agenda

**Queue 1 is BFS-per-query.** Queue 1 (`WorkQueue`, `core/core.hpp:707`) is a
linked-list FIFO used only within a single call to `Evaluator::runN`. It is
populated and drained entirely inside `run_one` → `runN`. There is no
connection between Queue 1 of one query and Queue 1 of another. After each
`run_one` call, Queue 1 is implicitly discarded (it lives in `eval_arena`, which
is reset immediately after `apply_changeset`, `loop.hpp:248`).

**No interaction between Queue 1 interleaving and Queue 2 ordering.** Queue 2
ordering is determined only by the order of `enqueue` calls in
`apply_changeset`, which happens after Queue 1 is fully drained (or OOM-stopped)
for the current query. There is no mechanism for Queue 1 BFS steps to preempt
or reorder Queue 2. The two queues are strictly sequential: Queue 1 runs to
completion (one answer, or OOM, or exhaustion), then Queue 2 advances by one
step.

**Can BFS interleaving make ChangeSet construction non-deterministic?** Yes, in
principle. If a query has multiple successful branches (e.g., a `disj` with two
`cons-ops` calls), `runN(1, ...)` will return the **first** answer found by
BFS. BFS processes `g->bin.g1` before `g->bin.g2` (`step` GoalTag::Disj case,
`core/core.hpp:1429-1438`): `w2` (g2) is pushed first, then `w1` (g1), so `w1`
is at the front of the FIFO queue and is processed first. This is deterministic
for a given goal tree. However, if the goal tree's structure changes between
runs (e.g., because a different agenda state is passed as an argument that
influences unification), the branch ordering can differ, producing a different
ChangeSet from logically equivalent initial conditions.

---

## 5. Probe Interaction with Agenda/ChangeSet Machinery

**Sandbox and substitution isolation.** `GoalTag::Probe` is handled in
`Evaluator::step` (`core/core.hpp:1478-1535`). `probe_run`
(`core/core.hpp:1349-1402`) creates a fresh local `WorkQueue q2` and a fresh
`kont_done` continuation, runs the sub-goal with the current `State st` (which
carries the current `client_region_.offset` as `st.client_offset` and the
current op count as `st.saved_client_count`), and returns the outcome and
witness state. Inside `probe_run`, `step` is called with the same
`client_region_` (which is a member of the same `Evaluator` instance), so
`cons-ops` calls inside a Probe sub-goal write into the same `ChangeSet` as
the outer query. The `sandbox` flag controls whether the witness substitution
(`witness.subst`) is propagated back to the outer state.

**Probe op_count isolation.** After `probe_run` returns, the
`GoalTag::Probe` handler in `step` resets `client_region_.client_count` to
`st.saved_client_count` (the pre-probe count), discarding any op-count
increments accumulated inside the probe. For `sandbox=false`, the witness
`State` also has its `saved_client_count` overridden to `st.saved_client_count`
before being passed to `apply_k_or_yield`, so the outer BFS work item carries
the pre-probe count forward. The result is that `cons-ops` calls inside a Probe
sub-goal do not persist into the outer ChangeSet regardless of the sandbox flag.
Sandbox isolation therefore applies to both substitution bindings and ChangeSet
contents. Regression test B in `rap/test_stage2.cpp` verifies this: a Probe
sub-goal that calls `cons-ops` must not produce an output op in the outer
ChangeSet.

**Probe step budget and step counting.** `probe_run` (`core/core.hpp:1387`) uses
its own local loop counter `i` from 0 to `max_iter - 1`. This is completely
independent of the outer `runN` loop counter (`produced` in `runN`,
`core/core.hpp:1725`). Each `probe_run` call starts its step budget fresh
regardless of how many outer BFS steps have been executed. The inner probe's
queue (`q2`) is also separate from the outer `q`; steps in `probe_run` do not
consume from or contribute to the outer queue.

**`insufficient` vs `bounded` classification.** The `insufficient` outcome
(`Outcome::Insufficient`) is set when `max_iter` is not a valid `Int` term
(i.e., the budget argument is unresolved), when the sandbox argument is
unresolved, or when `req_ground=true` and the sub-goal is not ground
(`core/core.hpp:1491-1523`). `bounded` (`Outcome::Bounded`) is set when the
`probe_run` loop exhausts its `max_iter` budget without finding a solution
(`core/core.hpp:1401`). These classifications depend on term values in the
substitution at probe-evaluation time, which is determined by the BFS order of
the outer Queue 1. If the sub-goal's `max_iter` variable could be bound to
different values depending on which branch of an outer `disj` is reached first,
the classification can vary. In practice, for any single `run_one` call that
takes one answer, the probe is evaluated in a specific BFS state and the
classification is deterministic for that state.

---

## 6. Termination / Starvation Behavior

**No anti-starvation or cycle-prevention mechanism exists.** There is no
mechanism in the current implementation that prevents a query from re-adding
itself to the agenda or from participating in a cycle of queries that mutually
re-add each other.

Specifically:
- `Agenda::enqueue` (`agenda.hpp:46`) accepts any `Rel` term unconditionally,
  including one that refers to the query currently being executed.
- `apply_changeset` (`loop.hpp:260`) applies `Add` ops without any check against
  the current set of agenda entries.
- `run_until_empty` (`loop.hpp:186`) loops while `has_runnable()` returns true.
  If a query always adds a runnable entry in its ChangeSet, `has_runnable` will
  always return true and the loop will not terminate.
- There is no visited-set, depth limit, round limit, or any form of fairness
  enforcement across Queue 2 iterations.

A query that unconditionally emits `(add self-rel ())` in its ChangeSet will
cause `run_until_empty` to loop forever (bounded only by eventual arena
exhaustion from `head` advancing until `enqueue` returns 0 due to the
non-wrapping OOM guard at `agenda.hpp:69`).

---

## 7. Memory/Arena Behavior Under Sustained Agenda Churn

**Non-wrapping buffer with compaction.** The `Agenda` buffer does not wrap. The
`head` cursor only advances. Space is recovered only by `remove(id)`
(`agenda.hpp:138`), which compacts all entries **after** the removed one forward
toward `tail` by deep-copying them. After compaction, `head` is updated to the
new write position (`agenda.hpp:181`).

**Cost of removal.** For an agenda with N total entries and the removed entry at
position k (0-indexed from tail), `remove` must deep-copy all N−k−1 subsequent
entries. Each deep copy rewrites `PairNode*` pointers to new addresses in the
earlier buffer region. The cost is proportional to the total byte size of
entries after the removed one, not just the count. Since `dequeue_runnable`
always removes the first runnable entry (which is typically near `tail`), it
triggers compaction of nearly the entire buffer: O(N) entries.

**Head advancement.** `enqueue` always writes at `head` and advances `head`.
Compaction after `remove` reduces `head`. If the add/remove rate is balanced
(one `Add` and one `Remove` per cycle), the buffer remains bounded in size:
after removing the front entry and compacting, `head` regresses approximately
by the byte size of the removed entry. In this balanced case, `head` oscillates
and does not advance monotonically toward `QUEUE2_ARENA_SIZE`.

**Reset on empty.** When `count` reaches 0, both `enqueue` and `dequeue` reset
`head = 0; tail = 0` (`agenda.hpp:51,89`). This prevents any long-lived waste
from a buffer that fully drains. However, if the agenda never fully empties
(e.g., because state-holder entries persist indefinitely), `tail` never resets
and the only way to recover space is via `remove`.

**Asymptotic worst case.** For sustained add/remove churn with K long-lived
state-holder entries near `tail` and one new runnable entry added per cycle:
each call to `dequeue_runnable` → `remove` must compact the K state-holder
entries forward. Cost per iteration: O(K · average_entry_size). The buffer
does not grow if K is bounded and entries are constant-size, but the compaction
work is O(K) per reactive step.

---

## Notes on Existing Documentation

The following findings diverge from or supplement comments and documentation
currently in the repository:

1. **`cons-ops` and client region under sandbox Probe (rap/rap.hpp, Section 5).**
   After the ChangeSet backtrack fix (`docs/CHANGESET_BACKTRACK_FIX.md`),
   `cons-ops` calls inside a sandboxed Probe are fully isolated: both the
   bump-pointer and the `op_count` tracker (`client_region_.client_count`) are
   rewound to their pre-probe values when `probe_run` returns. Sandbox isolation
   now applies to both substitution bindings and ChangeSet contents.

2. **`op_count` now guarded by save/restore (Section 3).** The comment at
   `core/core.hpp` describing `ClientRegion` obligations now covers both the
   bump-pointer offset and the `client_count` field. `ChangeSet::op_count` is
   kept in sync with `client_region_.client_count` via an explicit sync at the
   start of each `handle_cons_ops` call and via `sync_changeset_op_count()`
   before `apply_changeset`. The guarantee no longer relies on usage conventions;
   it is enforced by the save/restore discipline in `State::saved_client_count`
   and `ClientRegion::restore`.

3. **Agenda reset-on-empty behavior (agenda.hpp:51, 89).** The comment "reset on
   empty" appears inline but is not explained in the struct's block comment.
   This behavior is significant for sustained-churn scenarios: without it, a
   buffer that fully empties would leave `tail` at a non-zero position and
   permanently waste all space before `tail`.

4. **EVAL_ARENA_SIZE comment (loop.hpp:15).** The comment after
   `EVAL_ARENA_SIZE = 1024 * 1024 * 1024` reads only `// ` (trailing space,
   no explanation). The size was set to 1 GiB for OOM investigation (see git
   history commit `87ef3b9`). The formal behavior of `runN` returning
   `StepResult::OOM` when this arena exhausts is the intended termination
   signal; the arena size itself is a tuning parameter, not a semantic
   invariant.

5. **`dequeue_runnable` removes before returning (agenda.hpp:107-126).** The
   function name suggests only a query selection, but the implementation calls
   `remove(saved_id)` before returning the entry's data to the caller. This
   means the entry is immediately compacted out of the buffer; there is no
   "in-flight" state for the running query in the agenda. `run_one` works with
   a local `QueryEntry` copy.
