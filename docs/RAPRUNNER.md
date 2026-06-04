# raprunner Specification: Reactive RAP Program Runner

**Version:** 1.0  
**Depends on:** Stage 2 complete (rap/loop.hpp, rap/agenda.hpp, etc.)  
**New file:** `raprunner.cpp`  
**Status:** Specification — not yet implemented  
**Date:** May 2026

---

## Purpose

A command-line harness for running RAP programs. A RAP program is a file
of `defrel` definitions with two conventional entry points:

- **`main`** — called once at startup with CLI args as a term
- **`handle_input`** — enqueued when input arrives on a watched fd

The runner manages the reactive loop, watches file descriptors for input,
and enqueues queries as appropriate. The relational program has full
control over the agenda — it can reorder, remove, or transform pending
queries including pending input queries before they ever execute.

```bash
raprunner program.rap                    # stdin only
raprunner program.rap arg1 arg2          # CLI args passed to main
raprunner program.rap --fd 4 --fd 7      # watch additional fds
```

---

## Program File Format

A raprunner program file contains only `defrel` forms. There is no `run`
form — the runner provides the execution harness.

```scheme
; program.rap

(defrel (main args ops)
  ; args is a list of CLI arg symbols
  ; ops is the initial ChangeSet
  (call no-ops ops))   ; do nothing on startup

(defrel (handle_input agenda fd input ops)
  ; agenda is the current pending-query list
  ; fd is an integer term identifying the source
  ; input is the line read (as a symbol)
  ; ops is the ChangeSet to apply
  (call cons-ops (output input) (no-ops) ops))  ; echo input
```

Both `main` and `handle_input` must be defined. If either is missing,
raprunner prints an error and exits.

---

## Entry Point Conventions

### main

Called once after the program file is loaded. Receives CLI args as a list
of symbol terms. Returns a ChangeSet via its last argument.

Arity: `(main args ops)` — 2 parameters.

The initial ChangeSet from `main` is applied before the reactive loop
begins. This gives the program a chance to enqueue initial work.

### handle_input

Not called directly. When a watched fd has a line of input available,
raprunner constructs a `(handle_input agenda fd input ops)` query term
and **enqueues it** into the agenda. The reactive loop then executes it
when it reaches the front of the queue — which may be immediately or
after other pending queries run, depending on what the program has done
to the agenda.

Arity: `(handle_input agenda fd input ops)` — 4 parameters.

- `agenda` — the current agenda list term (snapshot at enqueue time)
- `fd` — `Term::integer(fd_number)` identifying the source
- `input` — the line read, interned as a symbol
- `ops` — unified with the resulting ChangeSet

**The fd integer is meaningful to the relational program.** A program
watching multiple streams can pattern-match on `fd` to dispatch different
behavior per source:

```scheme
(defrel (handle_input agenda fd input ops)
  (disj
    (conj (== fd 0) (call handle_stdin agenda input ops))
    (conj (== fd 4) (call handle_control agenda input ops))))
```

---

## Watched File Descriptors

By default, raprunner watches fd 0 (stdin).

Additional fds are specified with `--fd N` on the command line. It is the
caller's responsibility to ensure these fds are open and readable before
launching raprunner (e.g. opened by a parent process and inherited, or set
up via shell redirection).

raprunner never opens or closes fds itself (except stdin). It only watches
them via `poll`.

**EOF on a fd:** When `poll` indicates EOF on a watched fd, raprunner
removes it from the watch set. When all watched fds have reached EOF and
the agenda is empty, raprunner exits cleanly.

---

## The Reactive Loop

```
1. Load program file — parse all defrel forms into session rel_env
2. Call main(args, ops) — run it, apply resulting ChangeSet to agenda
3. Loop:
   a. While agenda is non-empty:
      - Dequeue front query
      - Build agenda snapshot term (spine arena)
      - Run query via RapEvaluator
      - Apply resulting ChangeSet
   b. When agenda is empty:
      - If no fds remain open: exit 0
      - poll(watched_fds, timeout=indefinite)
      - For each readable fd:
          read one line
          intern line as symbol
          build (handle_input agenda fd line ops) query term
          enqueue into agenda
      - If a fd reaches EOF: remove from watch set
      - Continue loop
```

**Key property:** `poll` is only called when the agenda is empty. The
program runs to quiescence before new input is considered. This preserves
the single-threaded invariant: the agenda is modified only between query
executions, never during one.

**Timeout:** `poll` blocks indefinitely when waiting for input. There is
no polling timeout in the MVP. A future version could support a
`(handle_timeout agenda ops)` convention for timer-driven behavior.

