# docs/historical/ — Internal Development History

This directory contains files kept for historical reference that are **not**
current reference documentation. A newcomer reading the repository does not
need to read anything here to understand the codebase.

## Contents

### `specs/` — Implementation stage specs and bug-fix records

Design specs and bug reports from specific development phases:

- `STAGE_B.md`, `STAGE_B_FIX.md`, `STAGE_B_AFTER_FIX.md`, `STAGE_B_AFTER_FIX_2.md` —
  Multiple iterative passes on a specific correctness fix during Stage 2
  development. Kept as a record of how the fix was reasoned through.
- `KNOWN_FIX_1.md` — Bug report and fix spec for the GoalTag::Fresh env-leak
  issue, resolved during Stage B development.
- `CHANGESET_BACKTRACK_FIX.md` — Spec for the ChangeSet backtrack-safety fix.
  Referenced by `docs/formal-semantics.md` Section 6 notes.

The *current* stage specs (STAGE_0A, STAGE_0B, STAGE_0C, STAGE_2, STAGE_ARITH,
STAGE_DISEQ) remain in `docs/` as reference documentation for contributors.

### `ai-artifacts/` — AI-interaction prompt files

Internal prompts used during development to generate other docs:

- `rap-formal-semantics-spec.md` — Task spec that produced `docs/formal-semantics.md`.
- `meta-prompt-write-fix-spec.md` — Task spec used during Stage B debugging.

These are kept for completeness but are not human-authored design documents.
