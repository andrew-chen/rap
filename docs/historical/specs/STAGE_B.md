# STAGE_B: Block Input, Stdlib, and Revised Examples

**Version:** 1.0  
**Depends on:** STAGE_ARITH complete, REPL complete, raprunner complete  
**Modifies:** `raprunner.cpp`, `repl.cpp`  
**New files:** `stdlib/core.rap`, `examples/hello.rap`, `examples/echo.rap`,
              `examples/piglatin.rap`, `examples/celsius.rap`,
              `examples/todo.rap`, `examples/wc.rap`  
**Status:** Specification — not yet implemented  
**Date:** May 2026

---

## Overview

Three coordinated changes:

1. **Block-based character list input in raprunner** — replace line-based
   `getline` with `read`/`poll` producing character list terms. Newlines
   are characters like any other. No stripping.

2. **stdlib/core.rap** — a file of `defrel` wrappers over built-in
   relations, loaded automatically by both raprunner and the REPL before
   any user code.

3. **Revised examples** — all examples rewritten for character-list
   input. New `wc.rap` example demonstrating `charo` and character
   classification.

---

## Part 1: Block-Based Input in raprunner

### Current behavior (to be replaced)

raprunner currently reads one line at a time using `std::getline`,
strips the trailing newline, and interns the whole line as a single
symbol. This is wrong for two reasons:

1. Input is not necessarily line-oriented (pipes, blocks, pages).
2. The whole line as one symbol prevents relational programs from
   reasoning about individual characters.

### New behavior

When `poll` indicates a file descriptor is readable, raprunner calls
`read(fd, buf, BLOCK_SIZE)` to get however many bytes are available
(up to `BLOCK_SIZE = 4096`). It then builds a **list of
single-character symbols** from those bytes and enqueues a
`handle_input` query with that list as the `input` argument.

Each byte becomes a single-character symbol via `charo`'s interning
convention: `char buf2[2] = { byte, '\0' }; intern_cstr(arena, intern, buf2)`.

An empty read (`read` returns 0) signals EOF on that fd.

A partial read is fine — if only 3 bytes are available, the input list
has 3 elements. The relational program is responsible for buffering if
it needs complete lines.

### Character list construction

```cpp
Term build_char_list(Arena& arena, Intern& intern,
                     const char* buf, ssize_t len) {
    Term result = Term::nil();
    // Build list back-to-front for correct order.
    for (ssize_t i = len - 1; i >= 0; --i) {
        char cbuf[2] = { buf[i], '\0' };
        const SymEntry* s = intern_cstr(arena, intern, cbuf);
        PairNode* p = arena.make<PairNode>();
        if (!p) return Term::nil();  // OOM — return partial list
        p->car = Term::symbol(s);
        p->cdr = result;
        result = Term::make_pair(p);
    }
    return result;
}
```

Replace the current line-reading and symbol-interning code in the
poll loop with a call to `build_char_list` using the bytes from
`read()`.

### EOF behavior

When `read()` returns 0 on a watched fd:
- Remove the fd from the watch set.
- Enqueue a `handle_input` query with `input = ()` (empty list) to
  signal EOF to the relational program.
- The program can pattern-match on `(== input ())` to detect EOF.

When all fds have been closed and the agenda is empty, exit 0.

### REPL input model

The REPL is interactive and line-oriented by design — users type
complete expressions and press Enter. The REPL keeps its current
`getline`-based accumulator. The REPL does not use `build_char_list`.

Character list input applies only to raprunner.

---

## Part 2: stdlib/core.rap

### Location and loading

Create `stdlib/core.rap`. Both raprunner and the REPL load it
automatically before any user code:

- **raprunner:** load `stdlib/core.rap` as the first file, before
  loading the user's program file. Use the same `load_program`
  mechanism already in place.
- **REPL:** at startup, before showing the prompt, call
  `load_file("stdlib/core.rap")` into the session `rel_env`. If the
  file is not found, print a warning but continue — stdlib is optional.

The path is resolved relative to the raprunner/repl binary location,
or from an environment variable `RAP_STDLIB` if set. For simplicity
in the MVP, use a hardcoded relative path `../stdlib/core.rap` from
the binary location, and fall back to `stdlib/core.rap` from the
current working directory.