---

## Output

`output(Term)` ChangeSet operations print the term to stdout followed by
a newline, using `print_term`. No other output is produced by the runner
itself (errors go to stderr).

---

## Command-Line Interface

```
raprunner <program-file> [args...] [--fd N]...
```

- `<program-file>` — path to the `.rap` (or any extension) program file
- `[args...]` — zero or more arguments passed to `main` as a symbol list.
  Anything that is not `--fd N` is treated as an arg.
- `--fd N` — add fd N to the watch set. May be specified multiple times.

**Examples:**

```bash
raprunner echo.rap                        # echo server on stdin
raprunner chat.rap --fd 3 --fd 4          # watch two additional streams
raprunner init.rap setup production       # main gets (setup production)
```

---

## Memory Architecture

raprunner uses the same arena structure as `RapLoop` from Stage 2, with
one addition: a `prog_arena` for the program's parsed definitions.

```
prog_arena    — long-lived. Holds all defrel Goal trees loaded from file.
                Never reset. Sized for the program (128 KiB default).

intern_arena  — long-lived. Holds interned symbols across the session.
                Never reset.

eval_arena    — per-query. Reset after each query execution.

rap_arena     — permanent. Holds RapEvaluator's sym entries and client
                region. Never reset. (Same two-arena pattern as RapLoop.)
```

The `prog_arena` is separate from `intern_arena` because goal trees are
larger and have different lifetimes than interned symbols. Both are
long-lived, but keeping them separate makes sizing easier and prevents
symbol interning from competing with goal storage.

---

## Error Handling

| Condition | Behavior |
|---|---|
| Program file not found | Print error to stderr, exit 1 |
| Parse error in program file | Print diagnostic, exit 1 |
| `main` not defined | Print error to stderr, exit 1 |
| `handle_input` not defined | Print error to stderr, exit 1 |
| `main` produces no solution | Print error to stderr, exit 1 |
| Query produces no solution | Silent — empty ChangeSet, continue |
| OOM in prog_arena | Print error to stderr, exit 1 |
| OOM in eval_arena | Print error to stderr, exit 1 |
| `poll` error | Print errno to stderr, exit 1 |

---

## Loading the Program File

```cpp
bool load_program(const char* path, RapRunnerState& state) {
    // Read entire file into a buffer.
    // Parse all defrel forms using parse_query with defrel-only mode
    // (the extension added for the REPL).
    // Deep-copy all RelNode bodies into prog_arena.
    // Merge resulting rel_env into session rel_env.
    // Return false if any parse error occurs.
}
```

This reuses the defrel-only parse path added for the REPL
(`docs/REPL.md`). If the REPL has not yet been implemented, that parser
extension must be added first — it is a prerequisite.

---

## Building the Args Term

CLI args are collected into a list of interned symbols:

```cpp
Term build_args_term(Arena& arena, Intern& intern,
                     int argc, char** argv) {
    // argv already filtered to exclude --fd flags and program name.
    // Build (arg1 arg2 ...) as a Pair-list of Sym terms.
    Term result = Term::nil();
    for (int i = argc - 1; i >= 0; --i) {
        const SymEntry* s = intern_cstr(arena, intern, argv[i]);
        PairNode* p = arena.make<PairNode>();
        p->car = Term::symbol(s);
        p->cdr = result;
        result = Term::make_pair(p);
    }
    return result;
}
```

---

## Calling main

```cpp
// Build: (main args ops) as a Rel with param_count=2
// args is the CLI args list term
// ops is a fresh variable that will be unified with the ChangeSet

// Construct the goal: (call main-rel args ops-var)
// Run via RapEvaluator, collect ChangeSet from client region
// Apply ChangeSet to agenda
// If no solution: error and exit
```

`main` is looked up by name in `session.rel_env` after loading the
program file. If not found, exit with error.

---

## Building the handle_input Query Term

When a line is read from fd `n`:

```cpp
Term build_handle_input_query(Arena& arena, Intern& intern,
                               RelEnv& rel_env, Agenda& agenda,
                               SpineArena& spine,
                               int fd, const std::string& line) {
    // Look up handle_input relation
    const SymEntry* name = intern_cstr(arena, intern, "handle_input");
    Term rel_term = rel_env.lookup(name);
    // rel_term should be a Rel with param_count=4

    // Build args: agenda_term, fd_term, input_term
    Term agenda_term = agenda.as_term(spine.get());
    Term fd_term     = Term::integer(fd);
    const SymEntry* line_sym = intern_cstr(arena, intern, line.c_str());
    Term input_term  = Term::symbol(line_sym);

    // Return the Rel term itself — the execution loop calls it
    // with these args when it dequeues the entry.
    // [IMPL NOTE: The query is stored as the Rel term in the agenda.
    //  The execution loop passes the current agenda as the first arg.
    //  But handle_input takes 4 args (agenda, fd, input, ops).
    //  This requires wrapping fd and input with the Rel — see below.]
}
```

