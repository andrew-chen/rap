# STAGE_B_AFTER_FIX_2: Explicit Published Args, Retiring Closure Introspection

**Version:** 1.0  
**Depends on:** STAGE_B_FIX Parts 1–3 (id-only `Remove`, structural
             `Rel`-only invariant, original `as_term()` change) and
             STAGE_B_AFTER_FIX (`_relo`/`raw-rel`/`rel-argso`,
             `RelNode` `captured_values`/`captured_count`,
             `wc-carriero`/`carriero` carrier pattern) as currently
             committed to `main`.  
**Supersedes:** STAGE_B_AFTER_FIX in its entirety — `_relo`, `raw-rel`,
             and `rel-argso`-as-closure-reader are retired, not
             extended. `RelNode.captured_values`/`captured_count` are
             removed.  
**Modifies:** `core/core.hpp`, `core/mktypes.hpp`, `rap/agenda.hpp`,
             `rap/changeset.hpp`, `rap/rap.hpp`, `rap/loop.hpp`,
             `raprunner.cpp`, `stdlib/core.rap`, `rap/test_stage2.cpp`,
             `examples/wc.rap`, `examples/todo.rap`  
**Status:** Specification — not yet implemented  
**Date:** June 2026

---

## Why This Supersedes STAGE_B_AFTER_FIX

STAGE_B_AFTER_FIX solved the closure-survives-past-its-query problem by
making closures introspectable from the outside: `_relo` snapshotted
caller-specified values into a `RelNode` field, and `rel-argso` read
that field back. This worked mechanically, but it is the wrong kind of
mechanism. A closure's captured values are not supposed to be a public
interface — `rel-argso` only worked by reaching into a Rel's private
internal representation. The practical symptom of this wrongness
showed up directly in both example rewrites: `wc.rap`'s `wc-carriero`
and `todo.rap`'s `carriero` both had to construct a `Rel` whose body is
permanently dead (`(rel (unused) (== unused unused))` in `wc.rap`) —
existing purely to be introspected, never to be called. A `Rel` term
on the agenda that is never meant to be called is exactly the
"non-query data sitting on the agenda" problem STAGE_B_FIX's structural
invariant was supposed to eliminate, recreated one layer down by a
different mechanism.

**The correct fix:** a query that wants other queries to be able to
reason about its state must **publish that state explicitly**, as
ordinary data, alongside itself on the agenda — not bury it in a
closure and provide a special tool to dig it back out. Anything a
query keeps purely in its own closed-over variables remains genuinely
private, exactly as closures should be. Anything externally relevant
is published as an explicit second term, called **`args`**, supplied
at enqueue time and handed back to the query itself when it next runs.

This is also forward-compatible with a planned future extension (a
different paper) involving "bound queries" tied to frame slots, where
`args` is expected to generalize naturally; `args` is named to read
naturally toward that future use, not as a one-off word for this spec.

**`rel-argso` as a closure-content-reader is retired entirely.** A
narrower, honest replacement — `rel-arityo`, reporting only a `Rel`'s
declared `param_count`, a fact that is already public regardless of
closures — is added in its place (Part 6), motivated by legitimate
need (e.g., a shell-like program checking whether a builtin was
invoked with the right number of arguments) without reintroducing
closure introspection.

---

## Part 0: Retirement Checklist

Remove entirely (do not deprecate, do not leave behind for backward
compatibility — these are recently added and have no external
consumers beyond the examples and tests this spec also rewrites):

- `_relo` builtin and its `handleKnownRelation` dispatch entry
- `raw-rel` from `stdlib/core.rap`
- `rel-argso` builtin (the closure-reading version) and its dispatch
  entry
- `RelNode.captured_values` / `RelNode.captured_count` fields
- Any RelNode-construction-site code (in `sexp_parser.hpp`,
  `raprunner.cpp`, `loop.hpp`) that initializes the two fields above —
  revert those construction sites to whatever they looked like before
  STAGE_B_AFTER_FIX added the fields
- `wc-carriero` from `examples/wc.rap` (full rewrite, Part 7)
- `carriero` from `stdlib/core.rap` and its use in `examples/todo.rap`
  (full rewrite, Part 8)
- `make_carrier` from `rap/test_stage2.cpp` (full rewrite, Part 9)

**Verify before removing:** confirm nothing outside the examples and
tests this spec rewrites depends on any of the above. Given how
recently these were added, this is expected to be a clean, total
removal with no orphaned dependents — confirm rather than assume.

