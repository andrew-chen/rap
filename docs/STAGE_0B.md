# Stage 0B Specification: Evaluator Class and Extension Mechanism

**Version:** 1.0  
**Depends on:** Stage 0A complete (docs/STAGE_0A.md)  
**Required before:** Stage 0C (automated test suite), Stage 2 (work queue + ChangeSet)  
**Status:** Specification — not yet implemented  
**Date:** May 7, 2026

---

## Purpose

This document specifies the introduction of the `Evaluator` base class and
the extension mechanism that allows the RAP layer to participate in query
execution without the core knowing anything about RAP concepts.

When Stage 0B is complete:

- `step()` and `runN()` live inside an `Evaluator` base class as methods
- A single virtual method `handleUnknownRelation` replaces the Stage 0A
  free function of the same name
- Each `Evaluator` instance owns a `ClientRegion` — a managed byte region
  whose bump pointer is saved and restored with `State` on backtrack
- `RapEvaluator` subclasses `Evaluator`, registers as `ClientId::RAP`, and
  overrides `handleUnknownRelation` with stub `no-ops` and `cons-ops`
- All existing programs build and run correctly
- All existing security tests pass

This document supersedes the extension mechanism description in
`EXTENSION_MECHANISM.md` (which used "Stage 1" terminology and predates
several design decisions). `EXTENSION_MECHANISM.md` will be updated after
Stage 0B is complete to reflect the actual implementation.

---

## Why OO Here and Nowhere Else

The evaluator uses subclassing as the extension mechanism for one specific
reason: multiple evaluator instances with different clients must be safe.
If `handleUnknownRelation` were a free function or a function pointer passed
as a parameter, every call site would need to manually ensure the right
function and the right `ClientRegion` travel together. As soon as you have
multiple instances — which the FLCT architecture requires — this becomes
a maintenance burden. The instance carries its own behavior with it.

OO is used **only** for this one extension point. Nothing else in the
codebase uses inheritance. There is one level of subclassing: `Evaluator`
as base, `RapEvaluator` as the first (and currently only) subclass. No
deeper hierarchies.

The rest of the codebase remains POD structs and free functions.

---

## What Changes vs. What Stays the Same

**Changes:**

- `step()` and `runN()` move from free functions to methods on `Evaluator`
- `handleUnknownRelation` moves from a free function to a virtual method
- `State` gains a `client_offset` field (saved/restored on backtrack)
- `Evaluator` owns `ClientRegion`, `Arena*`, `Intern*`, `OutcomeSyms`
- `RapEvaluator` subclass added to `rap/rap.hpp`
- `parse_run.cpp` updated to construct an `Evaluator` instance
- `Makefile` dependency lists updated

**Does NOT change:**

- All POD term and goal structs — no changes
- Unification, `walk`, `resolve_bvar`, `ext_s` — no changes
- `Probe` machinery — no changes
- The call dispatch chain from Stage 0A — no changes
- `RelEnv`, `RelEnvEntry`, `defrel`, `rel_cache` — no changes
- `sexp_parser.hpp` — no changes
- `security/security_test.cpp` — must not be modified
- `rap/work_queue.hpp` — no changes (Stage 2)

---

## New Types to Add to core/mktypes.hpp

The following are already forward-declared in `mktypes.hpp` from Stage 0A
(added there in anticipation of Stage 0B). Verify they are present; if any
are missing, add them:

```cpp
enum class ClientId   : std::uint32_t;
struct ClientRegion;
```

---

## ClientId Enum

Add to `core/core.hpp` (after `OutcomeSyms`, before `State`):

```cpp
enum class ClientId : std::uint32_t {
    None = 0,   // uninitialized / no client registered
    RAP  = 1,   // Relational Agenda Programming layer (rap/)
    // Future clients: add new entries here.
    // Never reuse or renumber existing entries.
};
```

