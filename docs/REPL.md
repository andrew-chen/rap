# REPL: Interactive Query Shell

**Version:** 1.1  
**Depends on:** Stage DISEQ complete (core/ with =/=)  
**File:** `repl.cpp`  
**Status:** Implemented  
**Date:** May 2026 (updated June 2026)

---

## Purpose

A read-eval-print loop for the core engine. Allows interactive exploration
and scripted use via stdin redirection. No dependency on readline or any
external library. Stops on OOM. Covers `core/` only — not the RAP layer.

When complete:

```bash
./repl                          # interactive
./repl --verbose                # show compiled goal trees, defrel bodies, and arena usage
./repl --timing                 # print µs per query after each result
./repl --verbose --timing       # both
./repl < examples/appendo.rkt   # scripted via redirection
echo "(run 1 (q) (== q hello))" | ./repl
```

---

## User Experience

**Interactive mode** (stdin is a tty):

```
rap> (run 3 (q) (call membero q (1 2 3)))
q = 1
q = 2
q = 3
rap> (defrel (noto x) (=/= x yes))
defined: noto
rap> (run 2 (q) (conj (noto q) (disj (== q yes) (== q no))))
q = no
rap>
```

**Scripted mode** (stdin is not a tty): no prompt, same output otherwise.
Detection via `isatty(STDIN_FILENO)`.

**`--verbose` adds:**
- Arena usage (`[arena] name: used / total bytes`) after every `parse_query` call and after every `runN` call.
- The compiled goal tree for each `run` form (via `print_query`), before running.
- The compiled relation body (param count + Goal tree via `print_rel_body`) for every `defrel` defined in that dispatch call, printed immediately after "defined: name".

**`--timing` adds** a line after each query's results:

```
; 4.2 µs
```

Using `std::chrono::high_resolution_clock`, measuring from just before
`eval.runN()` to just after the last answer callback returns.

**OOM:** If `arena.alloc()` returns nullptr at any point, print:

```
error: out of memory — repl stopping
```

and exit with code 1. Do not attempt to recover or reset.

**Parse errors:** Print the existing diagnostic (already emitted by
`parse_query`) and continue to the next form. Do not exit.

**Empty input / EOF:** On EOF, exit cleanly with code 0. Print a newline
first if in interactive mode.

---

## Input Accumulation

Read character by character (or line by line) accumulating into a buffer.
Dispatch when the buffer contains a complete top-level form.

A form is complete when:
1. At least one non-whitespace character has been seen, AND
2. Open and close parentheses are balanced (depth == 0), AND
3. The last non-whitespace character was `)`

Track paren depth. When depth returns to 0 after being > 0, the form is
complete. Pass the accumulated string to `parse_query`.

**String literals:** The surface syntax does not have string literals, so
no special handling of parentheses inside strings is needed.

**Comments:** The accumulator must ignore parentheses that appear after
a `;` on the same line. When scanning a line for paren depth, stop
counting at the first `;` character. The full raw line (including the
comment) is still passed to `parse_query`, which handles comments
correctly — the comment stripping is only for depth-counting purposes.

**Implementation:** Use `std::getline` in a loop. Append each line to the
accumulator with a space separator. Check balance after each line.

```cpp
std::string accumulator;
int depth = 0;
std::string line;

while (std::getline(std::cin, line)) {
    for (char c : line) {
        if (c == ';') break; // ignore rest of line for depth counting
        if (c == '(') ++depth;
        if (c == ')') --depth;
    }
    accumulator += line;
    accumulator += ' ';

    if (depth == 0 && !accumulator empty/whitespace-only) {
        // dispatch accumulated form
        dispatch(accumulator);
        accumulator.clear();
        if (interactive) std::cout << "rap> " << std::flush;
    }
}
```

If depth goes negative (unmatched `)`) print a parse error and reset the
accumulator and depth to 0.

---

## Session State

The REPL maintains session state that persists across queries:

