# Stage 0A Specification: Anonymous Relations and Call Dispatch

**Version:** 2.0  
**Depends on:** Existing `core/` (arena, intern, core.hpp, sexp_parser.hpp)  
**Required before:** Stage 0B (Evaluator class), Stage 0C (test suite)  
**Status:** Specification — not yet implemented

### Changelog from v1.0
- Added `mktypes.hpp` as prerequisite first step
- Replaced parse-time RelEnv resolution with runtime resolution in `step()`
- Added inline relation cache on `SymEntry` for top-level named relations
- Corrected call dispatch chain: Rel term → cache → RelEnv → handleUnknownRelation
- Corrected `rel` body scoping: symbols compile to `Term::symbol`, not errors,
  when not found in scope (unified symbol/Rel treatment)
- Removed "compile error on unknown name in rel body" — unknown names are
  symbols at compile time, resolved at runtime through dispatch chain
- `RelEnv` is now runtime-only, not consulted during parsing of `call`
- `ParsedQuery.rel_env` is populated at parse time but used at runtime

---

## Purpose

This document specifies the addition of anonymous relations and call dispatch
to the core engine. When Stage 0A is complete:

- Anonymous relation terms can be created using `(rel (x y ...) body)`
- Relations can be called using `(call rel-or-name arg1 arg2 ...)`
- `defrel` is available as top-level syntactic sugar
- Mutual recursion is expressible via `fresh` + anonymous relations
- Definition-before-use applies at the top level
- All existing programs in `parse_run.cpp` continue to work unchanged

---

## Prerequisite: mktypes.hpp

**This must be done before any other change in Stage 0A.**

Create `core/mktypes.hpp` containing forward declarations for all types
across the codebase. This breaks circular dependencies and removes the
artificial constraints imposed by the original file split (which existed
only because the file was too large for an early code generation tool).

`core/` and `intern.hpp` do not need to be independent. `mktypes.hpp`
is the single inclusion point that gives every header access to forward
declarations without circular includes.

```cpp
// core/mktypes.hpp
#pragma once
#include <cstdint>

// Forward declarations for all types in the RAP codebase.
// Every header includes this first.

enum class TermTag    : std::uint8_t;
enum class GoalTag    : std::uint8_t;
enum class Outcome    : std::uint8_t;
enum class ClientId   : std::uint32_t;  // used in Stage 0B
enum class StepResult : std::uint8_t;

struct Arena;
struct SymEntry;
struct Intern;
struct Term;
struct PairNode;
struct RelNode;        // Stage 0A
struct Goal;
struct GoalCall;       // Stage 0A
struct State;
struct Binding;
struct EnvFrame;
struct WorkQueue;
struct Work;
struct Kont;
struct OutcomeSyms;
struct ClientRegion;   // Stage 0B
struct RelEnv;         // Stage 0A
struct RelEnvEntry;    // Stage 0A
struct GlobalBind;
struct BoundBind;
```

After creating `mktypes.hpp`:
- Add `#include "mktypes.hpp"` as the first include in `arena.hpp`,
  `intern.hpp`, `core.hpp`, and `sexp_parser.hpp`
- Add `rel_cache` field to `SymEntry` in `intern.hpp` (see below)
- Verify all six existing programs still build and run correctly

**Acceptance gate:** `make` builds cleanly and all six programs produce
unchanged output before any other Stage 0A changes are made.

---

## Inline Relation Cache on SymEntry

With `mktypes.hpp` in place, `SymEntry` in `intern.hpp` can reference
`RelNode` without a circular dependency. Add a cache field:

```cpp
struct SymEntry {
    std::uint32_t    hash;
    std::uint32_t    len;
    const char*      str;
    const SymEntry*  next;
    const RelNode*   rel_cache;  // ADD: null = not a known top-level relation
                                 // Set once by RelEnv::define() at defrel time.
                                 // Never set from fresh/== binding paths.
};
```

**Cache semantics:**
- `rel_cache` is set **once** by `RelEnv::define()` when a top-level
  `defrel` is processed. It is never updated after that.
- `rel_cache` is **never** set from runtime resolution of `fresh`
  variables or `==` bindings. Those paths resolve through the substitution,
  not the cache.
- At runtime in `step()`, when the walked relation term is a `Sym`,
  check `sym->rel_cache` first. If non-null, dispatch immediately —
  no `RelEnv` scan needed.
- Safe because top-level `defrel` is definition-before-use with no
  redefinition. A symbol in `RelEnv` stays bound to the same `Rel` forever.