### Contents of stdlib/core.rap

```scheme
; stdlib/core.rap — standard library for RAP
; Loaded automatically by raprunner and the REPL.

;; Arithmetic convenience wrappers

; (addo a b c) — a + b = c
(defrel (addo a b c)
  (addsubo a b c))

; (subo a b c) — a - b = c  (i.e. a = c + b)
(defrel (subo a b c)
  (addsubo c b a))

; (mulo a b c) — a * b = c
(defrel (mulo a b c)
  (multaddiso a b 0 c))

; (divo a b q r) — a = b*q + r, with 0 <= r < b
(defrel (divo a b q r)
  (multaddiso q b r a)
  (leqo 0 r)
  (lto r b))

; (modo a b r) — a mod b = r
(defrel (modo a b r)
  (fresh (q)
    (divo a b q r)))

;; Numeric predicates

; (zeroo n) — n == 0
(defrel (zeroo n)
  (eqo n 0))

; (poso n) — n > 0
(defrel (poso n)
  (gto n 0))

; (nego n) — n < 0
(defrel (nego n)
  (lto n 0))

;; Character classification
;; These use charo and leqo to classify characters by ASCII range.

; (digitcharo c) — c is a digit character (0-9, ASCII 48-57)
(defrel (digitcharo c)
  (fresh (n)
    (charo c n)
    (leqo 48 n)
    (leqo n 57)))

; (alphacharo c) — c is an alphabetic character (a-z or A-Z)
(defrel (alphacharo c)
  (fresh (n)
    (charo c n)
    (disj
      (conj (leqo 65 n) (leqo n 90))   ; A-Z
      (conj (leqo 97 n) (leqo n 122))))) ; a-z

; (lowercaseo c) — c is a lowercase letter (a-z, ASCII 97-122)
(defrel (lowercaseo c)
  (fresh (n)
    (charo c n)
    (leqo 97 n)
    (leqo n 122)))

; (uppercaseo c) — c is an uppercase letter (A-Z, ASCII 65-90)
(defrel (uppercaseo c)
  (fresh (n)
    (charo c n)
    (leqo 65 n)
    (leqo n 90)))

; (spacecharo c) — c is a whitespace character (space=32, tab=9, newline=10)
(defrel (spacecharo c)
  (disj
    (charo c 32)
    (charo c 9)
    (charo c 10)))

; (printablecharo c) — c is a printable ASCII character (32-126)
(defrel (printablecharo c)
  (fresh (n)
    (charo c n)
    (leqo 32 n)
    (leqo n 126)))

;; List utilities

; (lengtho lst n) — lst has length n
(defrel (lengtho lst n)
  (disj
    (conj (== lst ()) (eqo n 0))
    (fresh (h t n1)
      (conj
        (== lst (h . t))
        (addo n1 1 n)
        (lengtho t n1)))))

; (membero x lst) — x is a member of lst
(defrel (membero x lst)
  (disj
    (fresh (rest) (== lst (x . rest)))
    (fresh (h rest)
      (conj
        (== lst (h . rest))
        (membero x rest)))))

; (appendo l r out) — l ++ r = out
(defrel (appendo l r out)
  (disj
    (conj (== l ()) (== out r))
    (fresh (h t res)
      (conj
        (== l (h . t))
        (== out (h . res))
        (appendo t r res)))))

; (reverseo lst rev) — rev is the reverse of lst
(defrel (reverseo lst rev)
  (reverseo-acc lst () rev))

(defrel (reverseo-acc lst acc rev)
  (disj
    (conj (== lst ()) (== acc rev))
    (fresh (h t)
      (conj
        (== lst (h . t))
        (reverseo-acc t (h . acc) rev)))))

; (takeo n lst out) — out is the first n elements of lst
(defrel (takeo n lst out)
  (disj
    (conj (eqo n 0) (== out ()))
    (fresh (h t out-rest n1)
      (conj
        (== lst (h . t))
        (== out (h . out-rest))
        (addo n1 1 n)
        (takeo n1 t out-rest)))))
```

---

## Part 3: Revised Examples

All examples use character-list input. Each is self-contained and
does not depend on stdlib beyond what is loaded automatically.