```cpp
struct ReplState {
    alignas(64) uint8_t  intern_buf[64 * 1024];
    Arena                intern_arena;
    Intern               intern;
    OutcomeSyms          syms;
    RelEnv               rel_env;      // accumulates defrel across session

    alignas(64) uint8_t  query_buf[256 * 1024];
    Arena                query_arena;  // reset after each query

    Evaluator*           eval;         // constructed once, uses query_arena
};
```

**`intern_arena`** — long-lived. Holds interned symbols and `RelEnv`
entries. Never reset during the session.

**`rel_env`** — accumulates `defrel` definitions across queries. When
`parse_query` processes a `defrel` form, it populates `pq.rel_env`. The
REPL merges `pq.rel_env` into the session `rel_env` after each successful
parse.

**`query_arena`** — reset after each query. Holds goal structures,
substitution bindings, and all per-query allocation.

**`eval`** — constructed once with `query_arena`. Its `client_region_`
is never used (core-only REPL), but the `Evaluator` base class is fine.

**RelEnv merging:** After `parse_query` returns, walk `pq.rel_env.head`
and for each entry call `session.rel_env.define(intern_arena, entry->name,
entry->rel_term)`. This is safe because `SymEntry*` pointers are stable
(interned into `intern_arena`) and `RelNode*` pointers point into
`query_arena` — which means they must be copied into `intern_arena` before
the query_arena is reset.

**Copying RelNode into intern_arena:** A `RelNode` is `{ uint32_t
param_count; const Goal* body }`. The `Goal*` tree must be deep-copied
into `intern_arena` before `query_arena` is reset. Add a
`deep_copy_goal(Arena& dest, const Goal* g)` function alongside
`deep_copy_term`. This is the one meaningful new function in the REPL.

---

## dispatch() — Processing One Form

```cpp
void dispatch(const std::string& src, ReplState& state,
              bool verbose, bool timing) {

    state.query_arena.reset();

    ParsedQuery pq = parse_query(state.query_arena, src.c_str(),
                                 state.intern, state.rel_env);

    if (!pq.goal) {
        // parse_query already printed a diagnostic
        return;
    }

    if (verbose) print_query(pq);

    // Check if this was a defrel-only form (no run).
    if (pq.n == 0) {
        // Merge new defrels into session rel_env.
        merge_rel_env(pq.rel_env, state);
        // Print confirmation for each new relation defined.
        for (auto* e = pq.rel_env.head; e; e = e->next)
            std::printf("defined: %s\n", e->name->str);
        return;
    }

    // Run the query.
    auto t0 = std::chrono::high_resolution_clock::now();
    int count = 0;

    state.eval->runN(pq.n, pq.goal, pq.qvar, pq.vars_used,
                     state.rel_env,
        [&](Term ans, State) {
            std::printf("q = ");
            print_term(ans);
            std::printf("\n");
            ++count;
        });

    auto t1 = std::chrono::high_resolution_clock::now();

    if (count == 0) std::printf("(no solutions)\n");

    if (timing) {
        double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        std::printf("; %.1f µs\n", us);
    }
}
```

**Note on `pq.n == 0`:** The current `parse_query` always expects a `run`
form. A `defrel`-only input (no `run` form) needs a small extension to
`parse_query` to handle the case where all top-level forms are `defrel`
and there is no `run`. In this case, return a `ParsedQuery` with
`goal == nullptr` and `n == 0` and `rel_env` populated. The REPL checks
`n == 0 && rel_env.head != nullptr` to distinguish "defrel only" from
"parse error". This requires a small change to `sexp_parser.hpp` — see
**Parser Extension** below.

---

## Parser Extension Required

`sexp_parser.hpp` currently requires a `run` form at the top level. Add
support for `defrel`-only input:

In `parse_query`, after processing all top-level forms:
- If a `run` form was found: behavior unchanged.
- If only `defrel` forms were found (no `run`): return a `ParsedQuery`
  with `goal = nullptr`, `n = 0`, `rel_env` populated with the defined
  relations. This is a **success** — not a parse error.
- If neither was found and input is non-empty: existing parse error path.

The REPL distinguishes these cases:
- `pq.goal == nullptr && pq.rel_env.head == nullptr` → parse error
- `pq.goal == nullptr && pq.rel_env.head != nullptr` → defrel-only, success
- `pq.goal != nullptr` → has a run form, execute it