**Calling convention note:** Standard RAP queries are `Rel` with
`param_count=1` (agenda only). `handle_input` takes 4 parameters. The
solution is to wrap it in a 1-parameter anonymous relation at enqueue
time:

```scheme
; enqueued as:
(rel (ops) (call handle_input current-agenda fd-term input-term ops))
```

This wrapper captures `current-agenda`, `fd-term`, and `input-term` as
closed-over values at enqueue time. When executed, it receives the
*then-current* agenda as its single parameter — but `handle_input` sees
the *snapshot* agenda from enqueue time via the closed-over value.

**Design decision:** The snapshot agenda is passed to `handle_input`,
not the live agenda at execution time. This is consistent with the RAP
model — a query reasons over the agenda as it was when the query was
created, not as it is when it runs. If the program wants the live agenda,
it can use `membero` over the parameter passed to the wrapper.

[IMPL NOTE: Constructing this wrapper requires building a `RelNode` and
`Goal` tree programmatically in C++. Use the same primitives used
elsewhere in the codebase to construct goals. The wrapper body is a
single `Call` goal.]

---

## Makefile Addition

```makefile
raprunner: raprunner.cpp \
    rap/loop.hpp rap/agenda.hpp rap/spine.hpp rap/changeset.hpp \
    rap/rap.hpp core/sexp_parser.hpp core/core.hpp \
    core/intern.hpp core/arena.hpp core/mktypes.hpp
	$(CXX) $(CXXFLAGS) -o $@ $<
```

Add `raprunner` to `all`. Do not add to `make test` — raprunner is an
application, not a test binary.

---

## Example Program: Echo Server

```scheme
; examples/echo.rap

(defrel (main args ops)
  (call no-ops ops))

(defrel (handle_input agenda fd input ops)
  (call cons-ops (output input) (no-ops) ops))
```

Run with: `./raprunner examples/echo.rap`

Each line typed is echoed back via the output queue.

---

## Example Program: Line Counter

```scheme
; examples/counter.rap

(defrel (main args ops)
  ; Enqueue an initial count query
  (call cons-ops (add (count-query 0)) (no-ops) ops))

(defrel (handle_input agenda fd input ops)
  ; Find current count in agenda, increment it
  (fresh (n n1 rest)
    (== agenda ((count-query n) . rest))
    (call succ n n1)
    (conj
      (call cons-ops (remove-count-query n) (no-ops) ops0)
      (call cons-ops (add (count-query n1)) ops0 ops1)
      (call cons-ops (output (count n1)) ops1 ops))))
```

This example is intentionally incomplete (`succ`, `remove-count-query`
left as exercises) — it illustrates the pattern, not a working program.

---

## Prerequisites

The REPL spec (`docs/REPL.md`) must be implemented first, specifically:
- The defrel-only parse path in `sexp_parser.hpp`
- `deep_copy_goal` in `core/core.hpp`

These are reused directly by raprunner's program loader.

---

## Acceptance Criteria

- [ ] `raprunner.cpp` compiles cleanly with `-Werror`
- [ ] Program file with `main` and `handle_input` loads correctly
- [ ] Missing `main` or `handle_input` produces clear error and exit 1
- [ ] `main` ChangeSet is applied before the reactive loop begins
- [ ] stdin input enqueues `handle_input` query into agenda
- [ ] `output` operations from ChangeSets print to stdout
- [ ] EOF on stdin exits cleanly when agenda is empty
- [ ] `--fd N` adds additional watched fds
- [ ] EOF on non-stdin fd removes it from watch set
- [ ] Multiple fds: fd integer correctly identifies source in handle_input
- [ ] Echo example (`examples/echo.rap`) works correctly
- [ ] `make test` still passes (no regressions)
- [ ] `raprunner` added to `all` in Makefile

---

## What This Is Not

- No readline, no history
- No timeout/timer-driven queries (future: `handle_timeout`)
- No network sockets (future: opened externally, passed as --fd)
- No process management
- No persistent state across restarts
- No concurrency (single-threaded reactive loop)

---

*v1.0 May 2026 — initial specification*