**Rules for future clients:**
- Add a new enumerator with a unique value
- Document what the client region contains in a comment next to the entry
- Never remove or renumber existing entries

---

## ClientRegion Struct

Add to `core/core.hpp` (after `ClientId`):

```cpp
struct ClientRegion {
    ClientId  id       = ClientId::None;
    uint8_t*  base     = nullptr;
    uint32_t  capacity = 0;
    uint32_t  offset   = 0;   // saved/restored on backtrack via State

    // Allocate n bytes. Returns nullptr if region full.
    void* alloc(uint32_t n) {
        if (offset + n > capacity) return nullptr;
        void* p = base + offset;
        offset += n;
        return p;
    }

    // Restore to a saved checkpoint. Called by backtrack machinery.
    void restore(uint32_t saved_offset) {
        offset = saved_offset;
    }
};

static_assert(std::is_trivially_destructible_v<ClientRegion>);
```

**The core's only obligation regarding ClientRegion:**

Save `client_region.offset` when saving a choice point.
Restore `client_region.offset` when backtracking.
Never read, write, or interpret the bytes in the region.
Never reference `ClientId::RAP` by name inside `core/`.

---

## State: Add client_offset

`State` is the value type that gets copy-saved into choice points and
restored on backtrack. Add `client_offset` to it:

```cpp
struct State {
    const Binding*  subst;
    const Diseq*    diseqs;
    const EnvFrame* env;
    std::uint32_t   counter;
    std::uint32_t   client_offset;  // ADD: saved/restored with State
};
```

**How this integrates with backtracking:**

The existing `Disj` case in `step()` copies `st` into both branches:

```cpp
case GoalTag::Disj: {
    State st_copy = st;   // copies all fields including client_offset
    // push both branches with st and st_copy respectively
}
```

When a branch backtracks and its saved `State` is restored,
`st.client_offset` is restored automatically. The evaluator then calls:

```cpp
client_region.restore(st.client_offset);
```

This rewinds any client region allocations made after the choice point was
saved. No additional save/restore machinery is needed — `State` copy
handles it.

**Initializing client_offset:**

In `runN()` and `probe_run()`, initialize `State` with
`client_offset = client_region.offset` (which starts at 0 for a fresh
evaluator). This ensures the initial state correctly reflects the empty
client region.

---

## Constants

Add to `core/core.hpp` (near the top, after includes):

```cpp
// Maximum bytes in the client region.
// 16 KiB is sufficient for all paper examples.
// Production systems should use amortized growth (future work).
constexpr std::uint32_t MAX_CHANGESET_ARENA = 16 * 1024;

// Maximum number of ChangeSet operations in a single query result.
// Not used by Stage 0B stubs but defined now for consistency with Stage 2.
constexpr std::uint32_t MAX_CHANGESET_OPS = 64;
```

---

## The Evaluator Base Class

Move `step()`, `runN()`, and `probe_run()` from free functions into an
`Evaluator` class. The class owns the evaluation context. Add
`handleUnknownRelation` as the single virtual method.

