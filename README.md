# RAP — Relational Agenda Programming

## What This Is

RAP is a C++20 logic programming engine designed for embedding in larger systems.
It implements **Relational Agenda Programming**: an execution model in which
pending queries are first-class terms, query results atomically modify a work
agenda via a ChangeSet mechanism, and reasoning, control flow, and I/O all flow
through the same uniform relational machinery with no privileged escape hatches.

The system has two layers:

- **`core/`** — a self-contained µKanren engine extended with `Probe`, a novel
  four-valued bounded meta-evaluation primitive. This layer is complete.
- **`rap/`** — the agenda layer: work queue (Queue 2), ChangeSet construction
  and interpretation, the reactive execution loop, and the embedding API.
  This layer is partially scaffolded; the work queue is not yet implemented.

A third application layer (`security/`) is planned for a companion workshop
paper on embedded security policy verification. It will depend only on `core/`,
not on `rap/`.

---

## Layer Structure

```
rap/
├── core/                    # Complete: µKanren + Probe engine
│   ├── arena.hpp            # Bump allocator (injected buffer, POD-only)
│   ├── intern.hpp           # FNV-1a symbol interning (pointer-identity eq.)
│   ├── core.hpp             # Logic kernel: terms, goals, unify, evaluator
│   └── sexp_parser.hpp      # Tokenizer, s-expr parser, goal compiler, printer
│
├── rap/                     # Partial: agenda layer (work queue not yet implemented)
│   ├── rap.hpp              # RapEngine: thin wrapper over core evaluator
│   ├── work_queue.hpp       # Placeholder — see "What Needs to Happen" below
│   └── test_rap.cpp         # Smoke test: RapEngine instantiation + query
│
├── security/                # Planned: workshop paper application layer
│                            # Depends on core/ only, not on rap/
│
├── parse_run.cpp            # Driver: 6 example programs (not part of any layer)
└── Makefile                 # Builds parse_run and test_rap
```

---

## The Execution Model (RAP)

The RAP layer implements a single-threaded reactive loop over two queues and
one output channel:

1. Consult **Queue 2** (the agenda) to select the next query term
2. Remove the selected query from the agenda
3. Execute the query via **Queue 1** (the internal VM queue) to completion
4. The query produces a result: answers, a **ChangeSet** term, and output terms
5. Interpret the ChangeSet — apply agenda modifications atomically
6. Append output terms to the **output queue** (write-only)
7. Return to step 1

**Queue 1 (VM queue):** Internal to query execution. Manages goal resolution
iteratively. Prevents stack overflow. Enables bounded execution and the
`Probe` Unknown outcome. Not directly accessible to the embedder.

**Queue 2 (Agenda):** The pending query list. A **term** in the same relational
space as all other data. Passed as input to queries. Queries can reason over it
using unification — the same machinery used for domain reasoning. Modified only
between query executions, never during one.

**ChangeSet:** The portion of a query result specifying agenda modifications.
A term constructed using two foundational relations:

```prolog
valid-ops(no-ops).
valid-ops(cons-ops(Op, Rest)) :- valid-op(Op), valid-ops(Rest).
```

Where `valid-op` covers: `add(Query)`, `remove(QueryId)`, `output(Term)`.
Because `cons-ops` fails if given an invalid operation, any successfully
constructed ChangeSet is **valid by construction** — no runtime validation
step is needed.

**Output queue:** Write-only from the query's perspective. Queries specify
output terms in their ChangeSet; the runtime appends them. There is no feedback
loop from output back into the reasoning layer unless the embedder explicitly
constructs an input query from output data.

---

## The Core Engine (`core/`)

### Key Properties

- **No dynamic allocation** — all data lives in a caller-supplied fixed-size
  arena. `arena.reset()` reclaims everything.
- **All POD types** — every struct is trivially destructible.
- **Fully iterative** — unification and search use explicit job stacks; no
  recursion, no stack overflow risk.
- **Fair search** — FIFO work queue gives breadth-first interleaving of
  disjunctive branches.

### Terms

`Var` (logic variable), `BVar` (de Bruijn bound variable), `Int`, `Sym`
(interned), `Nil`, `Pair`

### Goals

`Eq` (unify), `Conj`, `Disj`, `Fresh` (lexical binder), `Probe`
(bounded meta-evaluation — see below)

### Surface Syntax (s-expressions)

```
(run N (q) GOAL)
(== u v)
(conj g1 g2 ...)
(disj g1 g2 ...)
(fresh (x y ...) GOAL)
(probe GOAL condition max_iter sandbox req_ground)
```

