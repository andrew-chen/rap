# RAP Implementation Roadmap

**Project:** Relational Agenda Programming (RAP)  
**Last updated:** May 7, 2026  
**Paper deadline:** May 15, 2026

---

## Status Overview

| Stage | Name | Status |
|-------|------|--------|
| Core | µKanren + Probe engine | ✅ Complete |
| Security | Workshop paper case studies | ✅ Complete |
| 0A | Anonymous relations + call dispatch | 🔄 In progress |
| 0B | Evaluator class + extension mechanism | ⬜ Not started |
| 0C | Automated test suite | ⬜ Not started |
| 1 | Extension mechanism (Stage 1 per original spec) | ⬜ Not started |
| 2 | Work queue + ChangeSet machinery | ⬜ Not started |

---

## Core (Complete)

**What it is:** The self-contained µKanren engine. Foundation for everything else.

**Key features:**
- `==` (unification), `fresh` (logic variables), `disj`, `conj`
- `Probe` — four-valued bounded meta-evaluation: `true`, `false`, `insufficient`, `bounded`
- Fully iterative execution — explicit stacks, no recursion, no stack overflow risk
- Fair FIFO search — breadth-first interleaving of disjunctive branches
- Arena bump allocator — zero heap allocation, all types trivially destructible
- FNV-1a symbol interning — pointer-identity equality
- De Bruijn index compilation for `fresh` variable binding
- S-expression surface syntax parser
- Diagnostic tooling: `print_sexp`, `print_goal`, `print_query`

**Files:** `core/arena.hpp`, `core/intern.hpp`, `core/core.hpp`, `core/sexp_parser.hpp`

---

## Security Layer (Complete)

**What it is:** Workshop paper application. Demonstrates `core/` is genuinely embeddable.

**Key features:**
- Case Study 1: RBAC access control — role lookup + permission check via `Probe` + `fresh`
- Case Study 2: Network policy — flow allow/deny via structural unification
- 10/10 test cases correct (3 ACL permitted, 3 ACL violations, 2 network permitted, 2 network violations)
- Query latency: 2–7 µs per query (after removing diagnostic output)
- C++ wrapper pattern: `build_query` + `check` + `generate_report`
- No dependency on RAP layer — uses `core/` only

**Files:** `security/security_test.cpp`

**Target venue:** miniKanren Workshop @ ICFP 2026, Indianapolis. Deadline late May 2026.

---

## Stage 0A — Anonymous Relations + Call Dispatch (In Progress)

**What it is:** Adds user-defined relations to the core language. Required before everything else.

**Key features:**
- `mktypes.hpp` — forward declarations for all types, breaks circular dependencies
- `TermTag::Rel` — anonymous relation as a first-class term value
- `RelNode` — `{ param_count, body }` — POD, arena-allocated
- `GoalTag::Call` — invoke a relation term with arguments
- `RelEnv` — runtime relation environment populated by `defrel`
- Inline `rel_cache` on `SymEntry` — O(1) lookup for top-level named relations
- Call dispatch chain: `Rel` → `rel_cache` → `RelEnv` scan → `handleUnknownRelation`
- `(rel (params...) body)` — anonymous relation surface syntax
- `(call f arg...)` — call surface syntax, all resolution deferred to runtime
- `(defrel (name params...) body)` — top-level sugar, sets `rel_cache`
- Closed relations: body uses fresh `BoundBind` for parameters; outer `fresh` variables visible as `Var` references via `genv` (enables mutual recursion)
- Unified symbol treatment: symbols compile to `Term::symbol`, resolved at runtime
- `walk` extended to resolve `Sym` through `rel_env`
- `handleUnknownRelation` free function — returns `StepResult::NoYield` (Stage 0B converts to virtual)
- Backward-compat 7-arg `runN` shim for `security/` (which cannot be modified)
- Multi-form `parse_query`: `defrel*` then `run`
- Programs 7–12 in `parse_run.cpp` validate all new features

**Key design decisions:**
- Relations are terms — first-class, passable as arguments, bindable via `==`
- `defrel` is sugar — will move to `stdlib/` when that layer exists
- Partial application is future work — current design supports it without changes to dispatch
- Frames (in FLCT) are more general than closures — no heap-allocated closures needed

**Spec:** `docs/STAGE_0A.md`

---

## Stage 0B — Evaluator Class + Extension Mechanism (Not Started)

**What it is:** Introduces OO structure to the evaluator. Enables the RAP layer's extension point.

**Key features:**
- `Evaluator` base class — owns arena, intern table, `OutcomeSyms`, `ClientRegion`, `State`
- `step()` and `runN()` move from free functions to methods
- One virtual method: `handleUnknownRelation(name, args, arg_count, state)`
- `ClientId` enum — compile-time ID for each client of the extension mechanism
- `ClientRegion` — opaque byte region in arena managed by client; bump pointer saved/restored with `State`
- `client_offset` added to `State` — saved/restored on backtrack automatically
- `RapEvaluator` subclass — sets `ClientId::RAP`, overrides `handleUnknownRelation`
- Stub `no-ops` and `cons-ops` in `RapEvaluator` — prove mechanism works, replaced in Stage 2
- Client region save/restore verified under nested choice points

**Key design decisions:**
- OO used only for this single extension point — no broader OO in the codebase
- Multiple evaluator instances with different clients are safe — each carries its own behavior
- Client region contents are opaque to core — core only saves/restores the bump pointer
- Wrong `ClientId` returns `GoalResult` (failure or unknown) — never crashes or asserts
- `parse_run.cpp` updated to construct `Evaluator` instance instead of calling free functions

**Spec:** `docs/EXTENSION_MECHANISM.md` (to be updated and committed after Stage 0B is written)