```cpp
// In core/core.hpp

class Evaluator {
public:
    // Construction: caller provides arena, intern table, outcome syms.
    // The evaluator does NOT own these — they are injected dependencies.
    Evaluator(Arena* arena, Intern* intern, const OutcomeSyms* syms)
        : arena_(arena), intern_(intern), syms_(syms)
    {
        client_region_.id       = ClientId::None;
        client_region_.base     = nullptr;
        client_region_.capacity = 0;
        client_region_.offset   = 0;
    }

    // Non-copyable, non-movable (owns the client region allocation).
    Evaluator(const Evaluator&)            = delete;
    Evaluator& operator=(const Evaluator&) = delete;

    // Run a parsed query, calling on_answer for each solution.
    template<typename OnAnswer>
    void runN(int n, const Goal* goal, Term qvar,
              std::uint32_t vars_used, const RelEnv& rel_env,
              OnAnswer&& on_answer);

    // 7-arg backward-compat overload for security/ and rap/ callers
    // that do not yet pass rel_env.
    template<typename OnAnswer>
    void runN(int n, const Goal* goal, Term qvar,
              std::uint32_t vars_used,
              OnAnswer&& on_answer) {
        runN(n, goal, qvar, vars_used, RelEnv{},
             std::forward<OnAnswer>(on_answer));
    }

protected:
    // Extension point: called when call dispatch fails to find a relation.
    // Default implementation returns StepResult::NoYield (failure).
    // Subclasses override to handle extension relations (no-ops, cons-ops, etc.)
    //
    // CONTRACT for overrides:
    // - Check client_region_.id before using the client region
    // - Return StepResult::NoYield (failure) or StepResult::Yield (success)
    //   based on relational semantics
    // - Do NOT crash or assert on unexpected ClientId values
    // - Allocations into client_region_ are automatically rewound on backtrack
    // - Do NOT recurse infinitely
    virtual StepResult handleUnknownRelation(
        const SymEntry* name,
        const Term*     args,
        std::uint32_t   arg_count,
        State&          st);

    // Accessible to subclasses for implementing extension relations.
    Arena*             arena_;
    Intern*            intern_;
    const OutcomeSyms* syms_;
    ClientRegion       client_region_;

private:
    // Internal evaluation methods (private — not part of extension API).
    StepResult step(Work* w, WorkQueue& q, State& st,
                    const RelEnv& rel_env, State& yielded);
    void probe_run(const Goal* goal, State st,
                   std::uint32_t max_iter, State& out_st,
                   const RelEnv& rel_env, Outcome& out);
};

// Default handleUnknownRelation — returns failure.
inline StepResult Evaluator::handleUnknownRelation(
    const SymEntry* name, const Term* args,
    std::uint32_t arg_count, State& st)
{
    (void)name; (void)args; (void)arg_count; (void)st;
    return StepResult::NoYield;
}
```

**Key design decisions:**

- `arena_`, `intern_`, `syms_` are injected (not owned) — callers manage
  lifetimes as before
- `client_region_` is owned by the evaluator instance
- `handleUnknownRelation` is `protected` virtual — subclasses override it,
  but external callers cannot call it directly
- `step()` and `probe_run()` are `private` — they are implementation details
  not part of the public API
- `runN()` is `public` — it is the API

**How GoalTag::Call uses handleUnknownRelation:**

In the `GoalTag::Call` case in `step()` (from Stage 0A), the fallthrough
from RelEnv lookup currently calls the free function:

```cpp
return handleUnknownRelation(rel_t.sym, g->call.args,
                             g->call.arg_count, st, a, q, w, k, yielded);
```

In Stage 0B, replace this with the virtual method call:

```cpp
return handleUnknownRelation(rel_t.sym, g->call.args,
                             g->call.arg_count, st);
```

The method has access to `arena_`, `client_region_`, and the work queue
through the instance — there is no need to pass them as parameters.

**Note on work queue access in handleUnknownRelation:**

The Stage 0A free function signature included `WorkQueue& q`, `Work* w`,
and `const Kont* k` as parameters to allow extension relations to push
new goals. The virtual method signature omits these. If an extension
relation needs to push goals, it should do so by returning a goal term
that the normal dispatch handles, not by directly pushing to the work
queue. For the stub `no-ops` and `cons-ops` in Stage 0B, this is not
needed — they only allocate into the client region and succeed or fail.
If Stage 2 requires direct work queue access for extension relations,
revisit this decision at that time.

---

## Intern Access: RapEvaluator Constructor Pattern

The base `Evaluator` class does **not** need an `Intern*` member. `runN()` and `step()` do not use `Intern` at runtime — symbol lookup uses `SymEntry*` pointer identity, and all interning is done at parse time.