---

## deep_copy_goal

Add to `core/core.hpp` (or a new `core/copy.hpp`):

```cpp
// Deep-copy a Goal tree from its current arena into dest.
// Required for persisting defrel bodies across query_arena resets.
inline const Goal* deep_copy_goal(Arena& dest, const Goal* g);
```

Recursively copies the `Goal` tree, calling `deep_copy_term` for all
`Term` fields. All `Goal*` pointers inside the copy point into `dest`.

This is structurally similar to `deep_copy_term` and uses the same
recursive approach. Term depth is bounded in practice.

Goal tags to handle:
- `Eq`: copy `g->eq.u` and `g->eq.v`
- `Diseq`: copy `g->diseq.u` and `g->diseq.v`
- `Disj`, `Conj`: copy `g->bin.g1` and `g->bin.g2`
- `Fresh`: copy `g->fresh.body`
- `Probe`: copy `g->probe.goal`, `g->probe.condition`
- `Call`: copy `g->call.rel_term`, copy each arg in `g->call.args`
- `RelNode` bodies reachable from `TermTag::Rel` terms: also copy into
  dest so they don't dangle when query_arena is reset

---

## merge_rel_env

```cpp
void merge_rel_env(const RelEnv& src, ReplState& state) {
    for (const RelEnvEntry* e = src.head; e; e = e->next) {
        // Deep-copy the RelNode body into intern_arena so it
        // survives query_arena reset.
        const Goal* body_copy = deep_copy_goal(
            state.intern_arena, e->rel_term.rel->body);

        RelNode* node = state.intern_arena.make<RelNode>();
        node->param_count = e->rel_term.rel->param_count;
        node->body        = body_copy;

        Term stable_rel = Term::relation(node);
        state.rel_env.define(state.intern_arena, e->name, stable_rel);
    }
}
```

---

## main()

```cpp
int main(int argc, char** argv) {
    bool verbose  = false;
    bool timing   = false;
    bool interactive = isatty(STDIN_FILENO);

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--verbose") verbose = true;
        if (std::string(argv[i]) == "--timing")  timing  = true;
    }

    ReplState state;
    // ... initialize arenas, intern, syms, eval ...

    if (interactive) {
        std::printf("rap — Relational Agenda Programming\n");
        std::printf("Type (run N (q) ...) to query, (defrel ...) to define.\n");
        std::printf("EOF (Ctrl-D) to exit.\n\n");
        std::printf("rap> ");
        std::flush(std::cout);
    }

    // accumulation loop ...
}
```

---

## Makefile Addition

```makefile
repl: repl.cpp core/sexp_parser.hpp core/core.hpp \
      core/intern.hpp core/arena.hpp core/mktypes.hpp
	$(CXX) $(CXXFLAGS) -o $@ $<
```

Add `repl` to `all`.

---

## Acceptance Criteria

- [ ] `repl.cpp` compiles cleanly with `-Werror`
- [ ] `./repl` shows prompt in interactive mode, no prompt when piped
- [ ] `(run N (q) ...)` forms execute and print results
- [ ] `(defrel ...)` forms accumulate across the session
- [ ] Defined relations are usable in subsequent queries
- [ ] `--verbose` prints parsed goal tree before running
- [ ] `--timing` prints µs after each query
- [ ] `./repl < somefile` works correctly
- [ ] Unbalanced `)` prints error and resets accumulator without exiting
- [ ] Parse errors print diagnostic and continue
- [ ] OOM prints message and exits with code 1
- [ ] EOF exits cleanly with code 0
- [ ] `make test` still passes (no regressions)
- [ ] `deep_copy_goal` implemented and used by `merge_rel_env`
- [ ] Parser extension: defrel-only input succeeds without error

---

## What This Is Not

- No readline, no history, no tab completion
- No RAP layer (`no-ops`, `cons-ops`, agenda)
- No multifile `load` or `include`
- No top-level `define` for non-relation values
- No error recovery beyond "skip this form and continue"

All of the above are reasonable future extensions. None are needed for MVP.

---

*v1.0 May 2026 — initial specification*
