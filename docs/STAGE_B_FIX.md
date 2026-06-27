# STAGE_B_FIX: Agenda Entry IDs, Structural Rel-Only Invariant, and Introspection

**Version:** 2.0 (supersedes v1.0 — full revert + structural invariant, not additive)  
**Depends on:** STAGE_ARITH complete, STAGE_B complete (merged to `main`,
             commit `520b1f6`)  
**Modifies:** `rap/agenda.hpp`, `rap/changeset.hpp`, `rap/rap.hpp`,
             `rap/loop.hpp`, `core/core.hpp`, `stdlib/core.rap`,
             `rap/test_stage2.cpp`, `examples/todo.rap`, `examples/wc.rap`  
**Status:** Specification — not yet implemented  
**Date:** June 2026

---

## Why This Supersedes v1.0

A prior implementation session (merged to `main` as part of STAGE_B)
introduced a dual-mode design to make `todo.rap`/`wc.rap` work:
`Op::Remove`'s `query_id: uint32_t` field was replaced with a shared
`Term query_term` field (Int = remove-by-id, anything else =
remove-by-structural-match), and `Agenda` grew `terms_equal`,
`remove_by_term`, `has_rel`, and `dequeue_rel` to support non-`Rel`
data entries (like `(wc-state ...)`) coexisting on the agenda alongside
real pending queries.

This works, but it reintroduces a lifetime-management problem the
arena-based design was specifically built to avoid: a non-`Rel` agenda
entry is data with no query ever responsible for deciding whether it
should still exist. Nothing forces any program to keep checking
whether such an entry is still needed; if a program's logic has a gap,
the entry becomes permanent, unowned garbage sitting on the agenda
forever — the same category of problem (something stale persists
because nothing remembered to clean it up) that arenas eliminate
everywhere else in this system, reintroduced at the one layer that was
supposed to be exempt from it.