However, `RapEvaluator::handleUnknownRelation` needs to recognize the names `"no-ops"` and `"cons-ops"`. The correct pattern:

1. `RapEvaluator` constructor takes `Intern*` as a parameter
2. In the constructor body, call `intern_cstr` to intern `"no-ops"` and `"cons-ops"` once
3. Store the resulting `SymEntry*` pointers as private members (`sym_no_ops_` and `sym_cons_ops_`)
4. In `handleUnknownRelation`, compare using pointer identity: `name == sym_no_ops_`
5. The `Intern*` does **not** need to be stored after construction

```cpp
class RapEvaluator : public Evaluator {
    // ...
    const SymEntry* sym_no_ops_;
    const SymEntry* sym_cons_ops_;

public:
    RapEvaluator(Arena* arena, Intern* intern, const OutcomeSyms* syms)
        : Evaluator(arena, syms)
    {
        sym_no_ops_   = intern_cstr(*arena, *intern, "no-ops");
        sym_cons_ops_ = intern_cstr(*arena, *intern, "cons-ops");
        // ... client region setup ...
    }
};

// In handleUnknownRelation:
if (name == sym_no_ops_)   return handle_no_ops(args, arg_count, st);
if (name == sym_cons_ops_) return handle_cons_ops(args, arg_count, st);
```

This is consistent with how all symbol comparison works in this codebase — pointer identity after interning, never string comparison at runtime.

The `Evaluator` base class constructor signature is therefore:

```cpp
Evaluator(Arena* arena, const OutcomeSyms* syms)
```

Not `Evaluator(Arena*, Intern*, const OutcomeSyms*)` as the earlier section shows. The earlier section's constructor snippet should be read with this correction applied.

---

## Backtrack Integration: Calling restore()

When `step()` backtracks (restores a saved `State`), it must also restore
the client region. Add this call wherever a saved `State` is restored in
the `step()` implementation:

```cpp
// Wherever st is restored to a previously saved state:
st = saved_st;
client_region_.restore(st.client_offset);
```

In the `Disj` case, the two branches share the same `client_offset` from
the point of the disjunction. When one branch fails and the other is tried,
`client_offset` is restored to the pre-disjunction value automatically
(because `st` is restored), and then `client_region_.restore()` rewinds the
actual bytes.

---

## parse_run.cpp: Construct Evaluator Instance

`parse_run.cpp` currently calls `runN()` as a free function. Update it to
construct an `Evaluator` instance and call `runN()` as a method:

```cpp
// Before (Stage 0A):
runN(a, pq.n, pq.goal, pq.qvar, pq.vars_used, pq.outcome_syms, pq.rel_env,
    [&](Term ans, State) { ... });

// After (Stage 0B):
Evaluator eval(&a, &intern_instance, &pq.outcome_syms);
eval.runN(pq.n, pq.goal, pq.qvar, pq.vars_used, pq.rel_env,
    [&](Term ans, State) { ... });
```

**Note on Intern:** `parse_query` currently constructs and owns the `Intern`
table inside `ParsedQuery`. The `Evaluator` needs access to the same intern
table to look up relation names in `handleUnknownRelation`. Pass a pointer
to `pq.intern` when constructing the evaluator.

**Note on OutcomeSyms:** Similarly, `pq.outcome_syms` is already populated
by `parse_query`. Pass a pointer to it.

The arena is reset after each program as before.

---

## RapEvaluator: Stub Implementation

Add to `rap/rap.hpp`. This replaces the existing `RapEngine` struct.