### The `Probe` Primitive

`Probe` is a novel contribution of this engine. It runs a sub-goal under a
bounded iteration limit and returns one of **four outcomes**:

| Outcome       | Meaning                                              |
|---------------|------------------------------------------------------|
| `true`        | Sub-goal succeeded                                   |
| `false`       | Sub-goal failed (finite failure)                     |
| `insufficient`| Sub-goal could not be determined — unground variables|
| `bounded`     | Iteration limit hit before determination             |

Two control flags govern behaviour:

- **`sandbox`** — when true, bindings from sub-evaluation do not propagate
  outward. Enables meta-evaluation without side effects on the outer state.
- **`req_ground`** — when true, unground variables short-circuit to
  `insufficient` before execution begins.

`Probe` is the foundation for operationally controlled negation in the RAP
layer: negated subgoals are evaluated under explicit budgets, and
`bounded` maps to the paper's "Unknown" outcome — a runtime signal distinct
from both logical success and logical failure.

---

## Build

```bash
make          # builds parse_run (6 example programs) and test_rap (smoke test)
make run      # runs parse_run
make clean
```

All targets build clean under `clang++` (or `$CXX`) with
`-std=c++20 -O2 -Wall -Wextra -pedantic -Werror`.

---

## Current Status

| Layer      | Status                                              |
|------------|-----------------------------------------------------|
| `core/`    | Complete. All features implemented and tested.      |
| `rap/`     | Scaffolded. `RapEngine` wraps core; smoke test passes. Work queue not implemented. |
| `security/`| Not yet started. Waiting for `core/` to stabilise (it has). |

---

## What Needs to Happen Next

### 1. Implement `rap/work_queue.hpp`

This is the immediate priority. The work queue (Queue 2 / agenda) needs to
support at minimum:

- Enqueue a query term
- Dequeue the next query term for execution
- Apply a ChangeSet — add queries, remove pending queries by stable ID
- Expose the current queue contents as a term (for reflective agenda reasoning)
- Accept output terms and append them to a write-only output queue

This is **not** a multi-agent or distributed scheduler. It is a single-engine,
single-threaded pending-query list whose contents are a relational term.

### 2. Implement the reactive execution loop in `rap/rap.hpp`

The loop described in "The Execution Model" section above. Drives queries from
Queue 2 through the core evaluator, applies ChangeSets, routes outputs.

### 3. Implement ChangeSet construction relations

`no-ops` and `cons-ops` as relational primitives. The inductive validity
theorem (any constructible ChangeSet is valid) should be verified against
the implementation.

### 4. Validate case studies

Two case studies are designed for the paper:

- **Configuration validation** — demonstrates `Probe`-based negation with
  explicit Unknown outcome
- **Reflective agenda reasoning** — demonstrates a query reasoning over the
  agenda term, finding redundant pending work via unification, and emitting
  a ChangeSet that prunes it

Both are specified in detail in the paper design. Once the RAP layer is
implemented, these need to run correctly against the working code.

### 5. Scaffold `security/`

After `core/` is confirmed stable (it is), the workshop paper application
layer can be started. This depends only on `core/`, not on `rap/`.

---

## Paper Context

This codebase supports two papers:

**RAP paper** — "Relational Agenda Programming: A Uniform Execution Model for
Embedded Reasoning Systems" — target: Onward! Papers at SPLASH 2026,
submission deadline May 15, 2026. Requires full RAP layer implementation.

**Workshop paper** — embedded security policy verification using `core/` only —
target: miniKanren Workshop at ICFP 2026, deadline late May 2026. Does not
require the RAP layer.

---

## Key Design Decisions (Do Not Revisit Without Good Reason)

- **`Probe` belongs in `core/`**, not in `rap/`. It is a core language
  primitive used by both layers and by the security application.
- **The agenda is a term**, not a separate data structure that gets converted
  to a term on demand. This is load-bearing for the reflective agenda reasoning
  contribution.
- **ChangeSets are valid by construction** via `no-ops`/`cons-ops`. No
  runtime validation step. Invalid operations cause construction to fail
  relationally.
- **Output queue is write-only** from the query's perspective. Queries cannot
  observe pending output. This keeps the model clean; embedders who need
  feedback can construct input queries from output data.
- **Single-threaded, single-process**. No concurrency, no TOCTOU. The agenda
  is modified only between query executions, so consistency is guaranteed by
  construction.
