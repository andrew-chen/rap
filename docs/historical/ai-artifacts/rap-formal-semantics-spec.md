# Spec: Formal Semantics Documentation

Produce a single markdown file at `docs/formal-semantics.md` documenting the
precise operational semantics of the RAP execution model, grounded in the
current implementation. Ground every claim with a file/function reference
(and line numbers where practical) so it can be verified against source.
Where behavior is ambiguous or undocumented in comments, describe what the
code actually does rather than what might be intended. Make no changes to existing code, however, you may generate new examples in the examples directory - any examples that you do generate, please put in that directory and keep there. Before doing any of this, ensure that the worktree and main are in sync. After doing this, ensure that the worktree and main are in sync, and commit the result.

## Sections

### 1. Agenda Selection Semantics

- The exact rule used to select the next query from Queue 2 each iteration
  of the main loop (FIFO, priority, or other).
- Behavior when Queue 2 is empty.
- Whether a query added to the agenda during execution of step N can run
  before a query that was already queued prior to step N.

### 2. ChangeSet Application Semantics

- The exact order of operations when a ChangeSet is applied, including
  whether it is strictly left-to-right through the `cons-ops` chain.
- Behavior when a ChangeSet contains `remove(QueryId)` for a QueryId that no
  longer exists in the agenda.
- When malformed `add` operations (e.g. referencing undefined relations) are
  detected: at ChangeSet construction, at application, or later at execution
  of the added query.
- The granularity at which `add`, `remove`, and `output` operations within a
  single ChangeSet are applied atomically. Cite the application code path.

### 3. Backtracking Interaction

- What happens to a partially constructed ChangeSet (via `cons-ops`) when the
  query that was building it fails partway through and backtracks. Describe
  the ClientRegion bump-pointer save/restore mechanism precisely, including
  exactly what "restored" means for any ops already accumulated before the
  failure point.
- Whether a successful ChangeSet can ever include effects from a branch that
  was later abandoned via backtracking.

### 4. Fairness / Search Strategy Interaction with the Agenda

- Whether BFS interleaving applies only to Queue 1 (intra-query search) with
  no interaction with Queue 2 ordering, or whether the two interact.
- Whether Queue 1's interleaving can make ChangeSet construction
  non-deterministic across repeated runs of the same query against the same
  agenda state.

### 5. Probe Interaction with Agenda/ChangeSet Machinery

- Whether a goal evaluated inside `Probe` with `sandbox=true` can construct or
  contribute to a ChangeSet that escapes the sandbox, or whether sandboxing
  affects only substitution bindings.
- The precise relationship between `Probe`'s step budget and step-counting
  elsewhere in the VM queue (same counter, nested counter, or independent).
- Whether `insufficient` vs `bounded` classification can depend on evaluation
  order within Queue 1 for logically equivalent goals.

### 6. Termination / Starvation Behavior

- Whether any mechanism prevents a query from perpetually re-adding itself or
  a cycle of queries that re-add each other, such that the agenda never
  empties. If no such mechanism exists, state this plainly.

### 7. Memory/Arena Behavior Under Sustained Agenda Churn

- The asymptotic behavior of the current fixed-size, non-wrapping Queue 2
  arena under sustained add/remove operations (e.g. cost per removal in
  terms of current agenda size), based on the existing compaction mechanism.

## Output Format

- Single file: `docs/formal-semantics.md`.
- Use the section numbers and headers above.
- Cite specific source files and function/method names for every claim.
- Include a final section, "Notes on Existing Documentation," listing any
  places where this document's findings differ from comments or docs
  currently in the repository.