```cpp
// rap/rap.hpp
#pragma once
#include "../core/core.hpp"

class RapEvaluator : public Evaluator {
public:
    RapEvaluator(Arena* arena, Intern* intern, const OutcomeSyms* syms)
        : Evaluator(arena, intern, syms)
    {
        // Allocate the client region from the main arena.
        uint8_t* region_base = static_cast<uint8_t*>(
            arena->alloc(MAX_CHANGESET_ARENA, alignof(std::max_align_t)));
        client_region_.id       = ClientId::RAP;
        client_region_.base     = region_base;
        client_region_.capacity = MAX_CHANGESET_ARENA;
        client_region_.offset   = 0;
    }

protected:
    StepResult handleUnknownRelation(
        const SymEntry* name,
        const Term*     args,
        std::uint32_t   arg_count,
        State&          st) override;

private:
    StepResult handle_no_ops(const Term* args,
                             std::uint32_t arg_count, State& st);
    StepResult handle_cons_ops(const Term* args,
                               std::uint32_t arg_count, State& st);
};

inline StepResult RapEvaluator::handleUnknownRelation(
    const SymEntry* name, const Term* args,
    std::uint32_t arg_count, State& st)
{
    // Safety check: verify this is our client region.
    if (client_region_.id != ClientId::RAP)
        return StepResult::NoYield;  // wrong client — fail

    if (sym_lit_eq(name, "no-ops"))
        return handle_no_ops(args, arg_count, st);
    if (sym_lit_eq(name, "cons-ops"))
        return handle_cons_ops(args, arg_count, st);

    // Not recognized by RAP layer either.
    return StepResult::NoYield;
}

// STAGE 0B STUB — replace in Stage 2 with real ChangeSet construction
inline StepResult RapEvaluator::handle_no_ops(
    const Term* args, std::uint32_t arg_count, State& st)
{
    if (arg_count != 1) return StepResult::NoYield;

    // Allocate a marker byte to prove backtrack rewind works.
    uint8_t* marker = static_cast<uint8_t*>(client_region_.alloc(1));
    if (!marker) return StepResult::NoYield;  // region full
    *marker = 0x00;  // sentinel: empty ops list

    // Unify argument with the sentinel symbol 'empty-ops'.
    // Stage 2 will unify with an actual ChangeSet term.
    const SymEntry* empty_ops_sym = intern_cstr(*arena_, *intern_, "empty-ops");
    // Perform unification inline (or call into step machinery as appropriate
    // given the actual unification API after Stage 0A refactor).
    // [IMPL NOTE: use whatever unify() signature exists in core.hpp]
    (void)args; (void)st; (void)empty_ops_sym;
    return StepResult::Yield;  // stub: always succeed
}

// STAGE 0B STUB — replace in Stage 2 with real ChangeSet construction
inline StepResult RapEvaluator::handle_cons_ops(
    const Term* args, std::uint32_t arg_count, State& st)
{
    if (arg_count != 3) return StepResult::NoYield;

    // Allocate a marker byte to prove backtrack rewind works.
    uint8_t* marker = static_cast<uint8_t*>(client_region_.alloc(1));
    if (!marker) return StepResult::NoYield;  // region full
    *marker = 0x01;  // sentinel: cons op

    // Stage 2 will validate Op and build a real Op struct here.
    (void)args; (void)st;
    return StepResult::Yield;  // stub: always succeed
}
```

**Important notes on the stub implementations:**

- The stubs allocate a marker byte to prove the backtrack rewind mechanism
  works. This is the minimum needed.
- The stubs do not perform real unification — they just succeed. Stage 2
  replaces them with real ChangeSet construction logic.
- The stubs are clearly marked `// STAGE 0B STUB` so they are easy to find
  and replace.
- The unification in `handle_no_ops` is left as a note because the exact
  unification API may differ from what this document anticipates. Claude Code
  should use whatever `unify()` or equivalent exists in `core.hpp` after
  Stage 0A. If unification is complex to invoke from outside `step()`,
  the stub may simply return `StepResult::Yield` without unifying — this
  is acceptable for Stage 0B since the test suite only checks that the
  mechanism works, not that the relational semantics are complete.

---

## rap/test_rap.cpp: Update for RapEvaluator

