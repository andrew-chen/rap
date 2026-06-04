# STAGE_ARITH: Arithmetic, Comparison, and Extended Constraint Store

**Version:** 2.0  
**Depends on:** Stage DISEQ complete  
**Modifies:** `core/mktypes.hpp`, `core/core.hpp`, `core/sexp_parser.hpp`,
             `rap/rap.hpp`, `parse_run.cpp`, `repl.cpp`, `raprunner.cpp`,
             `core/test_extension.cpp`  
**New file:** `core/test_arith.cpp`  
**Status:** Specification — not yet implemented  
**Date:** May 2026

---

## Overview

This spec makes five coordinated changes:

1. **Extended constraint store** — generalizes `Diseq{u, v}` to
   `Constraint{u, v, offset, rel}` where `rel` is drawn from a six-value
   enum. The existing `=/=` becomes the special case `{u, v, 0, Eq}`.

2. **Comparison relations** — `leqo`, `lto`, `geqo`, `gto`, `eqo`,
   `neqo` as core built-ins. With both arguments bound integers, check
   immediately. With one unbound, record a constraint. With both unbound,
   fail.

3. **`addsubo`** — `(addsubo a b c)` means `a + b = c`. With one
   unbound, compute it. With all bound, check. With two or more unbound,
   fail.

4. **`multaddiso`** — `(multaddiso a b c d)` means `a * b + c = d`.
   Key cases handled; others fail.

5. **`charo`** — `(charo c n)` relates a single-character symbol to its
   ASCII integer value. Bidirectional; enumerates printable ASCII when
   both are unbound.

All five are implemented as core built-ins dispatched from
`Evaluator::handleUnknownRelation`. The `Evaluator` base class gains an
`Intern*` member to support `charo`.

Integer semantics throughout: `TermTag::Int` with `int64_t` values,
C++ wraparound on overflow (no special handling).

---

## Part 1: Extended Constraint Store

### Replace Diseq with Constraint

In `core/mktypes.hpp`, replace `struct Diseq` with:

```cpp
enum class ConstraintRel : uint8_t { Eq, Ne, Lt, Le, Gt, Ge };
struct Constraint;
```

In `core/core.hpp`, replace the `Diseq` struct with:

```cpp
enum class ConstraintRel : uint8_t {
    Eq,   // fires when walk(u) == walk(v) + offset
    Ne,   // fires when walk(u) != walk(v) + offset
    Lt,   // fires when walk(u) <  walk(v) + offset
    Le,   // fires when walk(u) <= walk(v) + offset
    Gt,   // fires when walk(u) >  walk(v) + offset
    Ge,   // fires when walk(u) >= walk(v) + offset
};

struct Constraint {
    Term              u;
    Term              v;
    int64_t           offset;
    ConstraintRel     rel;
    const Constraint* next;
};

static_assert(std::is_trivially_destructible_v<Constraint>);
```

A constraint **fires** when its condition is true — firing means failure.
So `(=/= x y)` records `{x, y, 0, Eq}`: fail if `x == y + 0`. And
`(leqo x 5)` with `x` unbound records `{x, 5_term, 0, Gt}`: fail if
`x > 5`.

### Update State

```cpp
struct State {
    const Binding*    subst;
    const Constraint* constraints;  // was: const Diseq* diseqs
    const EnvFrame*   env;
    uint32_t          counter;
    uint32_t          client_offset;
};
```

Replace every `st.diseqs` with `st.constraints` throughout `core.hpp`.

### check_constraints

Replace `check_diseqs` with:

```cpp
inline bool check_constraints(Arena& a, const Constraint* cs,
                               const Binding* s,
                               const RelEnv& rel_env) {
    for (const Constraint* c = cs; c; c = c->next) {
        Term u = walk(c->u, s, rel_env);
        Term v = walk(c->v, s, rel_env);

        // If either side is still an unbound Var, defer — not yet checkable.
        if (u.tag == TermTag::Var || v.tag == TermTag::Var) continue;

        // Both sides are ground. For integer constraints, use numeric comparison.
        if (u.tag == TermTag::Int && v.tag == TermTag::Int) {
            int64_t uval = static_cast<int64_t>(u.value);
            int64_t vval = static_cast<int64_t>(v.value) + c->offset;
            bool fires = false;
            switch (c->rel) {
                case ConstraintRel::Eq: fires = (uval == vval); break;
                case ConstraintRel::Ne: fires = (uval != vval); break;
                case ConstraintRel::Lt: fires = (uval <  vval); break;
                case ConstraintRel::Le: fires = (uval <= vval); break;
                case ConstraintRel::Gt: fires = (uval >  vval); break;
                case ConstraintRel::Ge: fires = (uval >= vval); break;
            }
            if (fires) return false;
            continue;
        }

        // Both sides are ground non-integers. Only Eq is meaningful:
        // check structural equality the same way the original diseq did.
        if (c->rel == ConstraintRel::Eq) {
            const Binding* s2 = s;
            bool unified = unify(a, u, v, nullptr, s2, rel_env);
            if (unified && s2 == s) return false;  // already equal — fires
        }
        // Ne, Lt, Le, Gt, Ge on non-integer ground terms: skip (undefined).
    }
    return true;
}
```

Call `check_constraints` everywhere `check_diseqs` was called.

### Update GoalTag::Diseq

The `GoalTag::Diseq` case now creates a `Constraint` instead of a
`Diseq`. Replace:

```cpp
Diseq* d = a.make<Diseq>();
d->u = u; d->v = v; d->next = st.diseqs;
State st2{ st.subst, d, ... };
```

With:

```cpp
Constraint* c = a.make<Constraint>();
c->u = u; c->v = v; c->offset = 0; c->rel = ConstraintRel::Eq;
c->next = st.constraints;
State st2{ st.subst, c, st.env, st.counter, st.client_offset };
```

The rest of the `GoalTag::Diseq` logic is unchanged.

---

## Part 2: Evaluator Gets Intern*

`charo` needs to construct and intern single-character symbols at
runtime. Add `Intern*` to `Evaluator`:

```cpp
class Evaluator {
public:
    Evaluator(Arena* arena, Intern* intern, const OutcomeSyms* syms)
        : arena_(arena), intern_(intern), syms_(syms) { ... }
protected:
    Arena*             arena_;
    Intern*            intern_;   // ADD
    const OutcomeSyms* syms_;
    ClientRegion       client_region_;
};
```

Update all `Evaluator` construction sites to pass `&pq.intern` or
equivalent:
- `parse_run.cpp`
- `repl.cpp`
- `raprunner.cpp`
- `core/test_extension.cpp`

`RapEvaluator` already takes `Intern*` and passes it through — no change
needed there beyond ensuring it passes to the base class.

---

## Part 3: handleKnownRelation — Core Built-in Dispatch

The dispatch chain in `step()` currently calls `handleUnknownRelation`
as the final fallback. `handleUnknownRelation` is virtual — subclasses
override it completely and are not expected to call the base
implementation.

Core built-ins must not go through `handleUnknownRelation` because a
subclass that overrides it without calling `super` (which is correct and
expected behavior) would silently lose `leqo`, `charo`, etc.

The solution is a new **non-virtual** method `handleKnownRelation`
called by `step()` before `handleUnknownRelation`. It cannot be
overridden. `handleUnknownRelation` stays exactly as it was.

### StepResult gains NotHandled

```cpp
enum class StepResult : uint8_t {
    Yield,
    NoYield,
    OOM,
    NotHandled,   // ADD: returned only by handleKnownRelation
};
```

`NotHandled` means "I didn't recognize this name." It is only ever
returned by `handleKnownRelation`. All other code continues to use
`NoYield` for failure. `handleUnknownRelation` never returns
`NotHandled`.

### Updated dispatch in step()

Replace the current single fallthrough call:

```cpp
// Before:
return handleUnknownRelation(sym, args, arg_count, st);

// After:
StepResult sr = handleKnownRelation(sym, args, arg_count, st);
if (sr != StepResult::NotHandled) return sr;
return handleUnknownRelation(sym, args, arg_count, st);
```

### handleKnownRelation

Non-virtual, defined once in `Evaluator`. Intern all nine built-in
names at `Evaluator` construction as private members:

```cpp
const SymEntry* sym_leqo_;
const SymEntry* sym_lto_;
const SymEntry* sym_geqo_;
const SymEntry* sym_gto_;
const SymEntry* sym_eqo_;
const SymEntry* sym_neqo_;
const SymEntry* sym_addsubo_;
const SymEntry* sym_multaddiso_;
const SymEntry* sym_charo_;
```