Since stdlib is loaded automatically, examples may use `membero`,
`appendo`, `addo`, `digitcharo`, `spacecharo`, etc. without
re-defining them.

### examples/hello.rap

```scheme
; examples/hello.rap — hello world
; Outputs (h e l l o - w o r l d) and does nothing with input.
; Run: ./raprunner examples/hello.rap < /dev/null

(defrel (main args ops)
  (call cons-ops
    (output (h e l l o - w o r l d))
    (no-ops)
    ops))

(defrel (handle_input agenda fd input ops)
  (call no-ops ops))
```

### examples/echo.rap

```scheme
; examples/echo.rap — echo server
; Each block of input is echoed back as a character list term.
; Run: ./raprunner examples/echo.rap

(defrel (main args ops)
  (call no-ops ops))

(defrel (handle_input agenda fd input ops)
  (call cons-ops (output input) (no-ops) ops))
```

### examples/wc.rap

```scheme
; examples/wc.rap — character, word, and line counter
; Demonstrates: charo, character classification, stateful agenda.
; State is kept as (wc-state chars words lines in-word) in the agenda.
;
; Run: ./raprunner examples/wc.rap < somefile.txt

; Count one character: update chars, words, lines, in-word flag
(defrel (count-charo c chars words lines in-word
                       chars1 words1 lines1 in-word1)
  (addo chars 1 chars1)
  (disj
    ; newline (ASCII 10): increment lines, end word
    (conj
      (charo c 10)
      (addo lines 1 lines1)
      (== words1 words)
      (== in-word1 0))
    ; space or tab: end word, no line increment
    (conj
      (spacecharo c)
      (=/= c |newline-handled-above|)
      (== lines1 lines)
      (== words1 words)
      (== in-word1 0))
    ; non-space: if starting a new word, increment words
    (conj
      (call printablecharo c)
      (call alphacharo c)
      (== lines1 lines)
      (disj
        (conj (eqo in-word 0) (addo words 1 words1) (== in-word1 1))
        (conj (eqo in-word 1) (== words1 words)     (== in-word1 1))))))

; Process a list of characters, updating state
(defrel (count-charo-listo input chars words lines in-word
                              chars1 words1 lines1 in-word1)
  (disj
    (conj
      (== input ())
      (== chars1 chars)
      (== words1 words)
      (== lines1 lines)
      (== in-word1 in-word))
    (fresh (c rest c2 w2 l2 iw2)
      (conj
        (== input (c . rest))
        (count-charo c chars words lines in-word c2 w2 l2 iw2)
        (count-charo-listo rest c2 w2 l2 iw2
                            chars1 words1 lines1 in-word1)))))

(defrel (main args ops)
  ; Initial state: 0 chars, 0 words, 0 lines, not in a word
  (call cons-ops (add (wc-state 0 0 0 0)) (no-ops) ops))

(defrel (handle_input agenda fd input ops)
  (disj
    ; EOF: output the final counts
    (conj
      (== input ())
      (fresh (chars words lines iw)
        (membero (wc-state chars words lines iw) agenda)
        (call cons-ops
          (output (chars chars words words lines lines))
          (no-ops)
          ops)))
    ; Data: update state
    (fresh (chars words lines iw c2 w2 l2 iw2 ops0)
      (conj
        (=/= input ())
        (membero (wc-state chars words lines iw) agenda)
        (count-charo-listo input chars words lines iw c2 w2 l2 iw2)
        (call cons-ops (remove (wc-state chars words lines iw)) (no-ops) ops0)
        (call cons-ops (add (wc-state c2 w2 l2 iw2)) ops0 ops)))))
```

**Note:** The `count-charo` relation above is a simplification —
in particular the space/newline distinction needs care since
`spacecharo` includes newlines. Claude Code should simplify the
character classification logic using the stdlib relations and test
it works. The intent is to demonstrate stateful agenda reasoning
with character classification, not to provide a production `wc`.

### examples/piglatin.rap

