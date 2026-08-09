# STAGE_B_AFTER_FIX: Explicit Closure Capture and Remaining STAGE_B_FIX Work

**Version:** 1.0  
**Depends on:** STAGE_B_FIX Part 1–3 already implemented (id-only Remove
             reverted, structural Rel-only invariant, `as_term()`
             shape change). If those are not yet implemented, implement
             them first per `docs/STAGE_B_FIX.md` Parts 1–3 before this
             document.  
**Supersedes:** `docs/STAGE_B_FIX.md` Part 4 (`rel-argso` as originally
             specified) and the unresolved Part 5 integration question  
**Modifies:** `core/mktypes.hpp`, `core/core.hpp`, `core/sexp_parser.hpp`,
             `stdlib/core.rap`, `rap/test_stage2.cpp`,
             `examples/todo.rap`, `examples/wc.rap`  
**Status:** Specification — not yet implemented  
**Date:** June 2026

---

## Background: Why Part 4 of STAGE_B_FIX Needed Revision

Investigation during an earlier implementation attempt established that
`(rel (params...) body)` compiles outer-scope variable references as
predicted `Var(id)` literals baked into the body's `Goal` tree at
**parse time**. These predicted IDs are only meaningful within the
substitution of whatever query originally constructed the Rel. When a
Rel escapes that query — by being placed in a `ChangeSet`'s `add`
operation and later run as a different, unrelated query — its outer
Var references are dangling: the new query's substitution has no
binding for those IDs, and the originally captured values are gone.

Neither `deep_resolve_bvar` nor `walk_deep` recurses into Rel bodies,
so there is no existing mechanism that resolves these dangling
references before a Rel is enqueued. This is a correctness gap that
only manifests when a Rel is meant to persist on the agenda past the
query that constructed it — exactly the self-perpetuating-query
pattern this work depends on.

**Resolution:** add an explicit-capture builtin (`_relo`) that snapshots
caller-specified values into a Rel at construction time, storing them
as a real field on a freshly allocated `RelNode`. Add `raw-rel` as a
`stdlib/core.rap` convenience wrapper over `_relo` with call-site sugar
closer to ordinary `(rel ...)` syntax. Do **not** attempt automatic
capture-by-scanning in this spec — that requires a different, separate
builtin exposing compiled-body Var-ID references as data, which is
explicitly deferred (see "Future Work" at the end of this document).

---

## Part 1: RelNode Gains Captured Values

Add a captured-values field to `RelNode`:

```cpp
// core/core.hpp
struct RelNode {
    std::uint32_t param_count;
    const Goal*   body;
    const Term*   captured_values;   // ADD: nullptr if no captures
    std::uint32_t captured_count;    // ADD: 0 if no captures
};
```

Existing `(rel (params...) body)` parse-time construction (in
`compile_term`, `sexp_parser.hpp`) sets `captured_values = nullptr,
captured_count = 0` — **completely unchanged in behavior**. Ordinary
`(rel ...)` forms used immediately within the query that constructs
them (the existing, already-working use pattern — e.g. the
`eveno`/`oddo` mutual recursion example) are unaffected by this spec.
This field is populated only by the new `_relo` builtin, described
next.

Update `core/mktypes.hpp` if `RelNode`'s forward declaration needs
adjustment (it should not, since this is a field addition, not a
shape change visible to forward declarations — confirm and note if
this assumption is wrong).

---

## Part 2: _relo — Explicit-Capture Builtin

`(_relo param-count body-template captures rel-out)` — constructs a
Rel term with `param_count` parameters, `body-template` as its
(already-compiled, ordinary) body, and `captures` as a list of
already-resolved (ground, at the time of the call) terms snapshotted
into the new `RelNode`.

**This is intentionally a low-level, somewhat awkward-to-call builtin.**
Its job is to do the engine work correctly and minimally; `raw-rel`
(Part 3) is what programs actually call.

### Signature and behavior

```
(_relo param-count body-template captures rel-out)
```