```cpp
StepResult Evaluator::handleKnownRelation(
    const SymEntry* name, const Term* args,
    uint32_t arg_count, State& st)
{
    if (name == sym_leqo_)       return handle_leqo(args, arg_count, st);
    if (name == sym_lto_)        return handle_lto(args, arg_count, st);
    if (name == sym_geqo_)       return handle_geqo(args, arg_count, st);
    if (name == sym_gto_)        return handle_gto(args, arg_count, st);
    if (name == sym_eqo_)        return handle_eqo(args, arg_count, st);
    if (name == sym_neqo_)       return handle_neqo(args, arg_count, st);
    if (name == sym_addsubo_)    return handle_addsubo(args, arg_count, st);
    if (name == sym_multaddiso_) return handle_multaddiso(args, arg_count, st);
    if (name == sym_charo_)      return handle_charo(args, arg_count, st);
    return StepResult::NotHandled;
}
```

### handleUnknownRelation stays unchanged

`Evaluator::handleUnknownRelation` remains "return NoYield."
`RapEvaluator::handleUnknownRelation` continues to override it
completely without calling the base — correct and expected, no change
needed.

---

## Part 4: Comparison Relations

All six share the same structure. Each is a private method on `Evaluator`.

### Helper: add_constraint

```cpp
StepResult Evaluator::add_constraint(
    Term u, Term v, int64_t offset, ConstraintRel rel, State& st,
    Arena& a, WorkQueue& q, const Kont* k, Work* w, State& yielded)
{
    Constraint* c = a.make<Constraint>();
    if (!c) return StepResult::OOM;
    c->u      = u;
    c->v      = v;
    c->offset = offset;
    c->rel    = rel;
    c->next   = st.constraints;
    State st2{ st.subst, c, st.env, st.counter, st.client_offset };
    return apply_k_or_yield(a, q, st2, k, w, yielded);
}
```

### leqo: (leqo a b) means a <= b

```
Both bound integers:  check a <= b. Succeed or fail.
a unbound, b bound:   record {a, b, 0, Gt}  (fire if a > b, i.e. a > b+0)
b unbound, a bound:   record {b, a, 0, Lt}  (fire if b < a, i.e. b+0 < a)
Both unbound:         fail.
Non-integers:         fail.
Arity != 2:           fail.
```

### lto: (lto a b) means a < b

```
Both bound integers:  check a < b.
a unbound, b bound:   record {a, b, 0, Ge}  (fire if a >= b)
b unbound, a bound:   record {b, a, 0, Le}  (fire if b <= a)
Both unbound:         fail.
```

### geqo: (geqo a b) means a >= b

```
Both bound integers:  check a >= b.
a unbound, b bound:   record {a, b, 0, Lt}  (fire if a < b)
b unbound, a bound:   record {b, a, 0, Gt}  (fire if b > a)
Both unbound:         fail.
```

### gto: (gto a b) means a > b

```
Both bound integers:  check a > b.
a unbound, b bound:   record {a, b, 0, Le}  (fire if a <= b)
b unbound, a bound:   record {b, a, 0, Ge}  (fire if b >= a)
Both unbound:         fail.
```

### eqo: (eqo a b) means a == b (numeric)

`eqo` with one unbound argument reduces to unification (`==`), because
if `a` must numerically equal `b`, binding `a` to `b`'s value is the
correct relational action.

```
Both bound integers:  check a == b.
One unbound:          unify the unbound with the bound value (same as ==).
Both unbound:         fail.
Non-integers:         fail (eqo is numeric equality only).
```

### neqo: (neqo a b) means a != b (numeric)

```
Both bound integers:  check a != b.
a unbound, b bound:   record {a, b, 0, Eq}  (fire if a == b) — same as =/=
b unbound, a bound:   record {b, a, 0, Eq}  (fire if b == a)
Both unbound:         fail.
Non-integers:         fail.
```

---

## Part 5: addsubo

`(addsubo a b c)` means `a + b = c`. Arity must be 3.

Walk all three arguments before dispatch.

```
All three bound integers:
    Check a + b == c. Succeed or fail.

c unbound, a and b bound integers:
    Compute result = a + b (int64_t wraparound).
    Unify c with Term::integer(result).

a unbound, b and c bound integers:
    Compute result = c - b (int64_t wraparound).
    Unify a with Term::integer(result).

b unbound, a and c bound integers:
    Compute result = c - a (int64_t wraparound).
    Unify b with Term::integer(result).

Two or more unbound:
    Fail.

Any argument is a non-integer ground term:
    Fail.
```