**Why the cache is safe in the presence of `fresh` shadowing:**
If a `fresh` variable happens to have the same name as a top-level
relation, it shadows the top-level name within its scope. But the `fresh`
variable resolves through the substitution (via `BVar` → `Var` → walk),
producing a `Rel` term directly — the `Sym` cache path is never reached
for that variable. The cache is only consulted when the walked term
is literally a `Sym`, which only happens for unbound symbolic constants
and top-level relation names.

---

## New Term Type: RelTerm

Add a new term tag for anonymous relation values:

```cpp
// Add to TermTag enum in core/core.hpp
enum class TermTag : std::uint8_t {
    Var, BVar, Int, Sym, Nil, Pair,
    Rel   // ADD: anonymous relation value
};
```

A `RelNode` holds the parameter count and the compiled body goal:

```cpp
// Add to core/core.hpp
struct RelNode {
    std::uint32_t param_count;  // number of parameters
    const Goal*   body;         // compiled body (uses BVar for parameters)
};

static_assert(std::is_trivially_destructible_v<RelNode>);
```

Extend `Term` union and add static constructor:

```cpp
// In the Term union, add:
const RelNode* rel;   // Rel

// Static constructor:
static Term relation(const RelNode* r) {
    Term t;
    t.tag = TermTag::Rel;
    t.rel = r;
    return t;
}
```

`RelNode` is POD. `Term` remains trivially destructible.

---

## New Goal Type: Call

```cpp
// Add to GoalTag enum in core/core.hpp
enum class GoalTag : std::uint8_t {
    Eq, Disj, Conj, Fresh, Probe,
    Call   // ADD: invoke a relation term with arguments
};

// Add to core/core.hpp
struct GoalCall {
    Term          rel_term;    // Rel, Var, BVar, or Sym — all resolved at runtime
    const Term*   args;        // arena-allocated array of argument terms
    std::uint32_t arg_count;
};

static_assert(std::is_trivially_destructible_v<GoalCall>);

// In the Goal union, add:
GoalCall call;
```

---

## Global Relation Environment (Runtime)

`RelEnv` is populated by `defrel` during parsing and passed to `step()`
for runtime lookup. It is **not** consulted during compilation of `call`
expressions — all names compile to their scope-resolved term regardless
of whether they name a relation.

```cpp
struct RelEnvEntry {
    const SymEntry*    name;
    Term               rel_term;    // TermTag::Rel
    const RelEnvEntry* next;
};

static_assert(std::is_trivially_destructible_v<RelEnvEntry>);

struct RelEnv {
    const RelEnvEntry* head = nullptr;

    // Define a named relation. Also sets rel_cache on the SymEntry.
    void define(Arena& a, const SymEntry* name, Term rel_term) {
        RelEnvEntry* e = a.make<RelEnvEntry>();
        if (!e) return;  // OOM
        e->name     = name;
        e->rel_term = rel_term;
        e->next     = head;
        head        = e;
        // Set inline cache for O(1) runtime lookup.
        if (rel_term.tag == TermTag::Rel)
            const_cast<SymEntry*>(name)->rel_cache = rel_term.rel;
    }

    // Linear scan fallback (used only on cache miss).
    Term lookup(const SymEntry* name) const {
        for (auto* e = head; e; e = e->next)
            if (e->name == name) return e->rel_term;
        return Term::nil();
    }
};

static_assert(std::is_trivially_destructible_v<RelEnv>);
```

---

## Call Dispatch in step()

The complete dispatch chain for `GoalTag::Call`:

1. Walk the relation term (BVar → Var → substitution walk)
2. `Rel` term → dispatch directly (anonymous rel or fresh variable bound via ==)
3. `Sym` term → check `sym->rel_cache` (O(1), top-level defrel)
4. Cache miss → scan `RelEnv` (O(n) fallback)
5. RelEnv miss → call `handleUnknownRelation` (extension point)
6. Default `handleUnknownRelation` → `StepResult::NoYield` (failure)

