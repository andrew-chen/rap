# RAP — Relational Agenda Programming

## What This Is
A C++20 logic programming engine targeting embedded systems.
Currently a flat directory; needs to be factored into layers.

## Intended Layer Structure

### core/ (target: standalone µKanren engine)
The minimal logic engine. No dependencies outside standard C++20.
Currently lives (entangled) in: arena.hpp, intern.hpp, core.hpp, sexp_parser.hpp

Belongs in core:
- Arena allocator (arena.hpp — probably already clean)
- Symbol interning (intern.hpp — probably already clean)
- S-expression parser (sexp_parser.hpp)
- µKanren: terms, substitutions, unification
- Goals: fresh, disj, conj
- Fair FIFO search
- Iterative execution (explicit stacks, no recursion)
- De Bruijn index compilation
- Probe goal (four-valued bounded meta-evaluation:
  True, False, Insufficient, Bounded)

### stdlib/ (planned, not yet implemented)
Standard relations built on core. Does not exist yet.

### security/ (planned, not yet implemented)
Workshop paper application: embedded security policy verification.
Will depend on core/ (and eventually stdlib/).
Must be fully independent of rap/.

### rap/ (planned, partially implemented)
Relational Agenda Programming extensions.
Will depend on core/ (and eventually stdlib/).
Must be fully independent of security/.

Currently implemented in core.hpp (entangled with core):
- Probe (belongs in core/, not here)

Not yet implemented (needed for RAP paper):
- Introspectable work queues (Queue 2 / agenda mechanism)
- Agenda-as-term representation
- ChangeSet machinery
- Reactive execution loop
- Output queue

## Current State
- parse_run.cpp: driver/test harness, not part of any layer
- Everything else is flat headers, needs factoring

## What Needs to Happen
Factor existing code into core/ subdirectory first.
Do not implement anything new.
Identify what in core.hpp belongs in core/ vs. is already
scaffolding for rap/ (note it but don't implement it).