- `param-count`: must be a bound `Int`. The arity of the constructed Rel.
- `body-template`: must walk to a term that the engine can use directly
  as the new RelNode's `body` — in practice, this means `body-template`
  is itself constructed via ordinary `(rel (params...) inner-body)`
  syntax at the call site, and `_relo` extracts that pre-built Rel's
  `body` field to reuse as the new RelNode's body. **[IMPL NOTE: the
  cleanest mechanism is for `_relo`'s first two logical arguments to
  collapse into one — caller passes an ordinary `(rel (params...)
  body)` term as `body-template`, and `_relo` copies its `param_count`
  and `body` into the new RelNode, only adding `captures`. Reconsider
  the signature as `(_relo template-rel captures rel-out)` — three
  arguments, not four — if this is cleaner to implement. Use
  judgment; the four-argument form above is the spec's first-pass
  sketch, not a requirement.]**
- `captures`: a list of terms, each of which must be ground (use
  `boundo` semantics — unbound elements should cause `_relo` to fail,
  since a capture that isn't actually resolved yet defeats the purpose
  of snapshotting). Deep-copy each into the **same arena the new
  RelNode is allocated in** (not the per-query `eval_arena`, which
  will be reset) — for `RapEvaluator`-driven execution this is the
  `rap_arena`/permanent arena already used for other long-lived
  RAP-layer allocations; for the base `Evaluator` (no RAP layer), this
  is whatever arena the caller designates as long-lived. **[IMPL NOTE:
  determine the correct destination arena by reading how the existing
  `Op::Add` / `ChangeSet` deep-copy-on-add mechanism already handles
  this for ordinary Rel terms placed in `add` operations — captures
  should follow the same arena-lifetime discipline already established
  there, not a new one.]**
- `rel-out`: unified with the resulting `Term::relation(new_node)`.

### Implementation sketch

```cpp
StepResult Evaluator::handle_relo(
    const Term* args, std::uint32_t arg_count, State& st)
{
    if (arg_count != 3) return StepResult::NoYield;  // see IMPL NOTE above
                                                       // re: 3 vs 4 args

    Term template_rel = resolve_bvar(args[0], st.env);
    template_rel = walk(template_rel, st.subst, RelEnv{});
    if (template_rel.tag != TermTag::Rel || !template_rel.rel)
        return StepResult::NoYield;

    Term captures_list = resolve_bvar(args[1], st.env);
    captures_list = walk(captures_list, st.subst, RelEnv{});

    // Walk the list, checking each element is bound (boundo semantics),
    // counting elements, deep-copying each into the destination arena.
    // [IMPL NOTE: write this list-walk in C++ directly here — it does
    // not need to call the boundo/groundo relations, it can just check
    // TermTag::Var directly the same way handle_boundo does, since
    // this is engine code, not relational code.]

    RelNode* new_node = /* allocate in destination arena */;
    new_node->param_count    = template_rel.rel->param_count;
    new_node->body           = template_rel.rel->body;
    new_node->captured_values = /* the deep-copied array */;
    new_node->captured_count  = /* count */;

    Term result = Term::relation(new_node);
    Term walked_out = resolve_bvar(args[2], st.env);
    walked_out = walk(walked_out, st.subst, RelEnv{});
    const Binding* s = st.subst;
    if (!unify(*arena_, walked_out, result, nullptr, s, RelEnv{}))
        return StepResult::NoYield;
    st.subst = s;
    return apply_k_or_yield(*arena_, /* ... */);
}
```

Add `sym_relo_` to the interned built-in names and dispatch entry in
`handleKnownRelation`, following the established pattern.

---

## Part 3: rel-argso Becomes a Trivial Field Read

With `captured_values`/`captured_count` now real fields on `RelNode`,
`rel-argso` (as originally specified in `STAGE_B_FIX.md` Part 4) is now
genuinely simple — no investigation-dependent uncertainty remains:

```cpp
StepResult Evaluator::handle_rel_argso(
    const Term* args, std::uint32_t arg_count, State& st)
{
    if (arg_count != 2) return StepResult::NoYield;
    Term rel_t = resolve_bvar(args[0], st.env);
    rel_t = walk(rel_t, st.subst, RelEnv{});
    if (rel_t.tag != TermTag::Rel || !rel_t.rel) return StepResult::NoYield;

    const RelNode* node = rel_t.rel;
    Term result = Term::nil();
    for (std::int32_t i = static_cast<std::int32_t>(node->captured_count) - 1;
         i >= 0; --i) {
        PairNode* p = arena_->make<PairNode>();
        if (!p) return StepResult::NoYield;
        p->car = node->captured_values[i];
        p->cdr = result;
        result = Term::make_pair(p);
    }

    Term walked_out = resolve_bvar(args[1], st.env);
    walked_out = walk(walked_out, st.subst, RelEnv{});
    const Binding* s = st.subst;
    if (!unify(*arena_, walked_out, result, nullptr, s, RelEnv{}))
        return StepResult::NoYield;
    st.subst = s;
    return apply_k_or_yield(*arena_, /* ... */);
}
```

A Rel with `captured_count == 0` (the ordinary, non-`_relo`-constructed
case — i.e. every Rel built via plain `(rel ...)` syntax today) yields
`()` from `rel-argso`. This is correct and expected: ordinary Rels have
nothing captured to report.

Add `sym_rel_argso_` and dispatch entry in `handleKnownRelation`,
alongside `sym_boundo_` (unchanged from `STAGE_B_FIX.md` Part 4 —
`boundo` requires no revision; only `rel-argso` was affected by the
investigation).

---

## Part 4: raw-rel — stdlib Sugar

Add to `stdlib/core.rap`:

```scheme
; (raw-rel template captures rel-out) — sugar over _relo.
; template is an ordinary (rel (params...) body) term, written exactly
; as it would be for a plain Rel. captures is a list of already-bound
; values to snapshot into the result, so the constructed Rel survives
; being enqueued and run by a different, later query.
;
; Example:
;   (raw-rel (rel (next-agenda next-input next-ops)
;              (call wc-counter next-agenda c2 w2 l2 iw2
;                    next-input next-ops))
;            (c2 w2 l2 iw2)
;            carrier)
; constructs `carrier`, a Rel that — when later called with
; (next-agenda next-input next-ops) — runs the given body, with
; c2/w2/l2/iw2's *current* values snapshotted and available via
; (rel-argso carrier args).
;
; Note: raw-rel does not itself bind the captured values as variables
; inside the template body — the template body must reference the
; captures it needs through its own closed-over Var mechanism exactly
; as plain (rel ...) already does (this is unchanged from ordinary Rel
; construction). raw-rel's captures list exists so that OTHER queries
; can later introspect what this Rel is carrying via rel-argso — it is
; not how the Rel's own body accesses those values when called.
(defrel (raw-rel template captures rel-out)
  (_relo template captures rel-out))
```

