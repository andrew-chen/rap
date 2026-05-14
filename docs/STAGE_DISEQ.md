# Disequality Constraints (=/=) Specification

**Version:** 1.0  
**Depends on:** Stage 2 complete  
**Modifies:** `core/core.hpp`, `core/sexp_parser.hpp`,
             `rap/test_stage2.cpp`, `parse_run.cpp`  
**Does NOT modify:** `security/`, `core/test_extension.cpp`,
                     `rap/test_rap_extension.cpp`  
**Status:** Specification — not yet implemented  
**Date:** May 2026

---

## Purpose

This document specifies three coordinated changes:

1. Implement `=/=` (disequality constraints) as a proper goal primitive
2. Revert the VM queue from DFS (LIFO) back to BFS (FIFO)
3. Update `collect-weak-qidso` in `rap/test_stage2.cpp` to use a
   disequality guard that is correct under any search strategy

The motivation: `collect-weak-qidso` as written for Stage 2 had two
branches that were not mutually exclusive. The DFS change was a workaround
for this logic bug, not a design choice. The correct fix is to use
disequality to make the branches genuinely exclusive — then the relation
works correctly under BFS (standard miniKanren behavior) and the DFS
workaround can be removed.

---

## Current State (from core.hpp)

**`Diseq` struct** — already defined at line 160:
```cpp
struct Diseq { Term u; Term v; const Diseq* next; };
```

**`State.diseqs`** — already present at line 176:
```cpp
const Diseq* diseqs;
```

**`unify` signature** — line 410:
```cpp
inline bool unify(Arena& a, Term u0, Term v0, const EnvFrame* env,
                  const Binding*& s, const RelEnv& rel_env);
```
`s` is passed by reference and extended in place. To detect "no new
bindings added" (terms already equal): compare the `s` pointer before
and after — if same pointer, no new bindings.

**`GoalTag::Eq` case** — line 767:
```cpp
case GoalTag::Eq: {
    const Binding* s = st.subst;
    if (!unify(a, g->eq.u, g->eq.v, st.env, s, rel_env))
        return StepResult::NoYield;
    State st2{ s, st.diseqs, st.env, st.counter, st.client_offset };
    return apply_k_or_yield(a, q, st2, k, w, yielded);
}
```

**`GoalTag::Disj` case** — line 774 (currently DFS):
```cpp
case GoalTag::Disj: {
    // DFS: push g2 first, then g1 (g1 goes to top → explored first)
    Work* w2 = a.make<Work>();
    w2->g = g->bin.g2; w2->st = st; w2->k = k;
    q.push(w2);
    Work* w1 = w;
    w1->g = g->bin.g1; w1->st = st; w1->k = k;
    q.push(w1);
    return StepResult::NoYield;
}
```

---

## Change 1: Add GoalTag::Diseq to core/core.hpp

### 1a. Extend the GoalTag enum (line 50)

```cpp
// Before:
enum class GoalTag : std::uint8_t { Eq, Disj, Conj, Fresh, Probe, Call };

// After:
enum class GoalTag : std::uint8_t { Eq, Disj, Conj, Fresh, Probe, Call, Diseq };
```

### 1b. Add GoalDiseq struct (after GoalEq, line 52)

```cpp
struct GoalDiseq { Term u; Term v; };
static_assert(std::is_trivially_destructible_v<GoalDiseq>);
```

### 1c. Add to Goal union (after `call` field)

```cpp
GoalDiseq diseq;  // Diseq (=/=)
```

### 1d. Add make_diseq helper (after make_eq)

```cpp
inline const Goal* make_diseq(Arena& a, Term u, Term v) {
    Goal* g = a.make<Goal>();
    if (!g) return nullptr;
    g->tag   = GoalTag::Diseq;
    g->diseq = GoalDiseq{u, v};
    return g;
}
```

### 1e. Add check_diseqs helper (before step())

This function checks whether any active disequality constraint is violated
by the current substitution `s`. Returns true if all constraints hold,
false if any is violated.

```cpp
// Check all active disequality constraints under substitution s.
// A constraint (u ≠ v) is violated iff unify(u, v, s) succeeds
// without adding any new bindings (u and v are already equal).
// Returns true if all constraints still hold; false if any is violated.
inline bool check_diseqs(Arena& a, const Diseq* diseqs,
                          const Binding* s, const RelEnv& rel_env) {
    for (const Diseq* d = diseqs; d; d = d->next) {
        const Binding* s2 = s;
        // Attempt unification of the constraint terms under s.
        // env=nullptr: constraint terms are already walked/resolved.
        bool ok = unify(a, d->u, d->v, nullptr, s2, rel_env);
        if (ok && s2 == s) {
            // Unification succeeded with no new bindings:
            // u and v are already equal — constraint violated.
            return false;
        }
        // Either unification failed (u,v can never be equal — fine)
        // or it required new bindings (constraint not yet violated — fine).
    }
    return true;
}
```

