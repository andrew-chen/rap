# RAP Programming Guide and Best Practices

*A living document capturing how to write correct `.rap` programs and
the hard-learned lessons from Stages 2.0 through 2.5. Read this before
writing any new `.rap` code in this project. Update it whenever a new
bug class is discovered.*

## Part 1: Language Basics

### Core primitives

- `==` — unification
- `fresh` — introduce logic variables, e.g. `(fresh (x y) body)`
- `disj` — disjunction (OR); explores all branches. **Also requires at
  least 2 arguments**, same as `conj` below — both are enforced by the
  same parser check. `(disj (== q foo))` fails with `[compile_goal]
  ERROR: 'disj' requires at least 2 args`.
- `conj` — conjunction (AND); **requires at least 2 arguments** — a
  single-argument `conj` is a compile error (`'conj' requires at least
  2 args`). If you only have one goal, don't wrap it in `conj` at all.
- `defrel` — define a named relation: `(defrel (name params...) body)`
- `call` — invoke a **first-class relation value** (a `rel` term held
  in a variable), e.g. `(call some-rel-var arg1 arg2)`. Do NOT use
  `call` to invoke ordinary named relations or built-ins like
  `cons-ops`/`no-ops` directly in source — that's unnecessary
  verbosity (though not incorrect; `wc.rap` does this and still
  works). Write `(cons-ops ...)` not `(call cons-ops ...)`.
- `rel` — construct an anonymous relation value:
  `(rel (params...) body)`. Required whenever you need to pass a
  relation as data (e.g., inside an agenda `add` operation).
- `probe` — bounded meta-evaluation:
  `(probe Goal Condition Budget Sandbox ReqGround)`. See Part 3 for
  detailed usage and constraints.

### Data representation

- Lists are built from dotted pairs: `(a . (b . (c . ())))`, written
  as `(a b c)`.
- Association-list-style data: `((key1 . val1) (key2 . val2) ...)`.
- Tagged terms for anything that might coexist with structurally
  similar data: `(tag-symbol field1 field2 ...)` — see the "tagged
  shapes" lesson in Part 2, item 6.

### Native arithmetic (not Peano-encoded)

RAP has real `int32`-backed arithmetic as built-in relations, not
pure-relational Peano numerals:

- `addo a b c` — `a + b = c`
- `subo a b c` — `a - b = c`
- `mulo a b c` — `a * b = c`
- `leqo`, `lto`, `gto`, `eqo`, `geqo`, `neqo` — comparisons (≤, <, >,
  =, ≥, ≠ respectively). All accept constraint-mode arguments (one
  bound, one unbound).
- `divo a b q r` — `a / b = q remainder r`, floor-division semantics
  (result satisfies `0 <= r < b` even for negative dividends). **`b`
  must be strictly positive** — `divo 10 0 q r` and `divo 10 -1 q r`
  both fail with no solutions. This is implied by `0 <= r < b` but
  easy to miss, especially if you read "even for negative inputs" as
  covering negative `b` too — it doesn't.
- `modo a b r` — `a mod b = r`, floor-division semantics, same `b > 0`
  requirement as `divo`.
- **Overflow**: these now correctly FAIL (not silently wrap) if a
  true result doesn't fit in `int32`. This was not always true (see
  Part 4, "Silent integer overflow").