**Important clarification on what `captures` is actually for:**
`raw-rel`/`_relo`'s capture mechanism solves the *introspection*
problem (another query can later ask "what is this pending entry
carrying" via `rel-argso`) — it does **not**, by itself, solve the
*execution* problem (the Rel's own body still needs the values it
closed over to actually be available when the body runs). The
template body's own outer-Var references work exactly as plain
`(rel ...)` always has: by direct reference within the *same* query
that constructs and immediately uses them. For a self-perpetuating
query, this means the safe, correct pattern is:

```scheme
; Inside wc-counter, when re-enqueueing itself with updated state:
(call cons-ops
  (add (rel (next-agenda next-input next-ops)
         (call wc-counter next-agenda c2 w2 l2 iw2 next-input next-ops)))
  (no-ops) ops0)
```

This already works correctly **without** `raw-rel`, because `c2`,
`w2`, `l2`, `iw2` are referenced directly in the body and the `(add
...)` ChangeSet operation's existing deep-copy-on-add mechanism
(confirmed to exist — see Part 2's IMPL NOTE) already resolves and
copies these values correctly at the moment the ChangeSet is
constructed, before the originating query's substitution goes away.
**The dangling-reference problem only arises when some OTHER query
later needs to discover those values without calling the Rel** — i.e.
purely for introspection (`rel-argso`), not for the Rel's own
correct execution.

**This means `wc.rap`'s self-perpetuation (re-enqueueing itself with
updated state) was never actually broken** — only the *introspection*
path (`find-by-contento` reading another entry's carried values from
outside) needs `_relo`/`raw-rel`/`rel-argso` at all. Re-confirm this
understanding is correct by testing a self-perpetuating Rel's
re-enqueue-with-updated-closed-over-values pattern in isolation,
without any `_relo` involvement, before assuming `wc.rap` needs
`raw-rel` for its own loop — it may only need `raw-rel` at the specific
point where `handle_input` needs to *find* the live counter via
`find-by-contento`, not for the counter's own self-perpetuation.

---

## Part 5: wc.rap Integration (Resolves STAGE_B_FIX Part 5)

Per Investigation 2's findings: raprunner always dispatches arriving
input to the fixed `handle_input` relation — there is no mechanism to
redirect input dispatch to a different, dynamically-chosen Rel.
Therefore Approach (b) is the only feasible path: `handle_input` stays
the fixed entry point and is responsible for locating the live
wc-counter entry via `find-by-contento` + `rel-argso`.

```scheme
; examples/wc.rap

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

; A carrier Rel: when called, does nothing (it is never actually
; invoked as a query — only introspected via rel-argso by
; handle_input, then removed and replaced). Its sole purpose is to
; sit on the agenda holding (chars words lines in-word) for later
; discovery.
(defrel (wc-carriero chars words lines in-word carrier)
  (raw-rel (rel (unused) (== unused unused))
           (chars words lines in-word)
           carrier))

(defrel (main args ops)
  (fresh (carrier)
    (conj
      (wc-carriero 0 0 0 0 carrier)
      (call cons-ops (add carrier) (no-ops) ops))))

(defrel (handle_input agenda fd input ops)
  (disj
    ; EOF: find the live carrier, report final counts, do not re-add.
    (fresh (id rel-term capture-args chars words lines iw)
      (conj
        (== input ())
        (find-by-contento agenda chars words lines iw id)
        ; [IMPL NOTE: find-by-contento as specified in STAGE_B_FIX
        ; Part 7 matches on a single-pattern capture list — wc-carrier
        ; has FOUR captured values, not one. Either generalize
        ; find-by-contento to accept a multi-element pattern list
        ; directly (the cleanest fix — STAGE_B_FIX Part 7's
        ; single-element assumption was specific to todo.rap's
        ; single-value carriers and should not have been baked in as
        ; a general assumption), or write a wc.rap-specific finder.
        ; Generalizing find-by-contento is almost certainly correct
        ; — fix it there, not here.]
        (call cons-ops
          (output (chars chars words words lines lines))
          (no-ops) ops)))
    ; Data: find carrier, update state, remove old, add new.
    (fresh (id chars words lines iw c2 w2 l2 iw2 new-carrier ops0 ops1)
      (conj
        (=/= input ())
        (find-by-contento agenda chars words lines iw id)
        (count-charo-listo input chars words lines iw c2 w2 l2 iw2)
        (call cons-ops (remove id) (no-ops) ops0)
        (wc-carriero c2 w2 l2 iw2 new-carrier)
        (call cons-ops (add new-carrier) ops0 ops1)
        (== ops ops1)))))
```

**Note on `wc-carriero`'s dummy body:** the carrier's body
`(rel (unused) (== unused unused))` is never meant to be called — it
exists purely as a vehicle for `raw-rel`'s captures mechanism so
`rel-argso` has something to introspect. This is a slightly unusual
idiom (a Rel that exists to be inspected, not invoked) and is worth a
one-sentence callout if this pattern is reused elsewhere or written
about later — it is intentional, not an oversight.

---

## Part 6: Generalize find-by-contento

Revise `STAGE_B_FIX.md` Part 7's `find-by-contento` to accept a
captures list of any length, not just one element, since `wc.rap`
needs four:

```scheme
(defrel (find-by-contento agenda pattern-list id)
  (fresh (entry rest entry-id entry-rel entry-args)
    (== agenda (entry . rest))
    (disj
      (conj
        (== entry (entry-id . entry-rel))
        (rel-argso entry-rel entry-args)
        (== entry-args pattern-list)
        (== id entry-id))
      (find-by-contento rest pattern-list id))))
```

This is called as `(find-by-contento agenda (chars words lines iw) id)`
for `wc.rap` (a 4-element pattern list) and
`(find-by-contento agenda (item) id)` for `todo.rap` (a 1-element
pattern list) — no change to the relation itself, just confirming the
call sites already pass a list, which they do under STAGE_B_FIX Part
7's original definition. **This means STAGE_B_FIX Part 7 likely
requires no actual code change** — re-read it; the single-element
examples shown there were illustrative, and the relation as written
already takes `pattern` as whatever list shape the caller provides.
Confirm this during implementation rather than assuming a change is
needed.

---

## Acceptance Criteria

- [ ] `RelNode` gains `captured_values`/`captured_count`, defaulting
      to `nullptr`/`0` for ordinary `(rel ...)` construction —
      confirmed no behavior change for existing Rel usage
      (`eveno`/`oddo` mutual recursion test still passes)
- [ ] `_relo` builtin added, dispatched via `handleKnownRelation`
- [ ] Destination arena for captured values confirmed by reading the
      existing `Op::Add` deep-copy mechanism, not assumed
- [ ] `rel-argso` implemented as a trivial field read, returns `()`
      for any Rel with `captured_count == 0`
- [ ] `raw-rel` added to `stdlib/core.rap` as sugar over `_relo`
- [ ] Confirmed (by isolated test) that ordinary self-perpetuating
      re-enqueue-with-updated-closures does NOT require `_relo`/
      `raw-rel` — only cross-query introspection does
- [ ] `find-by-contento` confirmed to already support multi-element
      pattern lists with no change, or fixed if it does not
- [ ] `wc.rap` rewritten per Part 5: `wc-carriero` dummy-bodied
      carrier, `handle_input` as fixed dispatcher using
      `find-by-contento` + `rel-argso` to locate and replace state
- [ ] `todo.rap`, `strengthen-agendao` from `STAGE_B_FIX.md` Parts 6–7
      implemented (unaffected by this document's changes beyond
      depending on the now-resolved `rel-argso`)
- [ ] `make test` passes, including `test_stage2`'s exact original
      results: Remove(10), Remove(12), Output((pruned hypA test1)),
      entries 11 and 13 remaining
- [ ] Manual test: `todo.rap` add/done/list cycle correct
- [ ] Manual test: `wc.rap` correct counts across multiple input
      blocks (must specifically test more than one block arriving,
      to exercise find-then-replace, not just a single block)
- [ ] No example or stdlib relation enqueues a non-`Rel` term anywhere

---

## Future Work (Explicitly Deferred — Do Not Implement)

**Automatic capture by static analysis.** A future builtin —
tentatively `relbodyvarso` or similar — that walks a Rel's *compiled
body Goal tree* and returns the list of outer Var IDs it references,
as ordinary data. Once such a list exists as data, `boundo`/`groundo`
already provide everything needed to check each referenced var's
state one at a time — no further introspection machinery would be
needed beyond this one new builtin. This would allow a true `rel`-like
stdlib relation that captures outer values automatically rather than
requiring the explicit list `raw-rel` currently requires. Not
specified further here; revisit only if `raw-rel`'s explicit-list
ergonomics prove genuinely burdensome in practice.

---

*v1.0 June 2026 — initial specification*