**Note on Arena use in check_diseqs:** `unify` allocates `UnifyJob` nodes
and `Binding` nodes in the arena. The `Binding` allocations for `s2` are
wasted (we discard `s2`), but since the arena is reset between queries
this is acceptable at paper scale. If this becomes a problem, a scratch
arena can be used; for now, use the main arena.

### 1f. Add GoalTag::Diseq case to step() — insert before `default:`

```cpp
case GoalTag::Diseq: {
    // Walk both sides under current substitution and env.
    Term u = resolve_bvar(g->diseq.u, st.env);
    Term v = resolve_bvar(g->diseq.v, st.env);
    u = walk(u, st.subst, rel_env);
    v = walk(v, st.subst, rel_env);

    // Attempt unification to see if u and v are already equal
    // or could be forced equal.
    const Binding* s2 = st.subst;
    bool ok = unify(a, u, v, nullptr, s2, rel_env);

    if (!ok) {
        // u and v can never be equal — constraint trivially satisfied.
        // No need to record it; just continue.
        return apply_k_or_yield(a, q, st, k, w, yielded);
    }

    if (s2 == st.subst) {
        // Unification succeeded with no new bindings:
        // u and v are already equal — disequality violated immediately.
        return StepResult::NoYield;
    }

    // Unification would require new bindings — constraint not yet violated.
    // Record it: if those bindings are ever added, the constraint fires.
    Diseq* d = a.make<Diseq>();
    if (!d) return StepResult::OOM;
    d->u    = u;     // store walked terms for efficient re-checking
    d->v    = v;
    d->next = st.diseqs;

    State st2{ st.subst, d, st.env, st.counter, st.client_offset };
    return apply_k_or_yield(a, q, st2, k, w, yielded);
}
```

### 1g. Update GoalTag::Eq to check disequality constraints

After the successful `unify` call and before building `st2`, add a
disequality check. Replace the current `GoalTag::Eq` case:

```cpp
case GoalTag::Eq: {
    const Binding* s = st.subst;
    if (!unify(a, g->eq.u, g->eq.v, st.env, s, rel_env))
        return StepResult::NoYield;
    // Check that no active disequality constraint is violated
    // by the new bindings just added to s.
    if (!check_diseqs(a, st.diseqs, s, rel_env))
        return StepResult::NoYield;
    State st2{ s, st.diseqs, st.env, st.counter, st.client_offset };
    return apply_k_or_yield(a, q, st2, k, w, yielded);
}
```

---

## Change 2: Revert Disj to BFS (FIFO) in step()

Replace the current DFS `GoalTag::Disj` case with FIFO ordering.
Under FIFO (BFS), g1 should be explored before g2. Since `WorkQueue::push`
adds to the back and `pop` takes from the front (FIFO), push g1 first
so it is dequeued first:

```cpp
case GoalTag::Disj: {
    // BFS (FIFO): push g1 first so it is explored first.
    Work* w2 = a.make<Work>();
    if (!w2) return StepResult::OOM;
    w2->g = g->bin.g2; w2->st = st; w2->k = k;

    Work* w1 = w;
    w1->g = g->bin.g1; w1->st = st; w1->k = k;

    // Push g1 first (explored first under FIFO), then g2.
    q.push(w1);
    q.push(w2);
    return StepResult::NoYield;
}
```

**[IMPL NOTE: Verify that `WorkQueue::push` adds to the back and `pop`
takes from the front. If the queue is implemented differently, adjust
push order accordingly so g1 is dequeued before g2.]**

After this change, run `make test` immediately. The existing tests should
still pass. `test_stage2` will likely fail until Change 3 is done — that
is expected.

---

## Change 3: Add =/= to sexp_parser.hpp

### 3a. Add `=/=` case to compile_goal (immediately after `==` case)

```cpp
if (sym_lit_eq(op, "=/=")) {
    if (!c || !c->cdr || c->cdr->cdr) {
        std::printf("[compile_goal] ERROR: '=/=' requires exactly 2 args: ");
        print_sexp(x);
        std::printf("\n");
        return nullptr;
    }
    Term u = compile_term(a, genv, benv, c->car);
    Term v = compile_term(a, genv, benv, c->cdr->car);
    return make_diseq(a, u, v);
}
```

### 3b. Add GoalTag::Diseq to print_goal (after GoalTag::Call case)

