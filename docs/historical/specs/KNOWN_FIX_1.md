# Bug Report: GoalTag::Fresh Continuation Env Leak

**Severity:** High — silent incorrect behavior, not a crash  
**Found:** June 2026, during STAGE_B_AFTER_FIX implementation  
**Fixed:** Same session, committed to `main`  
**Affects:** Any program using nested `fresh` blocks where a continuation
            goal after the inner `fresh` block resolves a `BVar` that
            was bound in an *outer* scope

---

## Summary

`GoalTag::Fresh`'s step-handler did not restore the calling continuation's
`env` after a nested `fresh` block's body completed. Any goal that ran
*after* a nested `fresh` block, as part of the same enclosing `conj`,
could silently resolve `BVar` indices against the wrong environment —
the inner `fresh` block's environment, rather than the outer one it
should have returned to.

This is the same category of bug that `GoalTag::Call` already had a
fix for (a `RestoreEnv` continuation wrapper, added during Stage 2 —
see `docs/STAGE_2.md`), but the equivalent fix had not been applied to
`GoalTag::Fresh`.

---

## How It Was Found

While implementing `wc.rap`'s character-counting logic
(`count-charo-listo` calling `count-charo`, both using nested `fresh`
and `disj`), output was consistently wrong for any input containing
space characters — counts came back as zero regardless of actual
input content. Isolating the failure to a minimal test case (a
standalone program exercising just `count-charo-listo` outside of
`raprunner`) showed that variable bindings made *inside* a nested
`fresh` block were not visible to goals that ran immediately after it,
within the same outer `conj` — even though those goals referenced
outer-scope `BVar`s, not anything from the inner block.

This had not surfaced in any prior test (`parse_run.cpp`'s twelve
programs, `test_stage2.cpp`, `test_arith.cpp`) because none of those
programs happened to have a continuation goal, after a nested `fresh`
block, that needed to resolve an outer-scope `BVar` at a depth where
the inner block's altered environment would produce a different,
wrong-but-not-obviously-wrong result. The bug requires: (a) a nested
`fresh` inside a larger goal sequence, (b) a goal after that nested
`fresh` in the same sequence, and (c) that later goal referencing a
`BVar` whose resolution depends on the *outer* environment depth, not
the inner one. Smaller test programs tend not to combine all three
conditions in a way that produces visibly wrong output rather than
coincidentally-correct output.

---

## Root Cause

In `step()`'s `GoalTag::Call` handling, the continuation passed to the
called relation's body is wrapped with a `KontTag::RestoreEnv` marker
(added during Stage 2; see `docs/STAGE_2.md`), ensuring that once the
callee's body finishes, control returns to the caller's continuation
with the caller's original `env` restored — not the callee's.

`GoalTag::Fresh` introduces a new environment frame (extending `env`
with the fresh variables' bindings) for its body, conceptually
identical to what happens when a relation is called — but its
step-handler did not wrap the outer continuation with the same
`RestoreEnv` mechanism. As a result, once a nested `fresh` block's body
completed and control returned to the *enclosing* sequence of goals,
those goals continued executing with the inner `fresh` block's
environment still in effect, rather than the environment that was
active before the nested `fresh` was entered.

For shallow programs (no further `BVar` resolution after the nested
block, or coincidentally compatible environment depths) this produces
no visible symptom. For deeper nested structures, `BVar` indices
computed at compile time for the *outer* scope get resolved against
the *inner* scope's frame stack, walking to the wrong binding or to no
binding at all.

---

## The Fix

Apply the same `KontTag::RestoreEnv` wrapping that `GoalTag::Call`
already used, to `GoalTag::Fresh`'s continuation as well — when control
returns from the fresh block's body to whatever follows it in the
enclosing sequence, the environment is restored to what it was
immediately before the `fresh` block was entered.

This is a small, local fix: one additional continuation-wrapping step
in `GoalTag::Fresh`'s case in `step()`, mirroring code that already
existed for `GoalTag::Call`. No changes to `BVar` compilation, no
changes to how `fresh` introduces bindings — only to how control
returns afterward.

---

## Verification

After the fix:
- All existing tests (`parse_run`'s 12 programs, `test_stage2`,
  `test_arith`, `core_test_extension`, `rap_test_extension`, `test_rap`)
  continued to pass — the fix did not change behavior for any program
  that wasn't exercising the buggy path.
- `wc.rap`'s character-counting logic, including space and newline
  handling, produced correct counts after the fix, where it had
  previously produced all-zero output for any input containing spaces.
- A standalone isolated test (nested `fresh` followed by a continuation
  goal resolving an outer-scope `BVar`) was used to confirm the fix
  directly, separate from the `wc.rap` symptom.

---

## Why This Matters Beyond wc.rap

This bug was not specific to character counting, text processing, or
any feature of STAGE_B/STAGE_B_FIX — it is a general correctness defect
in the core evaluator's handling of `fresh`, present since `fresh` was
first implemented. Any program with sufficiently nested `fresh` usage
combined with post-nesting continuation goals resolving outer-scope
variables could have been silently affected, with no error, no crash —
just quietly wrong answers.

The reason it went undetected through Stage 0A, Stage 0B, Stage 0C,
Stage 2, and STAGE_ARITH is that the existing test suite, while
reasonably thorough for the *features* each stage introduced, did not
happen to construct a program with the specific structural shape
(nested `fresh`, then a continuation referencing outer scope) needed
to expose it. This is a useful data point for future test design: test
coverage organized around "does each language feature work in
isolation" is not the same as coverage for "do features compose
correctly when nested," and the latter requires deliberately
constructed nested test cases, not just one test per feature.

**Recommendation:** add a regression test specifically exercising
nested `fresh` with a post-nesting continuation referencing an
outer-scope variable, to `core/test_extension.cpp` or a similarly
durable location, so this exact defect shape cannot silently
reappear if the `RestoreEnv` wrapping is ever accidentally removed
or altered during future refactoring.

---

*Filed June 2026. Fix committed to `main` during the STAGE_B_AFTER_FIX
implementation session.*