```scheme
; examples/piglatin.rap — pig latin translator
; Input is a character list. Words are sequences of alpha characters.
; Pig latin: move first letter to end, append (a y).
;
; Commands (as character lists):
;   t o SPACE l e t t e r s NEWLINE  — translate to pig latin
;   f r o m SPACE l e t t e r s NEWLINE — translate from pig latin
;
; Run: ./raprunner examples/piglatin.rap

; Split a char list into head chars matching pred and a tail
(defrel (span-alphao lst alpha rest)
  (disj
    (conj (== lst ()) (== alpha ()) (== rest ()))
    (fresh (c t alpha-rest)
      (conj
        (== lst (c . t))
        (alphacharo c)
        (span-alphao t alpha-rest rest)
        (== alpha (c . alpha-rest))))
    (fresh (c t)
      (conj
        (== lst (c . t))
        (call neqo-alphao c)
        (== alpha ())
        (== rest lst)))))

(defrel (neqo-alphao c)
  (fresh (n)
    (charo c n)
    (disj
      (lto n 65)
      (conj (gto n 90) (lto n 97))
      (gto n 122))))

; pig-latino: first-letter moved to end, then (a y) appended
(defrel (pig-latino word piglatin)
  (fresh (first rest rotated)
    (== word (first . rest))
    (appendo rest (first . ()) rotated)
    (appendo rotated (a . (y . ())) piglatin)))

; Parse command: prefix chars before first space
(defrel (parse-cmdo input cmd arg)
  (fresh (space-and-rest)
    (appendo cmd space-and-rest input)
    (== space-and-rest (32-space . arg))))  ; space = ASCII 32

(defrel (main args ops)
  (call cons-ops (output pig-latin-ready) (no-ops) ops))

(defrel (handle_input agenda fd input ops)
  (disj
    ; to <word>: translate to pig latin
    (fresh (word pig)
      (conj
        (== input (t . (o . (32 . word))))
        (span-alphao word pig ())
        (call pig-latino pig pig-result)
        (call cons-ops (output (pig-latin pig-result)) (no-ops) ops)))
    ; from <word>: recover original
    (fresh (pig original)
      (conj
        (== input (f . (r . (o . (m . (32 . pig))))))
        (span-alphao pig pig-clean ())
        (call pig-latino original pig-clean)
        (call cons-ops (output (original original)) (no-ops) ops)))
    ; unknown
    (fresh (head tail)
      (conj
        (== input (head . tail))
        (=/= head t)
        (=/= head f)
        (call cons-ops (output unknown-command) (no-ops) ops)))))
```

**Note:** The character matching above uses integer values for
space (32). Claude Code should use `(charo space-char 32)` style
for clarity, or use stdlib's `spacecharo`. The intent of this
example is to show character-level pattern matching — the exact
implementation details should be cleaned up to use stdlib
consistently.

### examples/celsius.rap

```scheme
; examples/celsius.rap — temperature converter
; Demonstrates: bidirectional relations, character-list command parsing.
; Input: character lists. Commands are word-level after splitting on spaces.
;
; Conversion table: named temperature <-> Fahrenheit integer
; Run: ./raprunner examples/celsius.rap

(defrel (tempo name f)
  (disj
    (conj (== name freezing) (eqo f 32))
    (conj (== name body)     (eqo f 98))
    (conj (== name boiling)  (eqo f 212))
    (conj (== name cold)     (eqo f 39))
    (conj (== name warm)     (eqo f 77))
    (conj (== name hot)      (eqo f 104))))

; Parse a word (sequence of alpha chars) from a char list
(defrel (word-charo chars word rest)
  (disj
    (conj (== chars ()) (== word ()) (== rest ()))
    (fresh (c t word-rest)
      (conj
        (== chars (c . t))
        (alphacharo c)
        (word-charo t word-rest rest)
        (== word (c . word-rest))))
    (fresh (c t)
      (conj
        (== chars (c . t))
        (call non-alphao c)
        (== word ())
        (== rest chars)))))

(defrel (non-alphao c)
  (fresh (n)
    (charo c n)
    (disj (lto n 65)
          (conj (gto n 90) (lto n 97))
          (gto n 122))))

(defrel (main args ops)
  (call cons-ops (output temperature-converter-ready) (no-ops) ops))

(defrel (handle_input agenda fd input ops)
  (disj
    ; Input starts with 'c': convert Celsius name to Fahrenheit
    (fresh (rest name-chars f)
      (conj
        (== input (c . rest))
        (word-charo rest name-chars ())
        (== name name-chars)
        (tempo name f)
        (call cons-ops (output (celsius name is-fahrenheit f)) (no-ops) ops)))
    ; Input starts with 'f': convert Fahrenheit integer name to Celsius name
    (fresh (rest val-chars c-name)
      (conj
        (== input (f . rest))
        (word-charo rest val-chars ())
        (== val val-chars)
        (tempo c-name val)
        (call cons-ops (output (fahrenheit val is-celsius c-name)) (no-ops) ops)))
    ; help or unknown
    (fresh (head tail)
      (conj
        (== input (head . tail))
        (=/= head c)
        (=/= head f)
        (call cons-ops (output (usage c-name f-name)) (no-ops) ops)))))
```