```cpp
case GoalTag::Call: {
    // Step 1: Walk the relation term.
    Term rel_t = g->call.rel_term;
    rel_t = resolve_bvar(rel_t, st.env);
    rel_t = walk(rel_t, st.subst);

    // Step 2-5: Resolve to a RelNode through the dispatch chain.
    const RelNode* rel = nullptr;

    if (rel_t.tag == TermTag::Rel) {
        rel = rel_t.rel;

    } else if (rel_t.tag == TermTag::Sym && rel_t.sym) {
        // Check inline cache first (O(1)).
        if (rel_t.sym->rel_cache) {
            rel = rel_t.sym->rel_cache;
        } else {
            // Cache miss: scan RelEnv.
            Term found = rel_env.lookup(rel_t.sym);
            if (found.tag == TermTag::Rel) {
                rel = found.rel;
            } else {
                // Not in RelEnv: extension point for RAP layer etc.
                return handleUnknownRelation(
                    rel_t.sym, g->call.args, g->call.arg_count,
                    st, a, q, w, k, yielded);
            }
        }
    } else {
        // Not a Rel or Sym after walking: fail.
        return StepResult::NoYield;
    }

    // Step 3: Arity check.
    if (g->call.arg_count != rel->param_count)
        return StepResult::NoYield;

    // Step 4: Allocate fresh variables for parameters.
    std::uint32_t base = st.counter;
    st.counter += rel->param_count;

    // Step 5: Build fresh closed env for body (nullptr base = closed relation).
    const EnvFrame* body_env = nullptr;
    for (std::uint32_t i = 0; i < rel->param_count; ++i) {
        EnvFrame* ef = a.make<EnvFrame>();
        if (!ef) return StepResult::OOM;
        ef->var_id = base + i;
        ef->next   = body_env;
        body_env   = ef;
    }

    // Step 6: Unify fresh vars with caller's arguments.
    const Binding* s = st.subst;
    for (std::uint32_t i = 0; i < rel->param_count; ++i) {
        Term arg = resolve_bvar(g->call.args[i], st.env);
        arg = walk(arg, s);
        Term param = Term::var(base + i);
        if (!unify(a, param, arg, nullptr, s))
            return StepResult::NoYield;
    }

    // Step 7: Continue with body under fresh closed env.
    State st2{ s, st.diseqs, body_env, st.counter };
    w->g  = rel->body;
    w->st = st2;
    w->k  = k;
    q.push(w);
    return StepResult::NoYield;
}
```

**Threading `rel_env` through `step()`:**

**Walk behavior for Sym terms:**

`walk` currently follows `Var` chains only. For the dispatch chain and mutual
recursion via `fresh` to work correctly, `walk` must also resolve `Sym` terms
through `rel_env`. Add a final step to `walk`: after the standard variable
chain traversal, if the result is a `Sym` and `rel_env.lookup(sym)` returns a
`Rel` term, return that `Rel` term. This means a symbol that names a top-level
relation resolves to its `Rel` term anywhere `walk` is called, not only in
`step()`. This is consistent with the unified symbol treatment — symbols are
constants unless they name a relation, in which case they resolve to it.

The `walk` signature must therefore also take `const RelEnv& rel_env`. All
call sites that currently pass only `subst` to `walk` must be updated to also
pass `rel_env`.

Add `const RelEnv& rel_env` as a parameter to `step()`, `runN()`, and
`probe_run()`. Pass `pq.rel_env` at all call sites. For programs with no
`defrel`, the default-constructed empty `RelEnv` is passed — no behavior
change for existing programs.

**`handleUnknownRelation` in Stage 0A:**

Implement as a free function (virtual method comes in Stage 0B):

```cpp
inline StepResult handleUnknownRelation(
    const SymEntry* name,
    const Term* args, std::uint32_t arg_count,
    State& st, Arena& a, WorkQueue& q, Work* w,
    const Kont* k, State& yielded)
{
    // Default: unknown relation = failure.
    (void)name; (void)args; (void)arg_count;
    (void)st; (void)a; (void)q; (void)w; (void)k; (void)yielded;
    return StepResult::NoYield;
}
```

Stage 0B replaces this with a virtual method on the `Evaluator` class.

---

## Parser Extensions (sexp_parser.hpp)

### Unified symbol treatment (key design decision)

At compile time, **symbols are just symbols**. The compiler does NOT
consult `RelEnv` when compiling `call` expressions or `rel` bodies.
A symbol that might name a relation compiles to `Term::symbol(sym)`.
Resolution happens entirely at runtime through the dispatch chain.

This makes mutual recursion via `fresh` work without any special mechanism:

```scheme
(fresh (eveno oddo)
  (== eveno (rel (n) (disj (== n 0) (call oddo n))))
  ;; oddo compiles to Term::symbol(oddo_sym) inside the rel body
  ;; At runtime: walk finds the Rel term bound by == above
  (== oddo  (rel (n) (fresh (m) (conj (== n (s m)) (call eveno m)))))
  (call eveno q))
```

