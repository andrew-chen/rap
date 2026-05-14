# RAP Implementation Roadmap

**Project:** Relational Agenda Programming (RAP)  
**Last updated:** May 8, 2026  
**Paper deadline:** May 15, 2026 AoE

---

## Status Overview

| Stage | Name | Status |
|-------|------|--------|
| Core | µKanren + Probe engine | ✅ Complete |
| Security | Workshop paper case studies | ✅ Complete |
| 0A | Anonymous relations + call dispatch | ✅ Complete |
| 0B | Evaluator class + extension mechanism | ✅ Complete |
| 0C | Automated test suite | ✅ Complete |
| 2 | Work queue + ChangeSet machinery | ✅ Complete |
| Paper | Onward! 2026 submission draft | ✅ Draft complete — needs measurements + citations |

---

## Core (Complete)

Self-contained µKanren engine. Foundation for everything else.

**Key features:** `==`, `fresh`, `disj`, `conj`, `Probe` (four-valued bounded meta-evaluation), fully iterative execution, arena bump allocator, FNV-1a symbol interning, De Bruijn index compilation, s-expression parser with full diagnostic error messages, `print_sexp`/`print_goal`/`print_query`.

**Files:** `core/mktypes.hpp`, `core/arena.hpp`, `core/intern.hpp`, `core/core.hpp`, `core/sexp_parser.hpp`

---

## Security Layer (Complete)

Workshop paper application. Demonstrates `core/` is genuinely embeddable.

**Key features:** RBAC access control (role lookup + permission check via `Probe` + `fresh`), network policy (flow allow/deny via structural unification), 10/10 correct, 2–7 µs per query, no dependency on RAP layer.

**Files:** `security/security_test.cpp`

**Target venue:** miniKanren Workshop @ ICFP 2026, Indianapolis. Deadline late May 2026.

---

## Stage 0A — Anonymous Relations + Call Dispatch (Complete)

**Key features:** `mktypes.hpp`, `TermTag::Rel`, `GoalTag::Call`, `RelEnv` with inline `rel_cache`, call dispatch chain, `(rel ...)` / `(call ...)` / `(defrel ...)` surface syntax, closed relations with outer `fresh` variables visible as `Var` references (enables mutual recursion), unified symbol treatment, `walk` extended for `Sym` → `rel_env` resolution, backward-compat `runN` shim, Programs 7–12 including mutual recursion (Program 10).

**Spec:** `docs/STAGE_0A.md`

---

## Stage 0B — Evaluator Class + Extension Mechanism (Complete)

**Key features:** `Evaluator` base class, `handleUnknownRelation` as single virtual method, `ClientId` enum, `ClientRegion` with `client_offset` in `State` (saved/restored on backtrack automatically), `RapEvaluator` with `ClientId::RAP`, `sym_no_ops_`/`sym_cons_ops_`/`sym_empty_ops_` interned at construction.

**Key design decision:** OO used only for this single extension point. One level of subclassing.

**Spec:** `docs/STAGE_0B.md`

---

## Stage 0C — Automated Test Suite (Complete)

**Key features:** `core/test_extension` (4 tests of base `Evaluator`), `rap_test_extension` (7 tests of `RapEvaluator` backtrack behavior), `make test` target.

**Spec:** `docs/STAGE_0C.md`

---

## Stage 2 — Work Queue + ChangeSet Machinery (Complete)

**Key features:**

**Memory:** Queue 2 ring-buffer arena (bump alloc/dealloc + pointer-rewriting compaction — NOT memmove), ChangeSet arena (placement-new reset), spine arena (short-lived Pair nodes), `deep_copy_term` as the core pointer-rewriting copy primitive used by all three.

**ChangeSet:** Real `no-ops`/`cons-ops` replacing Stage 0B stubs. `no-ops` unifies arg with `empty-ops` sentinel. `cons-ops` validates Op term, deep-copies into ChangeSet arena, unifies OpsOut with current op count for chaining. Client region backtrack-safe via `client_offset` in `State`.

**RapLoop:** Reactive execution loop. Queries are `Rel` terms with `param_count=1` (agenda as single argument). `run_one()` builds agenda list term (spine arena), calls query with it, extracts ChangeSet, applies atomically.

**Search strategy:** DFS (LIFO VM queue) — required for `strengthen-agendao` case study. BFS finds "skip item" branch first (empty result); DFS finds "collect item" branch first (complete result). Trade-off documented in paper Section 4.3.

**Bugs found and fixed during implementation:**
- `RestoreEnv` continuation tag — callee's env must not leak into caller's continuation goals
- `deep_resolve_bvar` for compound args before `handleUnknownRelation`
- `save src before deep_copy_term` in `remove()` — prevents header corruption when copy destination overlaps source

**Validation:** `test_stage2` passes 18/18. `strengthen-agendao` produces Remove(10), Remove(12), Output((pruned hypA test1)) on 4-entry test agenda.

**Spec:** `docs/STAGE_2.md`

---

## Paper (Draft Complete)

**File:** `rap_paper_draft.md` (in outputs)

**Status:** All `[OPT-D]`/`[OPT-FULL]` markers resolved. Full implementation framing throughout.

**Remaining before submission:**
- Fill `[RESULT: ...]` placeholders with actual measurements (hardware, query latency, agenda operation latency)
- Fill `[CITE: ...]` placeholders with real ACM SIGPLAN citations
- Convert Markdown → ACM two-column format
- Decide Appendices A and B (include or cut based on page count)
- Double-blind check: remove identifying information
- Submit by May 15, 2026 AoE

---

## Future Work (Post-Paper)

**Near-term:**
- Amortized growth for fixed-size arenas
- True circular buffer wraparound for Queue 2 (currently OOM on wrap; compaction would need two-pass or back-to-front approach)
- Hash table for `RelEnv`
- Configurable search strategy (DFS vs BFS)

**Medium-term:**
- `stdlib/` layer — standard relations, `defrel` moved from parser
- Partial application (`TermTag::PartialRel`)
- Static stratification analysis

**Language introspection:**
- `parseo` — string → term, bidirectional
- `int-stringo(N, S)` — integer/string relation (atoi/itoa equivalent)
- `lexeo` — token-level relation (forward straightforward, reverse is string generation)
- General principle: make implementation machinery accessible as relations (homoiconicity at implementation level)

**FLCT integration:**
- Frame operations as ChangeSet op types via `handleUnknownRelation`
- Persistent agenda state
- Multi-engine coordination via shared agenda terms

---

## Paper Target

**RAP paper:** "Relational Agenda Programming: A Uniform Execution Model for Embedded Reasoning Systems"  
**Venue:** Onward! Papers @ SPLASH 2026, Oakland  
**Submission deadline:** May 15, 2026 AoE  
**Decisions:** June 22, 2026  
**Revision deadline:** July 27, 2026  
**Camera-ready:** August 25, 2026  
**Presentation:** October 2026

**Workshop paper:** Embedded security policy verification  
**Venue:** miniKanren Workshop @ ICFP 2026, Indianapolis  
**Deadline:** Late May 2026