The existing `rap/test_rap.cpp` tests `RapEngine`. Update it to use
`RapEvaluator` instead. The test should verify:

1. `RapEvaluator` constructs without error
2. A simple query runs correctly through `RapEvaluator::runN()`
3. `no-ops` succeeds (one solution) — `client_region_.offset > 0` after query
4. `cons-ops` with 3 args succeeds (one solution)
5. `cons-ops` with wrong arg count fails (no solutions)
6. A fully unknown relation fails (no solutions, not crash)

The existing "answer: ok / answer: fail / PASS: got 2 answer(s)" output
should be preserved or updated to match the new test structure.

---

## Makefile Updates

Add `core/mktypes.hpp` to any dependency lists that don't already have it
(should already be done from Stage 0A). No new files need to be added to
`all` — `rap/test_rap` already exists as a target. Verify it still builds.

---

## What Does NOT Change in core/

To preserve the separation between `core/` and `rap/`:

- `core/` must not include any `rap/` headers
- `core/core.hpp` must not reference `ClientId::RAP` by name
- `core/` must not interpret the bytes in `client_region_`
- The default `handleUnknownRelation` in `Evaluator` returns failure —
  it does not know about any RAP concepts
- `core/` must compile and work correctly with `ClientId::None` (no client)

---

## Acceptance Criteria for Stage 0B

Stage 0B is complete when:

- [ ] `ClientId` enum added to `core/core.hpp`
- [ ] `ClientRegion` struct added to `core/core.hpp`
- [ ] `client_offset` field added to `State`
- [ ] `MAX_CHANGESET_ARENA` and `MAX_CHANGESET_OPS` constants defined
- [ ] `Evaluator` class added to `core/core.hpp` with `runN()` as method
- [ ] `handleUnknownRelation` is a `protected virtual` method on `Evaluator`
- [ ] Default `handleUnknownRelation` returns `StepResult::NoYield`
- [ ] `step()` calls `handleUnknownRelation` at the fallthrough of call dispatch
- [ ] Backtrack machinery calls `client_region_.restore(st.client_offset)`
      wherever `State` is restored
- [ ] `State.client_offset` initialized correctly in `runN()` and `probe_run()`
- [ ] `RapEvaluator` added to `rap/rap.hpp` with `ClientId::RAP`
- [ ] `RapEvaluator` stubs for `no-ops` (1 arg) and `cons-ops` (3 args)
- [ ] Stubs clearly marked `// STAGE 0B STUB — replace in Stage 2`
- [ ] `parse_run.cpp` updated to construct `Evaluator` instance
- [ ] `rap/test_rap.cpp` updated for `RapEvaluator`
- [ ] `make` builds cleanly with `-Werror`
- [ ] All 6 existing programs in `parse_run` produce unchanged output
- [ ] Programs 7–12 produce unchanged output
- [ ] `security/security_test` all 10 cases still pass
- [ ] `rap/test_rap` passes
- [ ] `core/` contains no references to `ClientId::RAP` or RAP concepts
- [ ] This document updated if any design decisions changed

---

## What Stage 0C Builds on Top of This

Stage 0C adds the automated test suite. It will add:

- `core/test_extension.cpp` — tests the base `Evaluator` mechanism directly
- `rap/test_rap_extension.cpp` — tests `RapEvaluator` backtrack behavior

Stage 0C does not change any implementation — only adds tests. If Stage 0C
reveals bugs in Stage 0B, fix them in Stage 0B's files.

## What Stage 2 Builds on Top of This

Stage 2 replaces the `// STAGE 0B STUB` implementations in `RapEvaluator`
with real ChangeSet construction. It also adds Queue 2, the reactive
execution loop, and the output queue. Stage 2 does not require any changes
to `Evaluator` or the extension mechanism itself.

---

*This document is the authoritative specification for Stage 0B.*  
*If implementation diverges from this document, update it and note why.*

*v1.0 May 7, 2026 — initial specification*