**This version reverts the dual-mode design entirely and replaces it
with a structural invariant: every agenda entry is a `Rel` term, with
no exceptions, enforced by `Agenda::enqueue()` itself.** Persistent
state (like `wc.rap`'s running counts) is carried as closed-over
arguments inside a self-perpetuating query — a query that, each time
it runs, decides whether to continue and if so emits `(add ...)` with
its own updated state captured in a freshly constructed `Rel`. State
survives exactly as long as something currently re-proposes it, with
no separate sweep, no separate non-`Rel` carve-out, and no "is this
garbage" question that exists outside the normal query lifecycle.

Because every agenda entry is now a `Rel`, two new introspection
builtins are required so that one query can still examine what another
pending entry is carrying: `boundo` (is this specific term currently
bound or still a free variable — engine primitive) and `rel-argso`
(what are this `Rel`'s captured argument terms — engine primitive,
needs direct `RelNode` access). `groundo` (recursive ground-check) is
built from `boundo` as an ordinary `stdlib/core.rap` relation, not a
new builtin.

---

## Part 1: Full Revert of the Dual-Mode Remove Design

### rap/changeset.hpp

Revert `Op` to the original id-only design:

```cpp
enum class OpTag : std::uint8_t {
    Add    = 0,  // add(QueryTerm)   — enqueue a query (must be a Rel term)
    Remove = 1,  // remove(QueryId)  — remove pending query by ID
    Output = 2,  // output(Term)     — append to output queue
};

struct Op {
    OpTag tag;
    union {
        Term          query_term;   // Add: the Rel term to enqueue
        std::uint32_t query_id;     // Remove: stable integer ID
        Term          output_term;  // Output
    };
};
static_assert(std::is_trivially_destructible_v<Op>);
```

### rap/agenda.hpp

Remove `terms_equal`, `remove_by_term`, `has_rel`, and `dequeue_rel`
entirely. `QueryEntry`, `Agenda::dequeue()`, and `Agenda::remove(id)`
are unchanged from their original Stage 2 form — no edits needed
beyond removing the four added methods.

### rap/rap.hpp — handle_cons_ops

Revert the `remove` case to Int-only:

```cpp
} else if (sym_lit_eq(op_head.sym, "remove")) {
    if (op_arg.tag != TermTag::Int) return StepResult::NoYield;
    op.tag      = OpTag::Remove;
    op.query_id = static_cast<std::uint32_t>(op_arg.value);
    push_this_op = true;
}
```

### rap/loop.hpp

Revert `run_until_empty` and `run_one` to use plain `dequeue()` /
`agenda.empty()`:

```cpp
void run_until_empty(std::uint32_t max_steps = 10000) {
    for (std::uint32_t s = 0; s < max_steps && !agenda.empty(); ++s)
        run_one();
}

void run_one() {
    if (!evaluator) return;
    QueryEntry entry;
    if (!agenda.dequeue(entry)) return;
    // ... unchanged below this point ...
}
```

Revert `apply_changeset`'s `Remove` case to the simple int path:

```cpp
case OpTag::Remove:
    agenda.remove(op.query_id);
    break;
```

---

## Part 2: Structural Rel-Only Invariant

### Agenda::enqueue() rejects non-Rel terms

```cpp
// Enqueue a query term. The term MUST be a Rel — this is enforced here,
// not merely expected by convention. Returns assigned query_id on success,
// or 0 on failure (OOM or non-Rel term — both are "could not enqueue").
std::uint32_t enqueue(Term query_term) {
    if (query_term.tag != TermTag::Rel) return 0;
    // ... rest of existing enqueue logic unchanged ...
}
```

**This makes "every agenda entry is a Rel" a property the agenda
itself guarantees, not a property callers are merely expected to
respect.** A `ChangeSet`'s `Op::Add` carrying a non-`Rel` term will
simply fail to enqueue — `apply_changeset`'s `OpTag::Add` case should
treat a 0 return from `enqueue` the same as any other silent failure
(no special handling needed; this mirrors how OOM from `enqueue` is
already handled — silently, since `ChangeSet` validity was never meant
to guarantee runtime resource availability, only operation-tag
validity).

**Verify before implementing:** confirm no existing code path
(`strengthen-agendao`, the original Stage 2 design, or any other
example) ever relied on enqueueing a non-`Rel` term. The original
Stage 2 design and `strengthen-agendao` always enqueued `Rel`s only,
so this restriction is expected to be a no-op in practice for
everything except `wc.rap`, which this spec rewrites anyway (Part 5).

---

## Part 3: as_term() Shape Change

`Agenda::as_term()` wraps each entry as `(id . content)`, where
`content` is now always specifically a `Rel` term:

```cpp
Term as_term(Arena& spine_arena) const {
    const QueryEntry* entries[MAX_CHANGESET_OPS];
    std::uint32_t n   = 0;
    std::uint32_t pos = tail;
    for (std::uint32_t i = 0; i < count && n < MAX_CHANGESET_OPS; ++i) {
        entries[n] = reinterpret_cast<const QueryEntry*>(buf + pos);
        pos += entries[n]->byte_size;
        ++n;
    }
    Term result = Term::nil();
    for (std::int32_t i = static_cast<std::int32_t>(n) - 1; i >= 0; --i) {
        PairNode* entry_pair = spine_arena.make<PairNode>();
        if (!entry_pair) break;
        entry_pair->car = Term::integer(static_cast<std::int32_t>(entries[i]->id));
        entry_pair->cdr = entries[i]->query_term;  // always a Rel, by Part 2

        PairNode* list_pair = spine_arena.make<PairNode>();
        if (!list_pair) break;
        list_pair->car = Term::make_pair(entry_pair);
        list_pair->cdr = result;
        result = Term::make_pair(list_pair);
    }
    return result;
}
```

---

## Part 4: New Introspection Builtins

### boundo — core builtin, non-recursive

`(boundo term)` succeeds if `term`, after walking through the current
substitution, is **not** an unbound `Var`. Fails if it is.

This is a one-step engine check, dispatched through
`handleKnownRelation` exactly like `leqo`/`charo`/the arithmetic
builtins added in STAGE_ARITH:

```cpp
// In core/core.hpp, alongside the other handle_* methods on Evaluator.
StepResult Evaluator::handle_boundo(
    const Term* args, std::uint32_t arg_count, State& st)
{
    if (arg_count != 1) return StepResult::NoYield;
    Term t = resolve_bvar(args[0], st.env);
    t = walk(t, st.subst, RelEnv{});
    if (t.tag == TermTag::Var) return StepResult::NoYield;  // still unbound
    return apply_k_or_yield(*arena_, /* ... */);  // succeeds, no new bindings
}
```

Add `sym_boundo_` to the interned built-in names and dispatch entry in
`handleKnownRelation`, following the exact pattern established for the
nine STAGE_ARITH builtins.

**Note:** `boundo` checks only the top-level term after walking — it
does not recurse into `Pair` substructure. A `Pair` whose `car` is
unbound is itself still considered "bound" by `boundo` (the `Pair`
value exists and is not itself a `Var`). Recursive ground-checking is
`groundo`'s job, built on top of `boundo`, below.

### rel-argso — core builtin

`(rel-argso rel-term args-list)` — `rel-term` must walk to a `TermTag::Rel`.
Yields `args-list` as the list of that `Rel`'s captured argument terms,
exactly as they were captured at the time the `Rel` was constructed
(via `(rel (params...) body)` closing over outer-scope values, per the
Stage 0A closed-relation design).

```cpp
StepResult Evaluator::handle_rel_argso(
    const Term* args, std::uint32_t arg_count, State& st)
{
    if (arg_count != 2) return StepResult::NoYield;
    Term rel_t = resolve_bvar(args[0], st.env);
    rel_t = walk(rel_t, st.subst, RelEnv{});
    if (rel_t.tag != TermTag::Rel || !rel_t.rel) return StepResult::NoYield;

    // [IMPL NOTE: The exact mechanism for retrieving a Rel's captured
    // argument terms depends on how Stage 0A's closed-relation capture
    // is represented internally — whether captured values are stored
    // as a side-table on the RelNode, or are only recoverable by
    // inspecting the compiled body's outer-scope Var references at the
    // time of construction. If captured arguments are not currently
    // retained in an inspectable form on RelNode, this builtin requires
    // adding a capture-list field to RelNode at construction time
    // (wherever (rel ...) terms are built, both in the parser and in
    // any place the engine constructs Rel terms programmatically, e.g.
    // raprunner's self-perpetuating query construction). Read the
    // current RelNode definition and Stage 0A's closure mechanism
    // before implementing; report back if RelNode needs a new field.]

    Term result = /* build list from rel_t.rel's captured arguments */;
    Term walked_out = resolve_bvar(args[1], st.env);
    walked_out = walk(walked_out, st.subst, RelEnv{});
    const Binding* s = st.subst;
    if (!unify(*arena_, walked_out, result, nullptr, s, RelEnv{}))
        return StepResult::NoYield;
    st.subst = s;
    return apply_k_or_yield(*arena_, /* ... */);
}
```

**This is the one part of this spec requiring investigation before
implementation, not just transcription.** Whether `RelNode` already
retains its captured arguments in a form `rel-argso` can read, or
whether this requires a new field populated at `Rel`-construction time,
must be determined by reading the current Stage 0A closure
implementation. Claude Code should report back on this rather than
guessing.

### groundo — stdlib relation, built from boundo

Add to `stdlib/core.rap`:

```scheme
; (groundo term) — term is fully determined: no Var anywhere in its
; structure, checked recursively. A Rel term is always considered
; ground at the surface — groundo does NOT recurse into a Rel's
; captured arguments. Use (rel-argso rel-term args) plus groundo on
; the resulting list elements if you need to check a Rel's captures.
(defrel (groundo term)
  (conj
    (boundo term)
    (disj
      (== term ())
      (fresh (h t)
        (conj (== term (h . t)) (groundo h) (groundo t)))
      (fresh (h t) (=/= term (h . t))))))
```

**[IMPL NOTE: get this correct and clean, not necessarily identical
to the sketch above. The intent: first reject outright-unbound terms
via boundo (fails fast). Then: if term is a Pair, recurse into car and
cdr, succeeding only if both are ground. If term is not a Pair (Int,
Sym, Nil, Rel — anything bound and non-Pair), succeed immediately —
there is no further structure to check. Write whatever disjunction or
case structure correctly and idiomatically expresses this in the
surface language; verify with test cases including nested pairs with
a buried unbound variable, which must fail.]**

---

## Part 5: Rewrite wc.rap as a Self-Perpetuating Query

`examples/wc.rap` currently keeps a bare `(wc-state chars words lines
in-word)` data entry on the agenda. Under the Part 2 invariant, this
is no longer permitted — rewrite it as a self-perpetuating `Rel` that
carries its own state as closed-over arguments.

```scheme
; examples/wc.rap — character, word, and line counter
; State (chars, words, lines, in-word) is carried as closed-over
; arguments of a self-perpetuating query, not as bare agenda data.

(defrel (count-charo c chars words lines in-word
                       chars1 words1 lines1 in-word1)
  (addo chars 1 chars1)
  (disj
    (conj
      (charo c 10)
      (addo lines 1 lines1)
      (== words1 words)
      (== in-word1 0))
    (conj
      (spacecharo c)
      (fresh (n) (conj (charo c n) (neqo n 10)))
      (== lines1 lines)
      (== words1 words)
      (== in-word1 0))
    (conj
      (alphacharo c)
      (== lines1 lines)
      (disj
        (conj (eqo in-word 0) (addo words 1 words1) (== in-word1 1))
        (conj (eqo in-word 1) (== words1 words)      (== in-word1 1))))))

(defrel (count-charo-listo input chars words lines in-word
                              chars1 words1 lines1 in-word1)
  (disj
    (conj
      (== input ())
      (== chars1 chars) (== words1 words)
      (== lines1 lines) (== in-word1 in-word))
    (fresh (c rest c2 w2 l2 iw2)
      (conj
        (== input (c . rest))
        (count-charo c chars words lines in-word c2 w2 l2 iw2)
        (count-charo-listo rest c2 w2 l2 iw2
                            chars1 words1 lines1 in-word1)))))

; The self-perpetuating counter. agenda is the standard first parameter;
; chars/words/lines/in-word are closed-over state carried between
; iterations. input is supplied however the integration in handle_input
; resolves (see IMPL NOTE below) — fixed here as a parameter for now.
(defrel (wc-counter agenda chars words lines in-word input ops)
  (disj
    ; EOF: report final counts, do not re-enqueue (stop the loop).
    (conj
      (== input ())
      (call cons-ops
        (output (chars chars words words lines lines))
        (no-ops) ops))
    ; Data: update state, re-enqueue self with updated state captured.
    (fresh (c2 w2 l2 iw2 ops0)
      (conj
        (=/= input ())
        (count-charo-listo input chars words lines in-word c2 w2 l2 iw2)
        (call cons-ops
          (add (rel (next-agenda next-input next-ops)
                 (call wc-counter next-agenda c2 w2 l2 iw2
                       next-input next-ops)))
          (no-ops) ops0)
        (== ops ops0)))))

(defrel (main args ops)
  (call cons-ops
    (add (rel (agenda input ops2)
           (call wc-counter agenda 0 0 0 0 input ops2)))
    (no-ops) ops))

(defrel (handle_input agenda fd input ops)
  ; [IMPL NOTE: this is the trickiest integration point. handle_input's
  ; conventional signature is (agenda fd input ops) — but the
  ; self-perpetuating wc-counter query, once enqueued, is itself the
  ; thing that should receive the next block of input, not a generic
  ; handle_input dispatch. Read the current raprunner.cpp input-dispatch
  ; mechanism (how it currently decides what query receives newly
  ; arrived input — does it always enqueue a fresh handle_input call,
  ; or is there already a notion of "the" current input-consumer
  ; query?) and propose a resolution. Two candidate approaches if
  ; raprunner has no existing notion of a designated consumer:
  ; (a) wc-counter's re-added next-iteration Rel could itself BE what
  ;     raprunner enqueues input wrappers against, if raprunner's
  ;     convention is "enqueue (call SOME-rel agenda fd input ops)
  ;     against whichever Rel is named handle_input in rel_env" — in
  ;     which case wc.rap may not need a separate handle_input at all,
  ;     and wc-counter's own re-added form should simply be registered
  ;     as the program's handle_input convention for this iteration;
  ; (b) handle_input stays a fixed dispatch point and is responsible
  ;     for locating the live wc-counter entry (by id, via something
  ;     that does NOT require structural content matching — e.g. main
  ;     could track a well-known fixed id is not available since ids
  ;     are runtime-assigned, so this likely means handle_input cannot
  ;     locate it without rel-argso-based introspection, which is
  ;     considerably more machinery for one example).
  ; Approach (a) is almost certainly cleaner if raprunner's actual
  ; mechanism permits it — investigate before implementing (b).
  ; Report the chosen approach and why before finalizing this example.
  (call no-ops ops))
```

**This file is intentionally left with an unresolved integration
question rather than a guessed answer.** The core lesson of this spec
— state lives inside self-perpetuating queries, not as bare agenda
data — is settled. How raprunner's per-block input-arrival mechanism
composes with a long-lived, self-perpetuating consumer of that input
is a real open design question. Claude Code should read
`raprunner.cpp`'s current input-handling loop, propose a resolution
(favoring Approach (a) above if feasible), confirm it, and only then
treat `wc.rap` as done.

---

## Part 6: Update todo.rap and strengthen-agendao

### strengthen-agendao (rap/test_stage2.cpp)

With ids now exposed structurally by `as_term()`, the hand-rolled
`(q qid (check H T))` id field is redundant. Simplify the agenda
content to drop the hand-placed id — `as_term()` supplies it
structurally as the entry's `car`. Since all agenda entries are now
`Rel` terms (Part 2), the test's agenda entries must each be a trivial
carrier `Rel` wrapping the check content:

```scheme
; Each test entry becomes, e.g.:
(rel (a) (== a (check hypA test1)))     ; id 10, assigned by enqueue
(rel (a) (== a (check+ hypA test1 refineX)))  ; id 11
```

Update `weak-check-qido`, `not-weak-check-qido`, and
`collect-weak-qidso` to destructure the `(id . rel-term)` shape via
`rel-argso`:

```scheme
(defrel (weak-check-qido entry H T qid)
  (fresh (rel-term args)
    (== entry (qid . rel-term))
    (rel-argso rel-term args)
    (== args ((check H T)))))

(defrel (not-weak-check-qido entry H T)
  (fresh (qid rel-term args chk)
    (== entry (qid . rel-term))
    (rel-argso rel-term args)
    (== args (chk))
    (=/= chk (check H T))))

(defrel (collect-weak-qidso agenda H T qids)
  (disj
    (conj (== agenda ()) (== qids ()))
    (fresh (entry rest qid tail)
      (conj
        (== agenda (entry . rest))
        (disj
          (conj
            (weak-check-qido entry H T qid)
            (collect-weak-qidso rest H T tail)
            (== qids (qid . tail)))
          (conj
            (not-weak-check-qido entry H T)
            (collect-weak-qidso rest H T qids)))))))
```

`strengthen-agendao` uses `find-by-contento` (Part 7) in place of the
hand-rolled `membero` lookup over `(q strong-qid (check+ H T R))`.

**Expected results unchanged:** Remove(10), Remove(12),
Output((pruned hypA test1)), entries 11 and 13 remain after applying
the ChangeSet. Only the mechanism for obtaining ids and content has
changed — structurally supplied and `rel-argso`-introspected, rather
than hand-rolled and directly destructured.

### todo.rap

Each `(todo item)` entry becomes a trivial carrier `Rel`:

```scheme
(defrel (carriero value carrier)
  (== carrier (rel (a) (== a value))))
```

`add` constructs `(carriero (todo arg) carrier)` and adds `carrier`.
`done` uses `find-by-contento` (Part 7) to locate the matching entry's
id, then `(remove id)`. `collect-todoso` (for `list`) destructures via
`rel-argso` the same way `weak-check-qido` does above.

---

## Part 7: find-by-contento (stdlib)

```scheme
(defrel (caro p h) (fresh (t) (== p (h . t))))
(defrel (cdro p t) (fresh (h) (== p (h . t))))
(defrel (conso h t p) (== p (h . t)))

(defrel (carriero value carrier)
  (== carrier (rel (a) (== a value))))

; find-by-contento walks (id . rel-term) pairs, extracts each Rel's
; captured arguments via rel-argso, and unifies the single-element
; argument list against pattern.
(defrel (find-by-contento agenda pattern id)
  (fresh (entry rest entry-id entry-rel entry-args)
    (== agenda (entry . rest))
    (disj
      (conj
        (== entry (entry-id . entry-rel))
        (rel-argso entry-rel entry-args)
        (== entry-args (pattern))
        (== id entry-id))
      (find-by-contento rest pattern id))))
```

---

## Acceptance Criteria

- [ ] `Op`/`OpTag` reverted to id-only `Remove`
- [ ] `terms_equal`, `remove_by_term`, `has_rel`, `dequeue_rel` removed
- [ ] `loop.hpp` reverted to plain `dequeue()`/`empty()`/int-only Remove
- [ ] `Agenda::enqueue()` rejects non-`Rel` terms structurally
- [ ] `as_term()` wraps entries as `(id . rel-term)`
- [ ] `boundo` added as core builtin via `handleKnownRelation`
- [ ] `rel-argso` added as core builtin — investigation into `RelNode`
      capture representation completed and reported
- [ ] `groundo` added to `stdlib/core.rap`, built from `boundo`,
      tested against a nested-pair-with-buried-unbound-variable case
- [ ] `caro`, `cdro`, `conso`, `carriero`, `find-by-contento` added
- [ ] `wc.rap` rewritten as self-perpetuating query; raprunner
      input-integration question investigated and resolved per the
      IMPL NOTE in Part 5, not guessed
- [ ] `todo.rap` updated to wrap content in trivial carrier `Rel`s via
      `carriero`
- [ ] `strengthen-agendao` updated per Part 6 (structural id,
      `rel-argso`-based destructuring, `find-by-contento` in place of
      hand-rolled `membero`)
- [ ] `make test` passes, including `test_stage2` with identical
      results: Remove(10), Remove(12), Output((pruned hypA test1)),
      entries 11 and 13 remaining
- [ ] Manual test: `todo.rap` add/done/list cycle works correctly
- [ ] Manual test: `wc.rap` produces correct counts with the new
      self-perpetuating structure
- [ ] No example or stdlib relation enqueues a non-`Rel` term anywhere

---

## What This Is Not

- Not an additive change — this fully replaces the dual-mode design
  merged in STAGE_B, not a layer on top of it
- No change to `Probe`, core unification, or anything outside the
  agenda/Rel-introspection surface described above
- `groundo` does not recurse into `Rel` closures — stated explicitly,
  not left ambiguous
- No new mechanism for a query to discover its own id — remains
  impossible by construction

---

*v1.0 June 2026 — initial specification (additive design, superseded)*  
*v2.0 June 2026 — full revert, structural Rel-only invariant,
boundo/rel-argso/groundo introspection*