`oddo` inside the `rel (n)` body:
- Not in that `rel`'s fresh `BoundBind` (only `n` is there)
- Not in `GlobalBind` (only `q` is there)
- Compiles to `Term::symbol(oddo_sym)`
- At runtime: walk of `oddo_sym` in the substitution finds the `Rel` term

### Parsing `(rel (params...) body)`

1. Parse the parameter list
2. Build the body's `BoundBind` scope as follows:
   - Start from `nullptr` (not the current `benv` directly)
   - For each entry in the current `benv`, add a corresponding entry to
     the body's `genv` (not `benv`) mapping the name to a fresh `Var` ID
     allocated from the current `vars_used` counter
   - Then push each parameter name onto the body's fresh `BoundBind`
   This means outer `fresh` variable names are visible inside the `rel`
   body as `Var` references (not `BVar`), so at runtime they walk through
   the substitution and find whatever `Rel` term was bound via `==`.
   Parameters remain as `BVar` indices. This is not a full closure —
   the body cannot see the caller's substitution directly, only the
   names of variables introduced by enclosing `fresh` forms.
3. Push each parameter name onto the fresh scope
4. Compile `body` under this fresh scope:
   - Parameters → `BVar(k)`
   - Query variable `q` in `GlobalBind` → `Var(0)`
   - Everything else → `Term::symbol(sym)`
5. Allocate `RelNode{param_count, body}` in the arena
6. Return `Term::relation(rel_node)`

**Why this fixes mutual recursion via `fresh`:**
Inside `(rel (n) (call oddo n))`, `oddo` is in the outer `benv`. With
this change it compiles to `Var(k)` rather than `Term::symbol(oddo_sym)`.
At runtime, `walk(Var(k), subst)` finds the `Rel` term bound by
`(== oddo (rel ...))` through the normal substitution chain.

### Parsing `(call f arg1 arg2 ...)`

1. Parse `f` as a term in the current scope (same as any other term):
   - In `BoundBind` → `BVar`
   - In `GlobalBind` → `Var`
   - Otherwise → `Term::symbol(sym)`
2. Parse each argument as a term
3. Allocate `Term*` array for arguments
4. Produce `Goal{GoalTag::Call, GoalCall{rel_term, args, n}}`

No RelEnv lookup. No compile-time error for unknown names. All deferred
to runtime.

### Parsing `(defrel (name x y ...) body)`

Valid **only at the top level** of a program. Diagnostic error if found
inside a goal body.

```
1. Extract relation name and parameter list
2. Compile body as (rel (params...) body) → Term::relation(rel_node)
3. Call rel_env.define(arena, intern(name), rel_term)
   → sets rel_cache on SymEntry for O(1) runtime lookup
4. Produces no Goal — side-effecting definition only
```

### Multi-form program parsing

`parse_query` must handle programs of the form:

```scheme
(defrel ...)   ; zero or more
(defrel ...)
(run N (q) GOAL)
```

Extend `parse_query` to loop: parse s-expressions one at a time, process
`defrel` forms (extend `rel_env`), stop on `run` form (compile goal).
Anything else at the top level is a diagnostic error.

`ParsedQuery` gains `RelEnv rel_env`:

```cpp
struct ParsedQuery {
    int            n            = 0;
    Term           qvar         = Term::nil();
    std::uint32_t  vars_used    = 0;
    const Goal*    goal         = nullptr;
    Intern         intern;
    OutcomeSyms    outcome_syms;
    RelEnv         rel_env;     // ADD: populated by defrel, used at runtime
};
```

---

## Print Changes

`print_term` — add `TermTag::Rel` case:
```cpp
case TermTag::Rel:
    std::printf("#<rel/%u>", t.rel ? t.rel->param_count : 0);
    break;
```

`print_goal` — add `GoalTag::Call` case:
```cpp
case GoalTag::Call:
    std::printf("(call ");
    print_term(g->call.rel_term);
    for (std::uint32_t i = 0; i < g->call.arg_count; ++i) {
        std::printf(" ");
        print_term(g->call.args[i]);
    }
    std::printf(")");
    break;
```

---

## What Does NOT Change

- `GoalTag::Probe` and all probe machinery
- `GoalTag::Eq`, `Disj`, `Conj`, `Fresh` — compilation and evaluation
- `OutcomeSyms`
- `walk`, `resolve_bvar`, `ext_s`, `unify`
- The six programs in `parse_run.cpp` — must produce unchanged output