---

## Part 1: Agenda Entry Shape — (id rel-term args)

Every agenda entry, as seen via `as_term()`, is now a proper 3-element
list: `(id rel-term args)`. `args` defaults to `Term::nil()` when a
query published nothing.

This is the **only** way any query ever learns anything about another
pending query. A query never knows its own `id` (it has already been
dequeued by the time it runs — unchanged from all prior specs in this
series). A query's own `agenda` and `args` are passed to it directly
as call arguments when it runs (Part 3) — it does not need to look
itself up in the agenda term to find these.

### QueryEntry gains an args field

```cpp
// rap/agenda.hpp
struct QueryEntry {
    std::uint32_t id;
    std::uint32_t byte_size;
    Term          query_term;  // must be a Rel (Part 2)
    Term          args;        // published state; Term::nil() if none
};
static_assert(std::is_trivially_destructible_v<QueryEntry>);
```

**[IMPL NOTE: `byte_size` accounting must now include the deep-copied
size of `args` as well as `query_term`, since both live in the same
agenda buffer slot. Read the current `enqueue`/`remove`/compaction
logic carefully — both terms need to be deep-copied into the same
sub-arena and their combined size used for `byte_size`, and the
pointer-rewriting compaction in `remove()` must copy both terms when
relocating a surviving entry, not just `query_term`. This is real,
careful work, not a one-line change — get the arena bookkeeping right
for both terms together.]**

### Agenda::enqueue takes a Rel and its args

```cpp
// Enqueue a query: rel_term must be a Rel with param_count exactly 1
// or 2. args is published alongside it (Term::nil() if none).
// Rejects (returns 0) if rel_term is not a Rel, or if its param_count
// is not 1 or 2 (Part 2).
std::uint32_t enqueue(Term rel_term, Term args) {
    if (rel_term.tag != TermTag::Rel || !rel_term.rel) return 0;
    std::uint32_t pc = rel_term.rel->param_count;
    if (pc != 1 && pc != 2) return 0;
    // ... existing logic, extended to deep-copy and store both
    // rel_term and args per the IMPL NOTE above ...
}
```