---

## Part 6: multaddiso

`(multaddiso a b c d)` means `a * b + c = d`. Arity must be 4.

Walk all four arguments before dispatch.

```
All four bound integers:
    Check a * b + c == d. Succeed or fail.

d unbound, a, b, c bound:
    Compute result = a * b + c (int64_t wraparound).
    Unify d with Term::integer(result).

c unbound, a, b, d bound:
    Compute result = d - a * b (int64_t wraparound).
    Unify c with Term::integer(result).

a unbound, b, c, d bound, b != 0:
    Check (d - c) is exactly divisible by b.
    If not: fail (no integer solution).
    Compute result = (d - c) / b.
    Unify a with Term::integer(result).

b unbound, a, c, d bound, a != 0:
    Check (d - c) is exactly divisible by a.
    If not: fail.
    Compute result = (d - c) / a.
    Unify b with Term::integer(result).

a unbound, b == 0, c and d bound:
    Check c == d (since a*0+c = c must equal d).
    If c == d: a is unconstrained — fail (a can be anything; infinite solutions).
    If c != d: fail (no solution).

b unbound, a == 0, c and d bound:
    Same as above with a and b swapped.

Division by zero (b == 0 when solving for a, or a == 0 when solving for b):
    Handled above — both cases result in fail.

Two or more of {a, b, c, d} unbound:
    Fail.

Any argument is a non-integer ground term:
    Fail.
```

---

## Part 7: charo

`(charo c n)` — `c` is a single-character symbol, `n` is its ASCII
integer value. Arity must be 2.

Walk both arguments before dispatch.

```
Both bound:
    c must be a Sym with str length exactly 1.
    n must be an Int.
    Check n == (int64_t)(unsigned char)c.str[0].
    Succeed or fail.

c bound, n unbound:
    c must be a Sym with str length exactly 1.
    Compute n_val = (int64_t)(unsigned char)c.str[0].
    Unify n with Term::integer(n_val).

n bound, c unbound:
    n must be an Int in range [1, 127].
    (0 is excluded: null byte cannot be interned as a symbol string.)
    (128–255 excluded for MVP: standard ASCII only.)
    If out of range: fail.
    Construct char buf[2] = { (char)(uint8_t)n.value, '\0' }.
    Intern: const SymEntry* s = intern_cstr(*arena_, *intern_, buf).
    Unify c with Term::symbol(s).

Both unbound:
    Enumerate n from 32 to 126 (printable ASCII) inclusive.
    For each n: construct the symbol and yield (c=symbol, n=integer).
    Use the standard BFS work-queue mechanism — push one work item
    per value, each unifying c and n with the corresponding values.
    (In practice: implement as a GoalTag::Call dispatch that fans out
    into 95 disjunctive branches, or push them onto the work queue
    directly in the handler.)
```

---

## Part 8: Test File

`core/test_arith.cpp` — add to `all` and `make test`.

