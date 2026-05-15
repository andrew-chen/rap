# RAP — Relational Agenda Programming

A C++20 logic programming engine designed for embedding in systems software.
Zero dependencies. Header-only. Callable via FFI from any language.

> **Papers:** This codebase supports two papers currently under review:
> - "Relational Agenda Programming: A Uniform Execution Model for
>   Embedded Reasoning Systems"
> - "Lightweight Runtime Security Policy Verification Using an Embeddable C++ miniKanren"
>
> Venue information will be added upon acceptance.

---

## What This Is

RAP is built on [miniKanren](http://minikanren.org) — specifically a C++20
implementation of the µKanren core extended with anonymous relations (`rel`,
`call`, `defrel`), disequality constraints (`=/=`), and a novel bounded
meta-evaluation primitive (`Probe`). If you know miniKanren, the surface
syntax and semantics will be immediately familiar.

The contribution beyond the engine itself is the **execution model**: most embedded reasoning systems draw a hard line between the reasoning layer
and the control layer. RAP eliminates that line.

The key idea: the pending-query agenda is a **first-class relational term**.
Queries receive the current agenda as input, reason over it using the same
unification machinery used for domain reasoning, and return a **ChangeSet**
specifying atomic agenda modifications. Reasoning, control flow, and I/O all
flow through the same uniform machinery — no privileged escape hatches.

Two properties follow from this design:

- **ChangeSet validity by construction.** ChangeSets are built using two
  relations — `no-ops` and `cons-ops` — where `cons-ops` fails on invalid
  operations. Any successfully constructed ChangeSet is valid. No runtime
  validation step is needed.

- **Reflective agenda reasoning.** A query can inspect pending work, detect
  redundancy via unification, and emit a ChangeSet pruning it. This is not
  an API feature — it falls out of the agenda being a term.

---

## Quick Start

```bash
git clone https://github.com/andrew-chen/rap
cd rap
make
./parse_run      # runs 13 example programs
make test        # runs full test suite
```

Requires a C++20 compiler (`clang++` or `g++`). No other dependencies.

---

## A Taste of the Language

The surface syntax is s-expressions. Here is `appendo` running forward and
backward in the same program:

```scheme
; forward: (1 2) ++ (3 4) = ?
(run 1 (q) (call appendo (1 2) (3 4) q))
; => ((1 2 3 4))

; backward: ? ++ (3 4) = (1 2 3 4)
(run 1 (q) (call appendo q (3 4) (1 2 3 4)))
; => ((1 2))
```

Mutual recursion via first-class anonymous relations:

```scheme
(run 5 (q)
  (fresh (eveno oddo)
    (== eveno (rel (n) (disj (== n 0)
                             (fresh (m) (conj (== n (s m))
                                             (call oddo m))))))
    (== oddo  (rel (n) (fresh (m) (conj (== n (s m))
                                        (call eveno m)))))
    (call eveno q)))
; => (0 (s s 0) (s s s s 0) ...)
```

Bounded meta-evaluation with `Probe` — the engine's novel primitive:

```scheme
; succeeds if the sub-goal finitely fails within 100 steps
(probe (call some-goal x) false 100 true false)
```

Disequality constraints:

```scheme
(run 3 (q)
  (fresh (x)
    (=/= x foo)
    (disj (== x foo) (== x bar) (== x baz))))
; => (baz bar)   ; foo excluded by constraint
```

Reflective agenda reasoning — a query that prunes redundant pending work:

```scheme
(defrel (strengthen-agendao agenda ops)
  (fresh (H T R strong-qid weak-qids ops0)
    (membero (q strong-qid (check+ H T R)) agenda)
    (collect-weak-qidso agenda H T weak-qids)
    (qids->remove-opso weak-qids ops0)
    (cons-ops (output (pruned H T)) ops0 ops)))
```

Given an agenda containing both `(check H T)` and `(check+ H T R)` entries,
this relation finds the strong check, collects all superseded weak checks via
unification, and emits a ChangeSet removing them. It runs correctly under any
search strategy because the branches are made mutually exclusive using `=/=`.

---

## Architecture

```
rap/
├── core/                   # Self-contained µKanren engine
│   ├── mktypes.hpp         # Forward declarations (breaks circular deps)
│   ├── arena.hpp           # Bump allocator (caller-supplied buffer, POD-only)
│   ├── intern.hpp          # FNV-1a symbol interning, pointer-identity eq.
│   ├── core.hpp            # Terms, goals, unifier, Evaluator class, Probe
│   └── sexp_parser.hpp     # S-expr tokenizer, goal compiler, printer
│
├── rap/                    # Agenda layer
│   ├── changeset.hpp       # Op, ChangeSet, deep_copy_term
│   ├── agenda.hpp          # Queue 2: ring-buffer, pointer-rewriting compaction
│   ├── spine.hpp           # Short-lived Pair nodes for agenda list term
│   ├── loop.hpp            # Reactive execution loop (RapLoop)
│   ├── rap.hpp             # RapEvaluator: no-ops, cons-ops, ClientRegion
│   ├── test_rap.cpp        # Extension mechanism tests
│   └── test_stage2.cpp     # strengthen-agendao validation
│
├── security/               # Embedded security policy case studies
│   └── security_test.cpp   # RBAC + network policy, 10/10 correct
│
├── parse_run.cpp           # 13 example programs including mutual recursion
└── Makefile
```

**`core/`** is completely self-contained. It can be used independently of
the RAP layer — the security case studies depend only on `core/`.

**`rap/`** builds on `core/` via a single virtual extension point
(`handleUnknownRelation`). The `Evaluator` base class owns evaluation;
`RapEvaluator` overrides the extension point to handle ChangeSet construction.
Client state (the ChangeSet being accumulated) lives in a `ClientRegion` whose
bump pointer is saved and restored with `State` on backtrack — making
ChangeSet construction automatically backtrack-safe.

---

## Key Implementation Properties

**No dynamic allocation.** All data lives in caller-supplied arenas.
`arena.reset()` reclaims everything. The system maintains several arenas
with different lifetimes: core (per-query), intern (long-lived), Queue 2
(ring-buffer with pointer-rewriting compaction), ChangeSet, and spine
(short-lived Pair nodes for the agenda list term).

**All POD types.** Every struct is trivially destructible. This enables the
arena strategy — no destructors to run, no ownership tracking needed.

**Fully iterative.** Unification and search use explicit job stacks. No
recursion in the engine, no stack overflow risk. (Exception:
`deep_copy_term` recurses over term structure; term depth is bounded in
practice and an iterative version is future work.)

**BFS interleaving.** The VM queue uses FIFO ordering, giving standard
miniKanren breadth-first interleaving. A depth-first variant is
straightforward to configure (swap push order in the `Disj` case).

**De Bruijn indices.** `fresh` variables compile to `BVar(k)` indices —
no alpha-renaming, no variable capture, evaluator stays purely structural.

**Disequality constraints.** `=/=` records constraints in `State.diseqs`
and checks them after every unification. The `not-weak-check-qido` guard
in the reflective agenda case study depends on this.

---

## Performance

Measured on Apple M2 Pro, macOS 14.8.3, Apple clang 16.0.0, `-O2`.
100 iterations after 10 warmup runs.

| Workload | Median latency |
|---|---|
| Network policy query (structural unification) | 3.6 µs |
| ACL query (two-step relational join) | 17 µs |
| `strengthen-agendao` via `run_one()` | 10.7 µs |
| `appendo` forward | 1.54 µs |
| `appendo` backward | 2.12 µs |

Latency scales with query complexity, not with embedding overhead.
Simple structural queries are sub-2 µs.

---

## Test Suite

```bash
make test
```

Runs five test binaries:

| Binary | What it tests |
|---|---|
| `parse_run` | 13 programs: core relational behavior, anonymous relations, mutual recursion, disequality |
| `security/security_test` | RBAC + network policy, 10/10 correct classifications |
| `test_rap` | Extension mechanism, RapEvaluator construction |
| `core_test_extension` | Base Evaluator backtracking, ClientRegion save/restore |
| `rap_test_extension` | RapEvaluator backtrack rewind, no-ops/cons-ops arity |
| `test_stage2` | `strengthen-agendao` full case study: Remove(10), Remove(12), Output((pruned hypA test1)) |

---

## The Probe Primitive

`Probe` is this engine's main extension to standard miniKanren.

```scheme
(probe Goal Condition Budget Sandbox ReqGround)
```

| Argument | Meaning |
|---|---|
| `Goal` | Subgoal to evaluate |
| `Condition` | Expected outcome: `true`, `false`, `insufficient`, or `bounded` |
| `Budget` | Maximum iteration count |
| `Sandbox` | If true, bindings do not propagate outward |
| `ReqGround` | If true, unground variables short-circuit to `insufficient` |

`Probe` succeeds when the actual outcome matches `Condition`.

The four outcomes distinguish things standard miniKanren collapses:
`false` is finite failure (proved); `bounded` means the budget was exhausted
(not proved either way); `insufficient` means the query was indeterminate
due to unground variables. This distinction matters for embedded systems
where "ran out of budget" and "logically false" require different responses.

---

## Embedding

```cpp
#include "core/sexp_parser.hpp"

// One-time setup
alignas(64) unsigned char mem[1 << 20];
Arena arena(mem, sizeof(mem));
Evaluator eval(&arena, &outcome_syms);

// Per-query
ParsedQuery pq = parse_query(arena, "(run 5 (q) (call appendo (1 2) (3) q))");
eval.runN(pq.n, pq.goal, pq.qvar, pq.vars_used, pq.rel_env,
    [](Term answer, State) {
        print_term(answer);
    });
arena.reset();
```

The caller owns the arena. The engine is a guest — it does not manage
process lifetime, threads, or I/O.

---

## Repository Layout

```
docs/          Stage specifications (internal development documentation)
core/          Self-contained engine — usable independently of the agenda layer
rap/           Agenda layer — Queue 2, ChangeSet, reactive execution loop
security/      Security policy case studies (depends on core/ only)
```

The `docs/` directory contains the stage-by-stage design specifications
written during development. They document design decisions and the rationale
behind them — useful context if you want to understand why things are the
way they are.

---

## License

Apache 2.0. See [LICENSE](LICENSE).

