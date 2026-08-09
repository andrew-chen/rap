# examples/

This directory contains RAP programs organized into three categories.

---

## `demos/` — Interactive raprunner programs

Six standalone programs demonstrating the `raprunner` interface:

| File | What it shows |
|---|---|
| `hello.rap` | Minimal `main` → single Output; the simplest possible raprunner skeleton |
| `echo.rap` | `handle_input` receiving and re-outputting stdin lines |
| `celsius.rap` | Bidirectional temperature conversion; demonstrates relational bidirectionality |
| `piglatin.rap` | Character-level string manipulation and partial relational decomposition |
| `todo.rap` | Stateful agenda-based to-do list; the richest pure-demo program |
| `wc.rap` | Accumulator-style character/word/line counting via `handle_input` |

Run any of them from the project root:

```bash
echo "" | ./raprunner examples/demos/hello.rap
echo "hello world" | ./raprunner examples/demos/echo.rap
./raprunner examples/demos/celsius.rap    # then type temperature names
```

---

## `memory/` — The memory-game worked example

The paper's main running example: a relational memory-card game developed
stage by stage from a single board-solving query to a full adversarial game
loop with a pseudorandom antagonist. Each file builds directly on the previous
one.

### Narrative sequence

| File | Stage | What it introduces |
|---|---|---|
| `memory_stage0.rap` | 0 | REPL loader — the Stage 0 board relations (pipe into `./repl`) |
| `memory_stage1.rap` | 1 | Solve a fully-known board; no agenda |
| `memory_stage2_0.rap` | 2.0 | Belief representation only |
| `memory_stage2_1.rap` | 2.1 | Reveal one position from ground truth |
| `memory_stage2_2.rap` | 2.2 | Confirmed-match detection via `Probe`-based negation-as-failure |
| `memory_stage2_3.rap` | 2.3 | Belief-as-agenda-term, no mutation |
| `memory_stage2_4.rap` | 2.4 | Belief updates via real `cons-ops` ChangeSet |
| `memory_stage2_5.rap` | 2.5 | Attempt-set maintenance and new-pair scheduling |
| `memory_stage2_6.rap` | 2.6 | Full non-adversarial game loop with LCG-shuffled board |
| `memory_stage3_0.rap` | 3 | **Full adversarial game — fixed seed, zero dependencies** |
| `memory_stage3_0_with_args.rap` | 3 | Full adversarial game — seed and attempt-count from command-line args |

**`memory_stage3_0.rap` vs. `memory_stage3_0_with_args.rap`:**
These serve genuinely different purposes and are both kept.

- **`memory_stage3_0.rap`** uses a hardcoded seed and requires nothing beyond
  `raprunner` — consistent with the project's zero-external-dependencies claim.
  Reach for this file if you want to see the complete adversarial game run
  immediately: `echo "" | ./raprunner examples/memory/memory_stage3_0.rap`

- **`memory_stage3_0_with_args.rap`** reads its seed and attempt-count from
  command-line arguments, making it suitable for exploring multiple seeds or
  reproducing the timing analysis (which uses a separate Python timing script).
  Reach for this file if you want to vary the seed or integrate with the timing
  harness.

### `component_tests/` — Isolated component tests

Raprunner programs that verify individual relations from the memory-game
implementation in isolation, one relation (or small group) per file. Each was
built during development of the corresponding stage and tested the piece before
it was wired into the full game loop.

These are not interactive demos — each runs headlessly with `< /dev/null` and
produces deterministic output. They are the unverified precursors to the
machine-checked `tests/` suite at the project root: adding `;;; EXPECT` markers
to these files would make the complete relation ladder machine-checked (see
`docs/ROADMAP.md`).

> **Note on `test_find_new_matches_collecto.rap` and `memory_stage2_6_oddfix.rap`:**
> Both were placed here pending a deferred decision. `test_find_new_matches_collecto.rap`
> may be superseded by `test_find_new_matches.rap` and `test_new_find_matches.rap`.
> `memory_stage2_6_oddfix.rap` is structurally a full game program but its stated
> purpose is verification (confirming an odd-count-safe replacement is inert in the
> non-adversarial game). Both dispositions are deferred.

---

## Other files at this level

Files left at the `examples/` root are either deferred-decision items or
deliberately out-of-scope for the current reorganization:

- **`prng.rap`** — LCG PRNG implementation; illustrative standalone demo and
  the library used by the Stage 3 game. Has `main`/`handle_input` so it is
  runnable under `raprunner`, but its primary character is illustrative.
- **`stdlib_noto_proposal.rap`** — Proposed `noto` (negation-as-failure)
  addition to `stdlib/core.rap`. Placement (`examples/` vs. `docs/`) is deferred.
- **`test_groundo.rap`** — Tests a general stdlib bug fix (`groundo`), not a
  memory-game component. Disposition deferred.
- **`test_guide_claims.rap`** — Validates claims in `docs/RAP_PROGRAMMING_GUIDE.md`.
  Disposition deferred.
- Other files (`hello2.rap`, `memory_stage1-error-output.txt`,
  `memory_stage3_0_classic.rap`, `old/`) — Flagged for eventual deletion in the
  public-readiness audit; deletion deferred.