**Note:** The temperature names in this example are atom symbols
(`freezing`, `body`, etc.) not character lists. The `tempo` relation
maps between symbolic names and integer Fahrenheit values and runs
bidirectionally. The input parsing extracts the command character and
the argument. Claude Code should clean up the `val` / `val-chars`
distinction and make the fahrenheit lookup work correctly.

### examples/todo.rap

```scheme
; examples/todo.rap — interactive to-do list
; Demonstrates: stateful agenda, character-list command parsing,
; add/remove ChangeSet operations.
;
; Commands (as character lists):
;   a d d SPACE <item-chars> NEWLINE  — add item
;   d o n e SPACE <item-chars> NEWLINE — mark done
;   l i s t NEWLINE                    — list all items
;
; Items are stored as (todo <char-list>) in the agenda.
; Run: ./raprunner examples/todo.rap

; Split input into command word and argument chars
(defrel (split-cmdo input cmd-chars arg-chars)
  (fresh (space-rest)
    (appendo cmd-chars (32 . arg-chars) input)))

; Collect all todo items from the agenda
(defrel (collect-todoso agenda items)
  (disj
    (conj (== agenda ()) (== items ()))
    (fresh (item rest tail)
      (conj
        (== agenda ((todo item) . rest))
        (collect-todoso rest tail)
        (== items (item . tail))))
    (fresh (other rest)
      (conj
        (== agenda (other . rest))
        (fresh (x) (=/= other (todo x)))
        (collect-todoso rest items)))))

; Output each item in a list
(defrel (output-eacho items ops-in ops-out)
  (disj
    (conj (== items ()) (== ops-in ops-out))
    (fresh (item rest ops-mid)
      (conj
        (== items (item . rest))
        (call cons-ops (output (todo item)) ops-in ops-mid)
        (output-eacho rest ops-mid ops-out)))))

; Match a specific command prefix
(defrel (cmd-addo input arg)
  ; "add " prefix = (a d d 32 ...)
  (== input (a . (d . (d . (32 . arg))))))

(defrel (cmd-doneo input arg)
  ; "done " prefix = (d o n e 32 ...)
  (== input (d . (o . (n . (e . (32 . arg)))))))

(defrel (cmd-listo input)
  ; "list" = (l i s t ...)
  (== input (l . (i . (s . (t . _))))))

(defrel (main args ops)
  (call cons-ops (output todo-ready) (no-ops) ops))

(defrel (handle_input agenda fd input ops)
  (disj
    ; add <item>
    (fresh (arg ops0)
      (conj
        (cmd-addo input arg)
        (call cons-ops (add (todo arg)) (no-ops) ops0)
        (call cons-ops (output (added arg)) ops0 ops)))
    ; done <item>
    (fresh (arg ops0)
      (conj
        (cmd-doneo input arg)
        (membero (todo arg) agenda)
        (call cons-ops (remove (todo arg)) (no-ops) ops0)
        (call cons-ops (output (removed arg)) ops0 ops)))
    ; list
    (fresh (items ops0)
      (conj
        (cmd-listo input)
        (collect-todoso agenda items)
        (output-eacho items (no-ops) ops0)
        (== ops ops0)))
    ; unknown
    (fresh (head tail)
      (conj
        (== input (head . tail))
        (fresh (a1 a2) (=/= input (a . (d . a1))))
        (fresh (d1 d2) (=/= input (d . (o . d1))))
        (fresh (l1)    (=/= input (l . (i . l1))))
        (call cons-ops (output unknown-command) (no-ops) ops)))))
```