```cpp
case GoalTag::Diseq:
    std::printf("(=/= ");
    print_term(g->diseq.u);
    std::printf(" ");
    print_term(g->diseq.v);
    std::printf(")");
    break;
```

---

## Change 4: Update collect-weak-qidso in rap/test_stage2.cpp

Add `not-weak-check-qido` and update `collect-weak-qidso` to use it.
This makes the two branches mutually exclusive under any search strategy.

### Why this is needed

Without the guard, both "match" and "skip" branches can succeed for the
same agenda item. Under BFS, the "skip" branch (shorter path to base case)
produces an empty result before the "match" branch finds items. The
disequality guard blocks the skip branch when a match is possible.

### The guarded relations

Replace the existing `collect-weak-qidso` definition in the defs string
in `rap/test_stage2.cpp`:

```scheme
; Guard: item does NOT match the weak-check pattern for (H, T)
(defrel (not-weak-check-qido item H T)
  (fresh (qid)
    (=/= item (list q qid (list check H T)))))

; Collect all weak check qids — now mutually exclusive branches
(defrel (collect-weak-qidso agenda H T qids)
  (disj
    (conj (== agenda ()) (== qids ()))
    (fresh (item rest qid tail)
      (conj
        (== agenda (cons item rest))
        (disj
          ; Match branch: item IS a weak check for (H, T)
          (conj
            (call weak-check-qido item H T qid)
            (call collect-weak-qidso rest H T tail)
            (== qids (cons qid tail)))
          ; Skip branch: item is NOT a weak check for (H, T)
          (conj
            (call not-weak-check-qido item H T)
            (call collect-weak-qidso rest H T qids)))))))
```

**Note on surface syntax:** The defs string uses the same `(list ...)` and
`(cons ...)` tagged-tuple convention already used in the test. The `=/=`
form is the new addition.

**Why this works under BFS:** When `item` matches `(q qid (check H T))`,
the disequality `(=/= item (list q qid (list check H T)))` is immediately
violated — `=/=` fails, blocking the skip branch. Only the match branch
can proceed. When `item` does not match, the match branch's `==` fails
and only the skip branch can proceed. The branches are genuinely exclusive.

---

## Change 5: Add Program 13 to parse_run.cpp

Add a validation program demonstrating disequality:

```cpp
const char* program13 =
    "(run 5 (q)"
    "  (fresh (x)"
    "    (=/= x foo)"
    "    (disj (== x foo) (== x bar) (== x baz))))";
```

Expected output: `bar` and `baz` — not `foo` (excluded by disequality).

Add to the `programs[]` array and confirm it produces the expected results.

---

## Acceptance Criteria

- [ ] `GoalTag::Diseq` added to enum
- [ ] `GoalDiseq` struct added and trivially destructible
- [ ] `make_diseq` helper added
- [ ] `check_diseqs` helper added (before `step()`)
- [ ] `GoalTag::Diseq` case implemented in `step()`
- [ ] `GoalTag::Eq` calls `check_diseqs` after successful unify
- [ ] `GoalTag::Disj` reverted to BFS/FIFO push order
- [ ] DFS comment removed from `GoalTag::Disj`
- [ ] `=/=` parsed in `sexp_parser.hpp` compile_goal
- [ ] `GoalTag::Diseq` added to `print_goal`
- [ ] `not-weak-check-qido` defined in test_stage2.cpp
- [ ] `collect-weak-qidso` updated with disequality guard
- [ ] Program 13 added to parse_run.cpp
- [ ] `make` builds cleanly with `-Werror`
- [ ] `make test` passes all existing tests
- [ ] `test_stage2` produces Remove(10), Remove(12), Output((pruned hypA test1)) under BFS
- [ ] Program 13 produces `bar` and `baz` but not `foo`
- [ ] `security/security_test` all 10 cases pass
- [ ] `parse_run` all programs produce expected output

---

## Paper Changes (for you to make manually — not for Claude Code)

After the code changes are verified:

**Section 2.1** — remove the sentence about DFS. Replace with:
"Our implementation follows the standard miniKanren BFS interleaving strategy."

**Section 4.3** — remove the entire "Search strategy" paragraph about DFS
being required for `strengthen-agendao`.

**Section 8.3** — replace the current text with something like:
"The VM queue uses breadth-first interleaving, consistent with standard
miniKanren, preserving the completeness guarantee. A depth-first variant
is straightforward (swap push order in the Disj case) and may be preferable
for performance-critical applications where only the first solution matters;
this is left as a future configuration option."

**Section 5.2** — update the `collect-weak-qidso` code listing to include
`not-weak-check-qido` and the guarded version.

---

*v1.0 May 2026 — initial specification based on actual core.hpp contents*