```
Comparison:
  (leqo 3 5)                          => 1 solution
  (leqo 5 3)                          => 0 solutions
  (leqo 3 3)                          => 1 solution
  (lto 3 5)                           => 1 solution
  (lto 3 3)                           => 0 solutions
  (run 1 (q) (leqo q 5) (== q 3))    => q = 3
  (run 1 (q) (leqo q 5) (== q 6))    => 0 solutions (constraint fires)
  (run 1 (q) (leqo q 5) (== q 5))    => q = 5
  (geqo 5 3)                          => 1 solution
  (geqo 3 5)                          => 0 solutions
  (gto 5 3)                           => 1 solution
  (gto 3 3)                           => 0 solutions
  (eqo 3 3)                           => 1 solution
  (eqo 3 4)                           => 0 solutions
  (run 1 (q) (eqo q 7))              => q = 7
  (neqo 3 4)                          => 1 solution
  (neqo 3 3)                          => 0 solutions
  (run 1 (q) (neqo q 3) (== q 3))    => 0 solutions (constraint fires)
  (run 1 (q) (neqo q 3) (== q 4))    => q = 4

addsubo:
  (addsubo 3 4 7)                     => 1 solution
  (addsubo 3 4 8)                     => 0 solutions
  (run 1 (q) (addsubo 3 4 q))        => q = 7
  (run 1 (q) (addsubo 3 q 7))        => q = 4
  (run 1 (q) (addsubo q 4 7))        => q = 3
  (run 1 (q) (addsubo q 4 3))        => q = -1
  (run 1 (q) (addsubo q q q))        => 0 solutions (two unbound — fail)

multaddiso:
  (multaddiso 3 4 0 12)               => 1 solution
  (multaddiso 3 4 2 14)               => 1 solution
  (multaddiso 3 4 2 15)               => 0 solutions
  (run 1 (q) (multaddiso 3 4 0 q))   => q = 12
  (run 1 (q) (multaddiso 3 4 2 q))   => q = 14
  (run 1 (q) (multaddiso q 4 0 12))  => q = 3
  (run 1 (q) (multaddiso 3 q 0 12))  => q = 4
  (run 1 (q) (multaddiso 3 4 q 14))  => q = 2
  (run 1 (q) (multaddiso q 4 0 13))  => 0 solutions (13 not div by 4)
  (run 1 (q) (multaddiso q 0 3 3))   => 0 solutions (b=0, c==d so a unconstrained)
  (run 1 (q) (multaddiso q 0 3 4))   => 0 solutions (b=0, c!=d)
  (multaddiso 5 0 3 3)               => 1 solution (5*0+3=3)

charo:
  (charo a 97)                        => 1 solution
  (charo a 98)                        => 0 solutions
  (run 1 (q) (charo a q))            => q = 97
  (run 1 (q) (charo q 97))           => q = a (the symbol)
  (run 1 (q) (charo q 65))           => q = A (the symbol)
  (run 1 (q) (charo q 0))            => 0 solutions (out of range)
  (run 1 (q) (charo q 128))          => 0 solutions (out of range)
  (run 3 (q) (fresh (n) (charo q n))) => first 3 printable ASCII symbols

Constraint interaction:
  (run 1 (q) (=/= q foo) (== q foo)) => 0 solutions (existing =/= still works)
  (run 1 (q) (leqo q 5) (leqo 3 q)) => q can be 3, 4, or 5
                                        (run 3 should give 3 solutions
                                         if enumeration is attempted,
                                         but since both-unbound fails,
                                         this should give 0 solutions —
                                         document which behavior occurs)
```

---

## Acceptance Criteria

- [ ] `struct Diseq` removed; `struct Constraint` and `enum class ConstraintRel` added
- [ ] `State.diseqs` renamed to `State.constraints`
- [ ] `check_diseqs` replaced by `check_constraints`
- [ ] `GoalTag::Diseq` creates `Constraint{u, v, 0, ConstraintRel::Eq}`
- [ ] `StepResult::NotHandled` added to enum
- [ ] `handleKnownRelation` non-virtual method added to `Evaluator`
- [ ] `step()` calls `handleKnownRelation` before `handleUnknownRelation`
- [ ] `Evaluator` takes `Intern*`; all construction sites updated
- [ ] All nine built-in names interned at `Evaluator` construction
- [ ] `handleUnknownRelation` in `Evaluator` and `RapEvaluator` unchanged
- [ ] `leqo`, `lto`, `geqo`, `gto` implemented with constraint recording
- [ ] `eqo` with one unbound reduces to unification
- [ ] `neqo` with one unbound records an Eq constraint
- [ ] `addsubo` implemented: all-bound check, one-unbound compute, two-unbound fail
- [ ] `multaddiso` implemented: all cases as specified
- [ ] Division by zero in `multaddiso` fails cleanly (no UB)
- [ ] `charo` implemented: both-bound check, one-unbound compute, both-unbound enumerate 32–126
- [ ] `core/test_arith.cpp` added; all tests pass
- [ ] `make test` passes all existing tests
- [ ] `=/=` still works correctly via updated constraint store
- [ ] `docs/STAGE_ARITH.md` committed to repo `docs/` directory

---

## What This Is Not

- No `TermTag::ProperInt` — deferred
- No bitwise operations — deferred
- No multi-variable arithmetic constraints (addsubo with two unknowns fails)
- No stdlib `defrel` wrappers (`addo`, `subo`, etc.) — those are Spec B
- No arbitrary-precision integers — future work

---

*v1.0 May 2026 — initial specification (first draft, superseded)*  
*v2.0 May 2026 — clean rewrite, all decisions explicit*