---

## Stage 0C — Automated Test Suite (Not Started)

**What it is:** Locks down all existing behavior before Stage 1 builds on top of it.

**Key features:**
- `core/test_extension.cpp` — tests extension mechanism at the base `Evaluator` level
- `rap/test_rap_extension.cpp` — tests `RapEvaluator` with stub `no-ops`/`cons-ops`
- Regression tests for all six original core programs
- Tests for all Stage 0A features (defrel, call, mutual recursion, relational backward execution)
- Negation correctness tests (planned for after negation is implemented)
- ChangeSet validity tests (planned for Stage 2)

**Test cases (extension mechanism):**
1. Unknown relation, no client registered → failure (not crash)
2. Default `handleUnknownRelation` returns failure
3. Choice point save/restore includes `client_region_offset`
4. `no-ops` succeeds and allocates into client region
5. Backtrack rewinds client region
6. `cons-ops` succeeds with three arguments
7. `cons-ops` fails with wrong argument count
8. Wrong `ClientId` → failure (not crash)
9. Nested choice points with client allocations at multiple levels
10. Fully unknown relation in `RapEvaluator` → failure

---

## Stage 1 — Extension Mechanism (Superseded by 0B/0C)

*Note: What was originally called "Stage 1" in the extension mechanism spec has been
split into Stage 0B (implementation) and Stage 0C (test suite). The original Stage 1
document describes the same content as 0B + 0C combined.*

---

## Stage 2 — Work Queue + ChangeSet Machinery (Not Started)

**What it is:** The full RAP execution model. This is the paper's core contribution.

**Key features:**

**Memory architecture:**
- Queue 2 arena — single fixed-size bump allocator with bump deallocation, circular with compaction on mid-queue removal
- ChangeSet arena — fixed-size, reset wholesale after each query
- List spine arena — short-lived `Pair` nodes for agenda list term, reset after each query
- All arenas: fixed size sufficient for paper examples; amortized growth is future work

**Queue 2 (Agenda):**
- Single-engine, single-threaded pending-query list
- Contents exposed as a relational term — queries reason over pending work using unification
- Enqueue, dequeue, apply ChangeSet (add/remove pending queries by stable ID)
- Agenda term constructed fresh at query invocation time from spine arena

**ChangeSet:**
- `no-ops` and `cons-ops` implemented via Stage 0B extension mechanism
- Valid by construction — `cons-ops` fails on invalid operation, so invalid ChangeSets cannot be expressed
- Inductive validity theorem: any constructible ChangeSet is valid
- Operations: `add(Query)`, `remove(QueryId)`, `output(Term)`
- Backtrack-safe — client region rewind in `RapEvaluator` undoes partial ChangeSet construction

**Reactive execution loop:**
1. Consult agenda (Queue 2) — select next query
2. Remove query from agenda
3. Execute via VM (Queue 1) to completion
4. Query produces: answers + ChangeSet term + output terms
5. Interpret ChangeSet — apply agenda modifications atomically
6. Send output terms to output queue (write-only)
7. Repeat

**Output queue:** Write-only from query perspective. No feedback into reasoning loop unless embedder explicitly constructs an input query from output data.

**Key design decisions:**
- Single-threaded, single-process — no concurrency, no TOCTOU
- Agenda modified only between query executions — consistency guaranteed by construction
- All I/O flows through same mechanism as control and reasoning — uniform model
- Agenda is a term — meta-level and object-level use the same language
- ChangeSet is a term — valid by construction via relational constructors

---

## Case Studies (Paper)

### Section 5.1 — Embedded Configuration Validation
Demonstrates `Probe`-based negation with explicit Unknown outcome.
- Four options: `debug`, `optimize`, `sanitize`, `target32`
- Three named rules
- Four scenarios: valid, prohibited (named rule), unsatisfiable (interaction), budget-induced Unknown
- Named rule violations reported as output terms — explanation produced by same relational machinery as result

### Section 5.2 — Reflective Agenda Reasoning
Demonstrates a query reasoning over the agenda term via unification.
- `strengthen-agendao` — detects strong check in agenda, removes weaker redundant checks
- Two-phase: `collect-weak-qidso` (structural traversal) then `qids->remove-opso` (fold via `cons-ops`)
- Dominance pruning extension: `subsumeso` for semantic subsumption
- C++ procedural baseline shows the "seam" that relational agenda programming eliminates
- Differentiates from CLIPS (procedural inspection), meta-interpreters (internal control), fine-grained step semantics (visualization only)

---

## Future Work (Post-Paper)

- Production-grade performance optimization
- Additional language bindings beyond C FFI
- Static stratification analysis (complement to runtime budget heuristic)
- Tabling and memoization
- Advanced search strategies (iterative deepening, probabilistic)
- Persistent agenda state across engine restarts
- Formal verification of ChangeSet validity theorem
- Partial application (`TermTag::PartialRel`)
- `stdlib/` layer with `defrel` and standard relations (`membero`, `appendo`, etc.)
- Multi-engine coordination via shared agenda terms
- Frame system integration (FLCT paper, Onward! 2027)

---

## Paper Target

**RAP paper:** "Relational Agenda Programming: A Uniform Execution Model for
Embedded Reasoning Systems"  
**Venue:** Onward! Papers @ SPLASH 2026, Oakland  
**Submission deadline:** May 15, 2026 AoE  
**Decisions:** June 22, 2026  
**Revision deadline (if conditional accept):** July 27, 2026  
**Camera-ready:** August 25, 2026  
**Presentation:** October 2026

**Workshop paper:** Embedded security policy verification  
**Venue:** miniKanren Workshop @ ICFP 2026, Indianapolis  
**Deadline:** Late May 2026