---

## New Validation Programs for parse_run.cpp

Add after the existing six programs.

**Program 7: Simple named relation**
```scheme
(defrel (same x y) (== x y))
(run 3 (q) (call same q foo))
```
Expected: `q = foo`

**Program 8: Recursive relation (appendo)**
```scheme
(defrel (appendo l r o)
  (disj
    (conj (== l ()) (== o r))
    (fresh (h t res)
      (conj (== l (h . t))
            (conj (== o (h . res))
                  (call appendo t r res))))))
(run 3 (q) (call appendo (1 2) (3 4) q))
```
Expected: `q = (1 2 3 4)`

**Program 9: Relation passed as argument**
```scheme
(defrel (apply-rel r x) (call r x))
(defrel (is-foo x) (== x foo))
(run 1 (q) (call apply-rel is-foo q))
```
Expected: `q = foo`

**Program 10: Mutual recursion via fresh**
```scheme
(run 3 (q)
  (fresh (eveno oddo)
    (== eveno (rel (n) (disj (== n 0)
                             (fresh (m) (conj (== n (s m))
                                             (call oddo m))))))
    (== oddo  (rel (n) (fresh (m) (conj (== n (s m))
                                        (call eveno m)))))
    (call eveno q)))
```
Expected: `0`, `(s s 0)`, `(s s s s 0)`

**Program 11: Run backward (relational property)**
```scheme
(defrel (appendo l r o)
  (disj
    (conj (== l ()) (== o r))
    (fresh (h t res)
      (conj (== l (h . t))
            (conj (== o (h . res))
                  (call appendo t r res))))))
(run 3 (q) (call appendo q (3 4) (1 2 3 4)))
```
Expected: `q = (1 2)`

**Program 12: Inline cache correctness**
```scheme
(defrel (is-bar x) (== x bar))
(run 1 (q) (call is-bar q))
```
Expected: `q = bar`

---

## Acceptance Criteria for Stage 0A

- [ ] `mktypes.hpp` created; all six existing programs build and run unchanged
- [ ] `SymEntry::rel_cache` field added (null-initialized for all new symbols)
- [ ] `TermTag::Rel`, `RelNode`, `GoalTag::Call`, `GoalCall` added
- [ ] `RelEnv` and `RelEnvEntry` added; `define()` sets `rel_cache`
- [ ] `step()`, `runN()`, `probe_run()` take `const RelEnv&` parameter
- [ ] Call dispatch follows: Rel → cache → RelEnv → handleUnknownRelation
- [ ] `handleUnknownRelation` free function returns `StepResult::NoYield`
- [ ] `(rel ...)` compiles with fresh `nullptr` benv (closed scope enforced)
- [ ] Symbols in `rel` bodies compile to `Term::symbol` (not errors)
- [ ] `(call ...)` compiles head as term — no parse-time RelEnv lookup
- [ ] `defrel` valid only at top level; diagnostic error if inside a goal
- [ ] `parse_query` handles multi-form programs (defrel* then run)
- [ ] `ParsedQuery` has `RelEnv rel_env` field
- [ ] `print_term` handles `TermTag::Rel`
- [ ] `print_goal` handles `GoalTag::Call`
- [ ] Programs 7–12 produce expected results
- [ ] `make` builds cleanly with `-Werror`
- [ ] This document updated if any design decisions changed

---

## What Stage 0B Builds on Top of This

Stage 0B introduces the `Evaluator` base class, moves `step()` and
`runN()` into it as methods, and replaces the `handleUnknownRelation`
free function with a virtual method. It also adds `ClientRegion` and
`client_offset` in `State` for the extension mechanism. The call dispatch
chain from Stage 0A is preserved exactly.

---

## Notes on Future Extensions (Do Not Implement Now)

**Partial application:** A future `TermTag::PartialRel` would be
unwrapped before the arity check in `step()`. No other dispatch changes.

**stdlib/ defrel:** `defrel` moves to `stdlib/` when that layer exists.

**RelEnv growth:** Replace linked list with hash table if needed.

---

*This document is the authoritative specification for Stage 0A.*  
*If implementation diverges from this document, update it and note why.*

*v1.0 April 2026 — initial specification*  
*v2.0 May 7 2026 — runtime resolution, mktypes.hpp, inline cache,*  
*unified symbol treatment, corrected dispatch chain*