- `divo`/`modo` correctly support computing quotient+remainder from
  known dividend+divisor (this also wasn't always true — see Part 4).

### The agenda model

- Every agenda entry is a runnable relation, **always**. There is no
  "state-holder" / non-runnable entry concept — this was found to
  exist informally in an earlier engine implementation and was
  deliberately removed (see Part 4). If you're tempted to add
  something to the agenda that "just holds data," it must still be a
  real, callable relation — even if its body is currently a no-op.
- Agenda entry relations take **2 or 3 parameters**: `(agenda ops)` or
  `(agenda args ops)`. `ops` is always last. If you need more than one
  piece of data, make `args` a single compound term (a list, an
  association list, a tagged tuple) — the calling convention is
  deliberately capped at 3 params; richer data goes inside the
  compound `args` term, not more parameters.
- Selection is strict FIFO: the oldest entry in the agenda always
  runs next.
- `add`/`remove`/`output` are ChangeSet operation *constructors*, not
  calls to anything — don't say "call add" or "call remove"; they are
  data describing what should happen when the ChangeSet is applied.
- `cons-ops`/`no-ops` build up a ChangeSet: `(cons-ops Op Rest Ops)`
  prepends `Op` to `Rest`, producing `Ops`. Chain multiple calls by
  threading intermediate variables: `ops0`, `ops1`, `ops2`, etc.,
  each one's output feeding the next call's `Rest` input. This works
  correctly chained arbitrarily deep (confirmed at least 4 deep).
- To add a new agenda entry: `(add (rel (params...) (call
  some-relation params...)) args-value)`. The `rel`'s own parameter
  names (commonly `a`, `b`, `r` or similar) don't need to match the
  target relation's parameter names — they're just local names for
  whatever `run_one` passes in (agenda, then args if 3-param, then a
  fresh `ops` variable).
- To remove an entry: `(remove id)`, where `id` came from a prior
  `find-by-contento` (or similar) lookup.
- Looking up an entry by its content: `find-by-contento agenda pattern
  id` — unifies `pattern` against the found entry's `args` field, and
  binds `id` to that entry's id. Passing an unbound variable as
  `pattern` matches the first entry found, unconditionally — see Part
  2, item 6 for why this is dangerous once multiple structurally
  similar entry kinds coexist.

## Part 2: Bug Patterns Actually Encountered (in the order discovered)

These are not hypothetical risks — every one of these caused a real,
confirmed bug in this project. Check for all of them before presenting
any new `.rap` code.

### 1. Tag/variable name collision (found and fixed 3+ times)

A bare symbol that matches a name currently in scope via `fresh` (or a
`defrel`'s own parameters) is **always compiled as a reference to that
variable** — even in output/tag position, even when you meant it as a
literal symbol. There is no separate "quoted symbol" vs. "variable
reference" distinction for bare symbols; a symbol's binding is
determined purely by whether it's in scope.

**Symptom**: instead of `(my-tag some-value)`, you get
`(some-value some-value)` — the tag position silently became a second
reference to the same variable as the data position.

**Fix**: before presenting any `defrel` that produces `output`,
explicitly cross-check every tag symbol used in an `output` call
against every name in that relation's `fresh` list (and its own
parameter list). This must be done for every single relation with an
`output` call, every time — it is not a "check once and remember" — it
recurred after being fixed once already in this same project, because
a *different* file's *different* relation had the same class of
mistake.

### 2. `disj` branches that aren't actually mutually exclusive (found and
   fixed 3+ times)

Writing `(disj (conj Condition Consequence) Fallback)` where
`Fallback`'s own success does NOT actually depend on `Condition`
failing is a bug. Both branches remain available regardless of the
real outcome, and which one a single-answer query (`run 1` or
equivalent) returns depends on search/branch order, not on
correctness. This is easy to write by accident because it *reads* as
"if/else," but relationally it isn't one unless you make it one.

**How to actually make branches exclusive**: convert the condition's
success/failure into an explicit, ground outcome symbol first (using
`Probe`, see item 3 below), then `disj`/branch on that explicit symbol.
Do not try to use `=/=` against a variable that hasn't been
meaningfully constrained by the condition's own success/failure — that
doesn't create real exclusivity, it just adds an independent
constraint that happens to often coincidentally work.