**[IMPL NOTE: decide whether `enqueue`'s existing single-Term-argument
callers (if any remain after Parts 7–9's rewrites) should get a
convenience overload `enqueue(Term rel_term)` that calls
`enqueue(rel_term, Term::nil())`, or whether all call sites should be
updated to pass both arguments explicitly. Prefer explicit at all call
sites if the number of call sites is small — this keeps the "args is a
real, present concept" visible rather than letting a one-argument
shortcut quietly become the default style.]**

### as_term() shape

```cpp
Term as_term(Arena& spine_arena) const {
    // ... collect entries as before ...
    Term result = Term::nil();
    for (/* back to front */) {
        // Build (id rel-term args) as a proper 3-element list:
        // (id . (rel-term . (args . ())))
        PairNode* p3 = spine_arena.make<PairNode>();
        if (!p3) break;
        p3->car = entries[i]->args;
        p3->cdr = Term::nil();

        PairNode* p2 = spine_arena.make<PairNode>();
        if (!p2) break;
        p2->car = entries[i]->query_term;
        p2->cdr = Term::make_pair(p3);

        PairNode* p1 = spine_arena.make<PairNode>();
        if (!p1) break;
        p1->car = Term::integer(static_cast<std::int32_t>(entries[i]->id));
        p1->cdr = Term::make_pair(p2);

        PairNode* list_pair = spine_arena.make<PairNode>();
        if (!list_pair) break;
        list_pair->car = Term::make_pair(p1);
        list_pair->cdr = result;
        result = Term::make_pair(list_pair);
    }
    return result;
}
```

A relational program destructures an entry with ordinary unification:
`(== entry (id rel-term args))` — no special introspection call
needed, ever, for this shape.

---

## Part 2: Enqueue-Time Validation

`Agenda::enqueue()` (Part 1) already rejects non-`Rel` terms and `Rel`
terms whose `param_count` is not 1 or 2. This is the complete
enforcement point — no other code path needs duplicate validation.

`Op::Add`'s handling in `rap/rap.hpp`'s `handle_cons_ops` should pass
through to `enqueue` and let it reject; `handle_cons_ops` does not need
its own separate param_count check.

---

## Part 3: Dequeue-Time Call Convention

When `run_one()` dequeues an entry and calls it, the number of
arguments passed depends on the `Rel`'s own `param_count`:

```cpp
// rap/loop.hpp — run_one()
const RelNode* rel = entry.query_term.rel;
std::uint32_t  nparams = rel->param_count;  // 1 or 2, guaranteed by enqueue

Term* call_args = /* allocate nparams Terms */;
call_args[0] = agenda_term;
if (nparams == 2) {
    call_args[1] = entry.args;
}
// nparams == 1: only agenda_term is passed, exactly as all examples
// before this spec already expect. No change in behavior for any
// 1-parameter Rel.
```

**This is the complete backward-compatibility story:** any `Rel`
written before this spec, taking only `agenda`, continues to work
completely unmodified — it simply never receives `args` (which will be
`Term::nil()` for such entries anyway, since nothing published
anything). A `Rel` that wants to both receive and publish state
declares 2 parameters and reads/writes `args` explicitly.

---

## Part 4: Op::Add Gains an args Field

```cpp
// rap/changeset.hpp
struct Op {
    OpTag tag;
    union {
        struct { Term rel_term; Term args; } add;  // Add
        std::uint32_t query_id;                      // Remove
        Term output_term;                             // Output
    };
};
```

**[IMPL NOTE: a union member with a non-trivial-looking nested struct
containing two `Term`s is fine as long as `Term` itself remains POD —
confirm this compiles cleanly under the existing
`static_assert(std::is_trivially_destructible_v<Op>)` and adjust the
union/struct layout if the compiler objects. The intent is just "Add
carries two Terms instead of one"; the exact C++ representation is an
implementation detail Claude Code should get working cleanly, not a
literal transcription requirement.]**

Update `handle_cons_ops` in `rap/rap.hpp`: the `add` op case must now
extract **two** terms from the relational `(add rel-term args)` call —
the surface syntax for an `add` ChangeSet operation becomes
`(add rel-term args)` (two arguments) rather than `(add rel-term)`
(one argument). **[IMPL NOTE: decide whether to support both
`(add rel-term)` (one-argument, implying `args = ()`) and
`(add rel-term args)` (two-argument) at the relational surface, for
ergonomics — a query that never publishes anything shouldn't be forced
to write `(add rel-term ())` everywhere. If supporting both, dispatch
on whether the `add` op term unifies as a 1-tuple or 2-tuple of
arguments inside `handle_cons_ops`. This mirrors how `cons-ops` already
dispatches on the shape of its `Op` argument — read that existing
dispatch logic and follow the same pattern.]**

Update `apply_changeset` in `rap/loop.hpp`:

```cpp
case OpTag::Add:
    agenda.enqueue(op.add.rel_term, op.add.args);
    break;
```

---

## Part 5: stdlib — find-by-contento Simplified

With `args` now plain, published data (not a closure to be read via
any special builtin), `find-by-contento` goes back to matching
directly against it:

```scheme
; find-by-contento: find the id of the agenda entry whose published
; args unify with pattern. Ordinary multi-solution relation — use
; run 1 / run* / Probe as needed, exactly like membero.
(defrel (find-by-contento agenda pattern id)
  (fresh (entry rest entry-id entry-rel entry-args)
    (== agenda (entry . rest))
    (disj
      (conj
        (== entry (entry-id entry-rel entry-args))
        (== entry-args pattern)
        (== id entry-id))
      (find-by-contento rest pattern id))))
```

No `rel-argso` call anywhere in this definition — `entry-args` is
already sitting right there in the destructured entry.

Retain `caro`/`cdro`/`conso` from STAGE_B_FIX Part 7 unchanged — these
were never part of the closure-introspection mistake and remain
useful, ordinary list primitives.

---

## Part 6: rel-arityo — Narrow, Honest Replacement

`(rel-arityo rel-term n)` — `rel-term` must walk to a `TermTag::Rel`.
Unifies `n` with that Rel's `param_count` (an `Int`).

This reads a public, structural fact already present on every
`RelNode` regardless of closures — it is not closure introspection,
and it does not require any of the retired `captured_values`
machinery.

```cpp
StepResult Evaluator::handle_rel_arityo(
    const Term* args, std::uint32_t arg_count, State& st)
{
    if (arg_count != 2) return StepResult::NoYield;
    Term rel_t = resolve_bvar(args[0], st.env);
    rel_t = walk(rel_t, st.subst, RelEnv{});
    if (rel_t.tag != TermTag::Rel || !rel_t.rel) return StepResult::NoYield;

    Term n = Term::integer(static_cast<std::int32_t>(rel_t.rel->param_count));
    Term walked_out = resolve_bvar(args[1], st.env);
    walked_out = walk(walked_out, st.subst, RelEnv{});
    const Binding* s = st.subst;
    if (!unify(*arena_, walked_out, n, nullptr, s, RelEnv{}))
        return StepResult::NoYield;
    st.subst = s;
    return apply_k_or_yield(*arena_, /* ... */);
}
```

Add `sym_rel_arityo_` and dispatch entry in `handleKnownRelation`,
following the established pattern.

---

## Part 7: wc.rap — Back to One Self-Perpetuating Query

No carrier, no dummy body, no `rel-argso`. `wc-counter` publishes its
own running state as `args` and receives it back when it next runs.

```scheme
; examples/wc.rap — character, word, and line counter
; State (chars words lines in-word) is published as this query's own
; args — handed back to it directly when it next runs. No separate
; carrier entry, no closure introspection.

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

; wc-counter is a 2-parameter Rel: (agenda args). args is
; (chars words lines in-word input) — its own published state PLUS
; the most recent input block. [IMPL NOTE: see below on how input
; actually reaches this query — this is the integration point that
; needs resolving, same as it did in the prior spec, but now simpler
; because there's no separate carrier to coordinate with.]
(defrel (wc-counter agenda args ops)
  (fresh (chars words lines in-word input)
    (conj
      (== args (chars words lines in-word input))
      (disj
        ; EOF: report final counts, do not re-add (stop the loop).
        (conj
          (== input ())
          (call cons-ops (output (chars chars words words lines lines))
                          (no-ops) ops))
        ; Data: update state, re-add self with updated args published.
        (fresh (c2 w2 l2 iw2 ops0 next-input)
          (conj
            (=/= input ())
            (count-charo-listo input chars words lines in-word
                                c2 w2 l2 iw2)
            (call cons-ops
              (add (rel (next-agenda next-args)
                     (call wc-counter next-agenda next-args next-ops))
                   (c2 w2 l2 iw2 next-input))
              (no-ops) ops)))))))

(defrel (main args ops)
  (call cons-ops
    (add (rel (agenda wc-args) (call wc-counter agenda wc-args opsx))
         (0 0 0 0 ()))
    (no-ops) ops))
```

**[IMPL NOTE — the actual integration question, carried over from the
prior spec and still the one real open design point: how does newly
arrived input from `raprunner` actually reach `wc-counter`'s `args`
slot for its *next* run, given that `raprunner` dispatches arriving
input to a fixed `handle_input` symbol, not to whatever-was-last-
enqueued? Two honest options:**

**Option A:** `wc-counter` does NOT receive input via its own `args`
at all. Instead, `handle_input` stays the fixed dispatcher: on each
input block, it uses `find-by-contento` to locate `wc-counter`'s
current entry (matching on a recognizable shape in its published
`args`, e.g. matching just the first four elements and leaving
`input`/5th slot as a wildcard), reads its current
`chars`/`words`/`lines`/`in-word`, removes that entry, computes updated
state using `count-charo-listo` against the newly arrived input
directly inside `handle_input` (not inside `wc-counter`'s own body),
and re-adds `wc-counter` with the updated four-element state published
(no 5th "input" element needed in `args` at all — `wc-counter`'s body
in this case might not even need to do anything except exist and hold
state, though it should still have a *real*, non-dead body — e.g. it
could simply re-publish itself unchanged if somehow run directly,
which would only happen if `raprunner`'s drain loop runs it before
new input arrives, which is fine and harmless).

**Option B:** keep `wc-counter` as the active consumer of input
directly (closer to the sketch above), which requires `raprunner`
itself to be modified so that input dispatch is not hardcoded to a
fixed `handle_input` symbol — a bigger, more invasive raprunner change
than this spec's scope likely warrants.

**Recommend Option A.** It requires no `raprunner.cpp` changes at all,
keeps `handle_input` as the stable, fixed integration point (consistent
with every other example), and `wc-counter`'s body becomes simple: hold
state, exist, do nothing interesting when run (or even — reconsider
whether `wc-counter` needs to ever actually run at all under Option A,
versus simply being a 1-parameter `Rel` whose body never matters
because `handle_input` always finds-removes-recreates it before it
would ever naturally be dequeued; if so, document this explicitly
rather than leaving it implicit, since "a Rel that in practice never
runs" is worth being honest about, distinct from the explicitly
-flagged dead-body carriers we just retired — the difference here is
this Rel COULD run correctly if dequeued, it just normally won't be
given how `handle_input` is expected to behave).**

Rewrite `wc-counter`/`main`/`handle_input` together, following Option
A, once this is confirmed working. Report the final design.

---

## Part 8: todo.rap — Items Publish Their Own Content

Each todo item becomes a minimal, genuinely-functioning
self-perpetuating query (not a dead-bodied carrier) that publishes
`(todo item)` as its own `args` and, if ever actually dequeued and run
without being removed first, simply re-adds itself unchanged — a
real, if usually-uninteresting, body.

```scheme
; A todo item: 1-parameter Rel, publishes (todo item) as args.
; If ever dequeued and run (not removed first), it just re-adds
; itself unchanged — genuinely functioning, not a dead body.
(defrel (todo-itemo item rel-out)
  (== rel-out
      (rel (agenda) (call todo-itemo-body agenda item))))

(defrel (todo-itemo-body agenda item)
  (fresh (self-rel)
    (conj
      (todo-itemo item self-rel)
      (call cons-ops (add self-rel (todo item)) (no-ops) ops))))
```

**[IMPL NOTE: confirm this self-referential re-add pattern actually
compiles and runs correctly — `todo-itemo-body` calling `todo-itemo`
to reconstruct its own form is a small recursive-looking definition
that should terminate immediately (it constructs one Rel and adds it,
it does not loop within a single execution), but verify there's no
issue with a relation being defined partly in terms of constructing a
call to itself in this way. If this proves awkward, an acceptable
simplification: have `handle_input`'s `add` command construct the
`(rel (agenda) (call todo-itemo-body agenda item))` form directly and
inline, without a separate `todo-itemo` helper relation — the helper
exists only for readability and is not load-bearing.]**

```scheme
(defrel (collect-todoso agenda items)
  (disj
    (conj (== agenda ()) (== items ()))
    (fresh (entry rest entry-id entry-rel entry-args item tail)
      (conj
        (== agenda (entry . rest))
        (== entry (entry-id entry-rel entry-args))
        (== entry-args (todo item))
        (collect-todoso rest tail)
        (== items (item . tail))))
    (fresh (entry rest entry-id entry-rel entry-args)
      (conj
        (== agenda (entry . rest))
        (== entry (entry-id entry-rel entry-args))
        (fresh (x) (=/= entry-args (todo x)))
        (collect-todoso rest items)))))
```

**Note on the third `collect-todoso` branch:** the prior session found
that `(=/= entry-args (todo x))` with a fresh `x` never actually fires
as a useful guard (the constraint is recorded but `x` stays unbound, so
it never triggers) — this was the `collect-todoso` duplicate-output bug
fixed directly in `examples/todo.rap` last session by deleting this
branch entirely, since at the time every agenda entry actually was a
todo item. **That assumption no longer holds once `strengthen-agendao`-
style or other non-todo entries could coexist on a general agenda** —
but for `todo.rap` specifically, where the agenda genuinely only ever
contains todo items (this is a single-purpose example program), the
same simplification — delete the third branch, assume every entry is a
todo item — remains valid and should be kept. Do not reintroduce the
non-firing guard pattern; if a future multi-purpose program needs to
skip non-matching entries, the correct guard is `(=/= entry-args (todo
ground-binding))` checked against something actually bound, not a
fresh variable — but `todo.rap` itself does not need this.

`handle_input`'s `add` branch constructs `(todo-itemo arg carrier)` (or
the inlined equivalent per the IMPL NOTE) and adds it with
`(add carrier (todo arg))`. The `done` branch uses `find-by-contento`
against `(todo arg)` directly — no `rel-argso` needed.

---

## Part 9: test_stage2.cpp — Direct Destructuring, No Carriers

Replace `make_carrier` entirely. Agenda entries are built directly via
`Agenda::enqueue(rel_term, args)` where `rel_term` is any minimal,
genuinely-valid 1-parameter `Rel` (it does not need to do anything
interesting for this test — `(rel (a) (== a a))` is fine, since this
test never actually calls these entries as queries, only
`strengthen-agendao` reasons about their published `args`) and `args`
is the check-content term directly:

```cpp
// Replaces make_carrier — build a minimal valid Rel and enqueue it
// with the actual content as its published args, directly.
static Term make_minimal_rel(Arena& stable) {
    Term bv0; bv0.tag = TermTag::BVar; bv0.id = 0;
    Goal* body = stable.make<Goal>();
    body->tag  = GoalTag::Eq;
    body->eq.u = bv0;
    body->eq.v = bv0;
    RelNode* rn = stable.make<RelNode>();
    rn->param_count = 1;
    rn->body        = body;
    return Term::relation(rn);
}

// Usage:
std::uint32_t id10 = loop.agenda.enqueue(make_minimal_rel(loop.intern_arena),
                                          content10);
```

**Note on the distinction from the retired `make_carrier`:** this Rel
is still trivial and is still never meaningfully called in this test —
but the difference from the retired carrier pattern is structural, not
behavioral: nothing about this design relies on the engine refusing to
call it, skipping it, or treating it specially because of what it
contains. It is an entirely ordinary, executable (if uninteresting)
`Rel`; the test simply chooses never to dequeue-and-run it in this
particular scenario. `strengthen-agendao` and friends reason entirely
over the entries' published `args`, never touching `rel_term` at all.

Update `weak-check-qido`, `not-weak-check-qido`, `collect-weak-qidso`,
and `strengthen-agendao` to destructure `(id rel-term args)` directly:

```scheme
(defrel (weak-check-qido entry H T qid)
  (fresh (rel-term)
    (== entry (qid rel-term (check H T)))))

(defrel (not-weak-check-qido entry H T)
  (fresh (qid rel-term chk)
    (== entry (qid rel-term chk))
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

(defrel (strengthen-agendao agenda ops)
  (fresh (H T R strong-qid weak-qids ops0)
    (conj
      (find-by-contento agenda (check+ H T R) strong-qid)
      (collect-weak-qidso agenda H T weak-qids)
      (call qids->remove-opso weak-qids ops0)
      (call cons-ops (output (pruned H T)) ops0 ops))))
```

**Expected results unchanged from every prior version of this test:**
Remove(10), Remove(12), Output((pruned hypA test1)), entries 11 and 13
remain.

---

## Acceptance Criteria

- [ ] Part 0 retirement checklist fully completed; confirmed no
      orphaned dependents before removal
- [ ] `QueryEntry` gains `args`; enqueue/remove/compaction correctly
      handle both `query_term` and `args` together
- [ ] `Agenda::enqueue(rel_term, args)` rejects non-Rel and
      wrong-param_count Rels
- [ ] `as_term()` produces `(id rel-term args)` 3-element lists
- [ ] `run_one()` dispatches 1 vs 2 call arguments based on
      `param_count`, confirmed no behavior change for any existing
      1-parameter Rel
- [ ] `Op::Add` carries `(rel_term, args)`; `handle_cons_ops` updated;
      surface syntax decision on 1-arg vs 2-arg `add` resolved and
      documented
- [ ] `find-by-contento` simplified to direct `args` matching, no
      `rel-argso` call
- [ ] `rel-arityo` added as a narrow builtin
- [ ] `wc.rap` rewritten per Part 7, Option A integration confirmed
      working and documented
- [ ] `todo.rap` rewritten per Part 8, self-perpetuating
      `todo-itemo`/inlined-equivalent confirmed working
- [ ] `test_stage2.cpp` rewritten per Part 9, exact original results
      reproduced
- [ ] `make test` passes
- [ ] Manual test: `wc.rap` correct across multiple input blocks
- [ ] Manual test: `todo.rap` add/done/list cycle correct, no
      duplicate-output regression
- [ ] No example or stdlib relation enqueues a non-`Rel`, or a `Rel`
      with `param_count` other than 1 or 2, anywhere
- [ ] No relation anywhere calls a builtin that reads into a Rel's
      closed-over values — `rel-argso` (closure-reading version) no
      longer exists to be called

---

## What This Is Not

- Not an extension of STAGE_B_AFTER_FIX — full retirement of its
  closure-introspection mechanism, as described in Part 0
- No change to `boundo`/`groundo` — unaffected, remain as specified in
  `STAGE_B_FIX.md`
- No change to the structural Rel-only agenda invariant — strengthened,
  if anything, since `param_count` is now also validated at enqueue
- No "bound query"/frame-slot mechanism — explicitly future work for a
  different paper, per the project owner; `args` is named in
  anticipation of that future direction but nothing here implements it

---

*v1.0 June 2026 — initial specification*