**Note:** The command matching uses character-level pattern matching
directly on the input list. The `_` wildcard in `cmd-listo` may need
to be replaced with a fresh variable. Claude Code should clean up
the unknown branch guard pattern using the structural decomposition
idiom from `not-weak-check-qido` (bind the head, then disequate).

---

## Changes to raprunner.cpp

Replace the poll loop's input handling section:

```cpp
// Old: read line, intern as symbol
std::string line;
std::getline(stream, line);
Term input_term = Term::symbol(intern_cstr(..., line.c_str()));

// New: read block, build char list
char block[4096];
ssize_t n = read(fd, block, sizeof(block));
if (n == 0) {
    // EOF: enqueue handle_input with empty list
    Term input_term = Term::nil();
    // enqueue handle_input query...
    remove_fd_from_watch_set(fd);
} else if (n > 0) {
    Term input_term = build_char_list(eval_arena, intern, block, n);
    // enqueue handle_input query...
}
```

Add `build_char_list` as a free function in `raprunner.cpp` as
specified in Part 1.

---

## Changes to repl.cpp and raprunner.cpp for stdlib loading

Add a `load_stdlib` function:

```cpp
bool load_stdlib(ReplState& state) {
    // Try paths in order:
    // 1. $RAP_STDLIB environment variable
    // 2. ../stdlib/core.rap relative to argv[0]
    // 3. stdlib/core.rap relative to cwd
    const char* env_path = std::getenv("RAP_STDLIB");
    std::vector<std::string> candidates;
    if (env_path) candidates.push_back(env_path);
    candidates.push_back("../stdlib/core.rap");
    candidates.push_back("stdlib/core.rap");

    for (const auto& path : candidates) {
        if (file_exists(path)) {
            return load_file(path, state);
        }
    }
    // stdlib not found — warn but continue
    std::fprintf(stderr, "warning: stdlib/core.rap not found; "
                         "standard relations unavailable\n");
    return true;  // non-fatal
}
```

Call `load_stdlib` in both the REPL `main()` and raprunner's
initialization, before any user code is loaded.

---

## Makefile

No new targets needed. The examples are `.rap` files run by
`raprunner`. `stdlib/core.rap` is a data file, not a compiled target.

Consider adding a `make examples` target that runs each example with
a trivial input as a smoke test:

```makefile
examples: raprunner repl
	echo "" | ./raprunner examples/hello.rap
	echo "hello world" | ./raprunner examples/echo.rap
	echo "" | ./raprunner examples/wc.rap
	@echo "Examples smoke test passed."
```

---

## Acceptance Criteria

- [ ] `build_char_list` implemented in `raprunner.cpp`
- [ ] raprunner poll loop uses `read()` + `build_char_list`
- [ ] EOF enqueues `handle_input` with empty list `()`
- [ ] `stdlib/core.rap` created with all wrappers listed above
- [ ] `load_stdlib` added to both `repl.cpp` and `raprunner.cpp`
- [ ] stdlib loaded before user code in both tools
- [ ] `RAP_STDLIB` environment variable respected
- [ ] `examples/hello.rap` updated
- [ ] `examples/echo.rap` updated (character list output)
- [ ] `examples/wc.rap` created
- [ ] `examples/piglatin.rap` rewritten for character-list input
- [ ] `examples/celsius.rap` rewritten for character-list input
- [ ] `examples/todo.rap` rewritten for character-list input
- [ ] `make test` still passes
- [ ] `make examples` smoke test passes (or equivalent manual test)
- [ ] Echo: `echo "hello" | ./raprunner examples/echo.rap`
      outputs the character list `(h e l l o \n)` or similar
- [ ] Pig latin: typing `to hello` translates correctly
- [ ] REPL: stdlib relations available at startup

---

## Known Limitations (document, don't fix)

- raprunner input is block-based: programs wanting line-oriented
  input must buffer characters and split on newlines themselves.
  A `stdlib/lines.rap` providing line-buffering defrels is future work.
- `wc.rap` is a demonstration, not a production utility.
- The todo and pig latin examples use symbol atoms for names/commands;
  full character-level text matching would require more infrastructure.

---

*v1.0 May 2026 — initial specification*