**A subtler version of this bug**: when the "condition" involves a
search that also needs to bind variables you want to keep (e.g.,
finding a partner position and needing that position's value later),
wrapping the whole search in `Probe` once (to learn true/false) and
then, in the true-branch only, doing the same search again for real
to get the bindings, avoids the mutual-exclusivity trap while still
getting real bindings out. See `has-new-pairo` /
`find-new-matches-collecto` in Stage 2.5 for a worked example — and
note the search subrelation was factored into its own named relation
specifically so the compound condition could be `Probe`d as a single
unit without duplicating its text at two call sites (duplication is
its own risk — see item 5 below).

### 3. `Probe`'s goal argument must be literal syntax at the call site,
   not a variable

`(probe some-goal-var condition budget sandbox reqground)` — where
`some-goal-var` is a parameter/variable holding a goal, rather than a
goal written directly — fails to compile (`compile_goal` requires a
syntactic list expression, not a bare symbol). `Probe` is currently an
**inline construct, not a higher-order one**: you cannot write a
general-purpose `noto`/negation-as-failure helper that takes an
arbitrary goal as a parameter and probes it internally. Every `probe`
call must have its goal written out literally at that exact call site.

This does NOT mean you can't reuse probed logic — factor the goal
itself into a named relation (e.g., `has-new-pairo`) and probe *that
named relation's call*, which is a single literal syntactic form:
`(probe (has-new-pairo a b c) condition budget sandbox reqground)`.
This works fine; only a bare variable standing in for the whole goal
form fails.

A proposed `noto`/`noto-strict` stdlib addition based on the false
assumption that `probe` could take a parameterized goal was drafted,
found infeasible via this exact constraint, and abandoned (see Part 4).
The right long-term fix (extend `probe` to accept a first-class `rel`
term as its goal, so `(noto (rel () goal-form))` would work) has been
identified but not implemented as of this writing.

### 4. Parenthesis balance — always verify programmatically, never by eye

Hand-counting nested parens, especially past 5-6 levels deep, is
unreliable — this project hit real, presentation-blocking paren-count
errors multiple times, including once from manually retyping a
previously-verified block (introducing a new error a straight copy
would not have) and once from removing a redundant wrapper and
miscounting the resulting depth by exactly one.

**Standing rule**: before presenting any `.rap` file with any
non-trivial nesting, run a small script that tracks paren depth line
by line and confirms every top-level form returns to depth 0
individually (not just the whole file summing to 0 — offsetting errors
in different forms can cancel out and hide a real bug). A minimal
version:

```python
depth = 0
form_start_line = None
for i, line in enumerate(lines, 1):
    code = line.split(';')[0]  # strip comments
    for c in code:
        if c == '(':
            if depth == 0: form_start_line = i
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                print(f'Form at {form_start_line} closes at {i}')
            if depth < 0:
                print(f'ERROR: negative depth at line {i}')
```

**Corollary**: never retype a block of `.rap` code that has already
been verified correct elsewhere. Extract and paste the exact text
programmatically (or via direct copy-paste), even across files. This
project's future module system (see Part 5) should make this a
non-issue by allowing real imports instead of manual duplication.

### 5. Avoid duplicating non-trivial logic across call sites, even
   within one file

Related to item 4's corollary: any time the "fix" for a `disj`
exclusivity problem (item 2) would require writing the same non-trivial
goal twice (once inside a `Probe`, once for real), factor it into a
named relation instead of inlining it twice. Two inline copies of the
same logic are two chances for them to silently drift apart or for one
copy to have a typo the other doesn't.

### 6. Untagged, structurally-ambiguous agenda entry shapes are a
   silent, slow-motion time bomb

`find-by-contento agenda pattern id` matches whatever entry's `args`
field first unifies with `pattern`. If `pattern` is loosely shaped
(e.g., a bare unbound variable, or a generic `(x . y)` dotted pair)
and **more than one kind of agenda entry could structurally match
it**, `find-by-contento` will silently return the wrong entry whenever
that other kind of entry happens to be dequeued first or appear
earlier in the agenda.

**This bug is dangerous specifically because it does not fail
immediately or obviously.** In Stage 2.5, an untagged `(matched .
counter)` pattern accidentally matched attempt entries' `(pos1 . pos2)`
args (both are bare 2-element dotted pairs), causing wrong entries to
be silently removed. The program still ran, still produced several
lines of *correct-looking* output, and only crashed (with a
segmentation fault, several iterations later) once the accumulated
corruption became severe enough — making this substantially harder to
diagnose than an immediate failure would have been.

**Standing rule**: any agenda entry meant to be found via
`find-by-contento` must have an unambiguous, explicitly tagged shape —
`(entry-kind-symbol field1 field2 ...)` — not a bare structural
pattern that could coincidentally describe a different kind of entry.
This includes entries that seem safe today because nothing else
currently shares their shape — that safety is not guaranteed to
survive the next stage that introduces a new entry kind. When adding
any new kind of agenda entry, explicitly check whether its `args`
shape could be confused with any existing entry kind's shape, not just
whether it's convenient to construct.

*(Known outstanding risk as of Stage 2.5: belief's own shape, a bare
association list `((pos . status) ...)`, is NOT tagged and has not yet
caused a problem only because no other alist-shaped entry currently
coexists with it in the agenda. This should be fixed — tag it, e.g.
`(belief . alist)` — before any future stage introduces another
alist-shaped agenda entry.)*

### 7. Static re-reading has limits — know when to switch to actual
   execution/tracing

Several bugs in this project (an argument-order assumption, a
De Bruijn index swap, a platform-specific `poll()` behavior difference)
were not found by re-reading code, no matter how carefully, and were
only found by actually running the code with targeted debug output or
a debugger. If tracing through logic by hand across two or more
careful attempts hasn't converged on an answer, that's a signal to
switch to empirical tracing (print statements, a debugger, a
stack trace) rather than a third round of the same static approach.

### 8. Isolated tests with small inputs can pass cleanly while a real
   bug (or an insufficient constant) only manifests at realistic scale

Two distinct issues in Stage 2.6 (a `Probe` budget too small once
belief was fully populated across a real 16-position board, and a
genuine engine-level memory-corruption bug in `deep_copy_term` during
buffer compaction — see Part 4) both had one thing in common: **every
relevant piece of logic had already passed its own isolated test with
small, hand-built inputs**, and both bugs only appeared once the same
logic ran against realistically-sized data inside the full integrated
program.

This does not mean isolated testing (Part 2's own general discipline,
used successfully throughout Stages 2.0-2.5) was wrong — it caught
many real bugs early and cheaply. It means isolated testing at small
scale is necessary but not sufficient. Two concrete takeaways:

- **`Probe` budgets and similar fixed constants** chosen against small
  test inputs may need to be re-checked (or set generously, or made
  configurable) once the same logic is exercised against realistic
  data sizes. A budget of `100` was plenty for a 5-6-position isolated
  test and silently insufficient for a real 16-position board.
- **Some bugs (particularly memory-safety issues in the engine itself)
  may only manifest under specific timing/size conditions** — e.g., a
  compaction bug that depends on the exact byte-size relationship
  between a removed entry and the entry being compacted into its gap.
  No amount of small-scale isolated testing would have caught this;
  it required running the full, realistically-sized integration and
  then tracing the actual failure with byte-level precision once it
  appeared.

**Practical implication**: after isolated tests pass, still budget
time for at least one full end-to-end run at realistic scale before
considering a stage done — and if that full run fails even though
every isolated piece passed, do not assume the isolated tests must
therefore be wrong; the bug may be a scale-dependent one that isolated
testing structurally cannot catch.

### 9. Hardcoded constants need their documented domain constraints
   checked explicitly — structural tracing alone won't catch this

A hardcoded seed value (`424242`) was passed to `lcg-nth-o`/
`lcg-nexto` without checking it against `lcg-nexto`'s own documented
constraint: the LCG's modulus is `65536` (a 16-bit range — see
`prng.rap`'s own comments), so any seed at or above that value is
already "wrapped" in a sense the multiplier arithmetic doesn't
tolerate. Concretely, `32765 × 424242 ≈ 13.9 billion` — more than 6×
`INT32_MAX` — so `mulo` (correctly, per the overflow fix in Part 4)
returned `NoYield`, collapsing the entire goal chain silently, with no
error message beyond the caller's own generic "produced no solution."

**What made this hard to find**: two full rounds of careful, correct
static tracing through compiled BVar output (checking argument order,
relation composition, entry shapes — everything described in items
1-8 above) found nothing wrong, because there *was* nothing
structurally wrong. The bug was a plain arithmetic domain violation on
a literal value, not a logic or composition error — exactly the kind
of thing structural tracing is not designed to catch, since it was
looking for "is this wired together correctly" rather than "is this
constant even a legal input." `--trace` producing zero dynamic output
(no ChangeSet dumps, no Probe outcomes) turned out to be the correct
and sufficient clue — it meant the failure was very early, before any
`Probe` was reached — but even with that clue in hand, the actual
fix required checking one specific relation's own documented domain
constraint against one specific literal, not further structural
tracing.

**Standing rule**: when a relation with a documented input range or
domain constraint (e.g. `lcg-nexto`'s implicit `[0, 65535]` seed
range, or `divo`/`modo`'s `b > 0` requirement) is called with a
hardcoded literal, explicitly check that literal against the
constraint before trusting the call — do not just check that the
relation is being called with the right number of arguments in the
right order. This is a distinct check from every other item in this
list, and is easy to skip precisely because "the code looks right"
structurally.

## Part 3: Working with `Probe`

`(probe Goal Condition Budget Sandbox ReqGround)`:

- `Goal` — the sub-goal to evaluate. Must be literal syntax at the
  call site (see Part 2, item 3).
- `Condition` — one of the symbols `true`, `false`, `insufficient`,
  `bounded`. `Probe` succeeds exactly when `Goal`'s actual outcome
  matches `Condition`.
- `Budget` — max iteration count (a plain integer, e.g. `100`).
- `Sandbox` — `true`/`false`. When `true`, `Goal`'s internal variable
  bindings do not propagate outward, **and** any `cons-ops`/`add`/
  `remove`/`output` ChangeSet operations `Goal` generates internally
  are also isolated and do not leak into the outer ChangeSet (verified
  by `test_stage2.cpp`'s "sandboxed probe op excluded" test). Use
  `true` almost always when you only care about success/failure, not
  about what `Goal` bound or attempted internally.
- `ReqGround` — `true`/`false`. When `true`, unground variables in
  `Goal` short-circuit to `insufficient` before evaluation begins.
  Usually `false` unless you specifically need this.

**Common idiom** for converting a goal's success/failure into an
explicit outcome symbol (used repeatedly in Stage 2.2 and 2.5):

```
(defrel (check-somethingo ... result)
  (disj
    (conj (probe (some-goal ...) true 100 true false) (== result yes))
    (conj (probe (some-goal ...) false 100 true false) (== result no))))
```

This gives you a genuinely mutually-exclusive, ground result you can
safely branch on — unlike a naive `disj` (see Part 2, item 2).

## Part 4: Notable Engine-Level Fixes (for historical context)

These are past bugs, already fixed, documented here so future work
understands what's already been addressed and doesn't need
re-discovering:

- **ChangeSet `op_count` backtracking bug**: `op_count` wasn't
  correctly rewound on backtrack, and could leak ops from a failed
  branch, or from inside a sandboxed `Probe`, into the final
  ChangeSet. Fixed by tracking a `saved_client_count` through `State`
  and restoring it at the same points the bump-pointer arena offset
  is restored.
- **`divo`/`modo` couldn't compute quotient+remainder from known
  dividend+divisor**: the underlying `multaddiso` built-in bailed out
  whenever 2+ of its 4 arguments were unbound, which was exactly the
  case needed for ordinary mod/div computation. Fixed by adding a
  dedicated `divmodo` built-in handling all binding patterns directly,
  with floor-division semantics for negative inputs.
- **Silent integer overflow**: arithmetic built-ins computed correctly
  in `int64` internally but silently wrapped when casting the result
  down to `int32` if the true result didn't fit — with no error, no
  distinct outcome, just a wrong answer. Fixed by checking
  fits-in-int32 before every such cast and returning `NoYield`
  (ordinary relational failure) instead of wrapping.
- **State-holder / non-runnable agenda entries**: an earlier engine
  implementation privileged entries whose stored `args` were non-`Nil`
  as passive "state-holders," never actually dequeued and run as real
  queries — contradicting the paper's own documented semantics (every
  agenda entry is runnable). Fixed by generalizing the calling
  convention to properly support 2- or 3-parameter agenda entries
  (see Part 1) and removing the state-holder concept entirely.
  `examples/wc.rap` was updated to match (its `wc-counter` relation now
  genuinely runs and does the counting itself, rather than being a
  passive cell read by `handle_input`).
- **`enqueue_handle_input`'s De Bruijn index swap**: a 2-param wrapper
  had its `agenda`/`ops` positions swapped due to a De Bruijn indexing
  mistake (innermost/last-pushed parameter is index 0, not the first
  parameter) — found only via targeted tracing, not static reading.
- **`poll()` `POLLNVAL` vs `POLLHUP`**: macOS returns `POLLNVAL`, not
  `POLLHUP`, when polling a stdin fd redirected from `/dev/null` —
  causing the reactive loop to spin forever without detecting EOF.
  Fixed by handling `POLLNVAL` explicitly. Also fixed a missing
  `fflush(stdout)` that could lose buffered output if the process was
  still running (or crashed) before an implicit flush occurred.
- **`deep_copy_term` compaction corruption (Stage 2.6)**: a genuine
  memory-safety bug in the core engine, not in any `.rap` code. When
  `Agenda::remove` compacts the queue buffer (sliding a later, larger
  entry into the gap left by a removed one), `deep_copy_term` copied a
  pair's `car` recursively, then only afterward read and copied its
  `cdr` — `p->cdr = deep_copy_term(dest, t.pair->cdr)` reads
  `t.pair->cdr` from source memory *after* the `car` recursion has
  already returned. If the destination writes from copying `car`
  happened to land on the same physical bytes as the *source* node's
  still-unread `cdr` field (which can happen when a compaction's
  source and destination ranges overlap), the `cdr` read back a
  corrupted value — observed as a `Nil` field silently becoming a
  stray `Var(2)`. This surfaced as some, but not all, of a batch of
  newly-added agenda entries mysteriously failing (arity/destructure
  mismatches against corrupted data) with no error and no crash,
  several steps removed from the actual write that caused it — one of
  the hardest bugs found in this project, requiring byte-level address
  tracing to pin down. Fixed by saving both `car` and `cdr` as local
  copies *before* either recursive `deep_copy_term` call, so neither
  can be corrupted by the other's writes. See Part 2, item 8 for how
  this was found and what it implies about testing at larger scale.
- **`Probe` budget too small at larger scale (Stage 2.6)**: a `Probe`
  budget of `100` (used inside `find-new-matches-collecto`, wrapping
  `has-new-pairo`) was sufficient for the small hand-built belief
  states used in isolated testing, but silently insufficient once
  belief was fully populated across a real 16-position board — the
  search needed closer to ~104-122 steps to exhaust a longer list,
  causing `Probe` to return `bounded` (an indeterminate outcome, not a
  clean `true`/`false`) rather than the expected answer, which made
  `find-new-matches-collecto` fail to find real matches with no error
  at all. Fixed by raising the budget to `300`. See Part 2, item 8 —
  this is the same underlying lesson as the `deep_copy_term` bug:
  isolated tests with small inputs can pass cleanly while a bug (or,
  here, an insufficient constant) only manifests at realistic scale.
- **Abandoned: parameterized `noto`/negation-as-failure stdlib
  proposal**: based on the (incorrect, at the time) assumption that
  `probe` could take a variable holding a goal. Infeasible as designed
  — see Part 2, item 3. A real fix would require extending `probe` to
  accept a first-class `rel` term as its goal; not yet implemented.

## Part 5: Known Future Work

- **A real module/import system.** The single biggest recurring
  friction and error source in this project has been manually copying
  verified relation definitions between files (Option B, chosen
  deliberately for now: no coupling, no unneeded shared state — but at
  the cost of duplication and the retyping-error risk in Part 2, item
  4). A real module system would let verified blocks live in one place
  and be imported, eliminating this whole class of error.
- **A real `noto`/negation-as-failure mechanism**, once `probe` (or an
  equivalent) can accept a first-class relation value as its goal.
- **Tag belief's agenda-entry shape** before it becomes a real problem
  (Part 2, item 6).
