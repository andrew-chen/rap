# Stage 2 Specification: Work Queue and ChangeSet Machinery

**Version:** 1.1  
**Depends on:** Stage 0C complete (docs/STAGE_0C.md)  
**Required before:** Paper case study 5.2 validation  
**Status:** Specification — not yet implemented  
**Date:** May 8, 2026

### Changelog from v1.0
- Replaced memmove-based compaction with pointer-rewriting copy (memmove
  is incorrect because PairNode* pointers would become invalid after move)
- Fixed `handle_no_ops`: use `sym_empty_ops_` member instead of
  `intern_init_ptr_` (which was never defined)
- Added construction-time interning of `"empty-ops"` symbol in RapEvaluator
- Added note on OpsOut unification approach for cons-ops

---

## Purpose

Stage 2 implements the full RAP execution model: Queue 2 (the agenda),
ChangeSet construction, the reactive execution loop, and the output queue.
This is the core contribution of the RAP paper beyond what already exists.

When Stage 2 is complete:

- The `RapEvaluator` stubs for `no-ops` and `cons-ops` are replaced with
  real ChangeSet construction
- Queue 2 exists as a ring-buffer arena with bump allocation and
  pointer-rewriting compaction
- The reactive execution loop drives queries from Queue 2 through the
  evaluator and applies ChangeSets
- The agenda is passed as a relational term to each query
- Queries can inspect and modify pending work via ChangeSet operations
- The paper's Section 5.2 case study (`strengthen-agendao`) runs and
  produces correct results
- `make test` still passes

---

## Overview of New Files

```
rap/
├── changeset.hpp     # ChangeSet: Op array + arena, deep_copy_term
├── agenda.hpp        # Queue 2: ring-buffer arena, query term storage
├── spine.hpp         # Spine arena: short-lived Pair nodes for agenda term
├── loop.hpp          # Reactive execution loop
└── rap.hpp           # Updated: real no-ops/cons-ops replacing stubs
```

All new files are header-only. All types trivially destructible.
No dynamic allocation beyond the fixed arenas.

---

## The Core Primitive: deep_copy_term

`deep_copy_term` is the fundamental operation used throughout Stage 2:
- Copying terms into the ChangeSet arena (copy-in from query execution)
- Copying query terms into the agenda arena (enqueue)
- Compaction after Remove (copy surviving entries forward, rewriting pointers)

All three are the same operation with different source/destination arenas.
Implement it once in `rap/changeset.hpp` and use it everywhere.

```cpp
// Deep-copy a term into dest_arena, rewriting all interior PairNode*
// pointers to point into dest_arena.
// Sym pointers are shared (interned symbols are stable — no copy needed).
// RelNode bodies are NOT copied (they live in parse-time arena, stable).
inline Term deep_copy_term(Arena& dest, Term t) {
    switch (t.tag) {
        case TermTag::Nil:
        case TermTag::Int:
        case TermTag::Sym:
        case TermTag::Var:
        case TermTag::BVar:
        case TermTag::Rel:
            return t;  // no heap pointers to rewrite

        case TermTag::Pair: {
            if (!t.pair) return t;
            PairNode* p = dest.make<PairNode>();
            if (!p) return Term::nil();  // OOM
            p->car = deep_copy_term(dest, t.pair->car);
            p->cdr = deep_copy_term(dest, t.pair->cdr);
            return Term::make_pair(p);
        }

        default:
            return Term::nil();
    }
}
```

**Note on recursion:** This function is recursive over term structure.
For the paper's example programs, term depth is small (tens of levels).
The recursive version is correct and simpler than an iterative version.

---

## Part 1: ChangeSet

Implement `rap/changeset.hpp` first.

### Op Types

```cpp
// rap/changeset.hpp
#pragma once
#include "../core/mktypes.hpp"
#include "../core/core.hpp"

enum class OpTag : uint8_t {
    Add    = 0,  // add(QueryTerm)   — enqueue a query
    Remove = 1,  // remove(QueryId)  — remove pending query by ID
    Output = 2,  // output(Term)     — append to output queue
};

struct Op {
    OpTag tag;
    union {
        Term     query_term;   // Add
        uint32_t query_id;     // Remove
        Term     output_term;  // Output
    };
};

static_assert(std::is_trivially_destructible_v<Op>);
```

### ChangeSet Struct

```cpp
struct ChangeSet {
    Op       ops[MAX_CHANGESET_OPS];
    uint32_t op_count = 0;
    uint8_t  arena_buf[MAX_CHANGESET_ARENA];
    Arena    arena;

    ChangeSet() : arena(arena_buf, MAX_CHANGESET_ARENA) {}

    void reset() {
        op_count = 0;
        arena.reset();
    }

    bool push(Op op) {
        if (op_count >= MAX_CHANGESET_OPS) return false;
        ops[op_count++] = op;
        return true;
    }
};

static_assert(std::is_trivially_destructible_v<ChangeSet>);
```

---

## Part 2: Queue 2 (Agenda)

Implement `rap/agenda.hpp`.

### Design

Queue 2 is a fixed-size buffer acting as a circular FIFO:

- **Enqueue:** bump `head` forward, copy term into new slot
- **Dequeue:** bump `tail` forward (FIFO)
- **Remove by ID:** find entry, copy all subsequent entries forward with
  pointer rewriting (NOT memmove — pointers must be rewritten), adjust `head`
- **OOM:** when `head` would reach `tail`

**Why no memmove:** Query terms are `PairNode*`-linked structures. After
a raw `memmove`, the interior pointers still point to the old addresses.
The correct approach is to copy each surviving entry forward using
`deep_copy_term` for the query term — the same operation used everywhere
else in Stage 2. This rewrites all interior pointers correctly. It is
less efficient than `memmove` but still cache-friendly (sequential read,
sequential write into adjacent memory), and correctness is required.

### QueryEntry

```cpp
// rap/agenda.hpp
#pragma once
#include "../core/mktypes.hpp"
#include "../core/core.hpp"
#include "changeset.hpp"

constexpr uint32_t QUEUE2_ARENA_SIZE = 64 * 1024;  // 64 KiB, paper-scale

struct QueryEntry {
    uint32_t id;          // stable ID for Remove ops
    uint32_t byte_size;   // total bytes this entry occupies in the buffer
                          // (header + all term data reachable from query_term)
    Term     query_term;  // the query to execute (Pair nodes in this buffer)
};

static_assert(std::is_trivially_destructible_v<QueryEntry>);
```

### Agenda Struct

```cpp
struct Agenda {
    uint8_t  buf[QUEUE2_ARENA_SIZE];
    uint32_t head     = 0;  // next write position
    uint32_t tail     = 0;  // oldest unread entry
    uint32_t count    = 0;  // number of pending entries
    uint32_t next_id  = 1;  // monotonically increasing

    bool empty() const { return count == 0; }

    // Enqueue a query term. Deep-copies into this buffer.
    // Returns assigned query_id, or 0 on OOM.
    uint32_t enqueue(Term query_term) {
        if (count == 0) { head = 0; tail = 0; }  // reset on empty

        // Reserve space for header + term data.
        // Use a sub-arena view of available space to copy the term.
        uint32_t avail = available_from_head();
        if (avail < sizeof(QueryEntry) + 64) return 0;  // conservative
		// Wraparound not supported: if head + entry_size would exceed
		// QUEUE2_ARENA_SIZE, return OOM. This keeps the buffer linear
		// (head >= tail) and is required for safe pointer-rewriting
		// compaction in remove(). Future work: amortized growth.

        uint8_t* entry_base = buf + head;
        Arena sub(entry_base + sizeof(QueryEntry),
                  avail - sizeof(QueryEntry));
        Term copied = deep_copy_term(sub, query_term);

        uint32_t term_bytes = static_cast<uint32_t>(
            reinterpret_cast<uint8_t*>(sub.cur) -
            reinterpret_cast<uint8_t*>(sub.base));
        uint32_t total = static_cast<uint32_t>(sizeof(QueryEntry)) + term_bytes;

        // Check we didn't overrun.
        if (head + total > QUEUE2_ARENA_SIZE) return 0;  // OOM
		// Wraparound not supported: if head + entry_size would exceed
		// QUEUE2_ARENA_SIZE, return OOM. This keeps the buffer linear
		// (head >= tail) and is required for safe pointer-rewriting
		// compaction in remove(). Future work: amortized growth.

        QueryEntry* entry  = reinterpret_cast<QueryEntry*>(entry_base);
        entry->id          = next_id;
        entry->byte_size   = total;
        entry->query_term  = copied;

        head += total;
        count++;
        return next_id++;
    }

    // Dequeue front entry (FIFO). Returns false if empty.
    bool dequeue(QueryEntry& out) {
        if (count == 0) return false;
        QueryEntry* e = reinterpret_cast<QueryEntry*>(buf + tail);
        out   = *e;
        tail += e->byte_size;
        count--;
        if (count == 0) { head = 0; tail = 0; }  // reset on empty
        return true;
    }

    // Remove a pending entry by ID.
    // Copies all entries after it forward using deep_copy_term (not memmove).
    // Returns true if found and removed.
    bool remove(uint32_t id) {
        if (count == 0) return false;

        // Find the entry.
        uint32_t pos = tail;
        for (uint32_t i = 0; i < count; ++i) {
            QueryEntry* e = reinterpret_cast<QueryEntry*>(buf + pos);
            if (e->id == id) {
                // Found. Copy all subsequent entries forward to fill the gap.
                uint32_t write_pos = pos;
                uint32_t read_pos  = pos + e->byte_size;

                while (read_pos < head) {

			// Correctness: this buffer is non-wrapping (head >= tail always,
			// wraparound returns OOM rather than wrapping). Compaction moves
			// entries toward lower addresses (toward tail). Processing
			// left-to-right means each entry's destination is strictly before
			// its source — deep_copy_term's sub-arena write cursor never
			// reaches the source data being read. If wraparound is added in
			// the future, this assumption breaks and a two-pass or
			// back-to-front approach will be needed.

                    QueryEntry* src = reinterpret_cast<QueryEntry*>(buf + read_pos);

                    // Copy this entry to write_pos using deep_copy_term
                    // so that interior PairNode* pointers are rewritten
                    // to point into their new location.
                    uint8_t* dst_base = buf + write_pos;
                    Arena sub(dst_base + sizeof(QueryEntry),
                              QUEUE2_ARENA_SIZE - write_pos - sizeof(QueryEntry));
                    Term copied = deep_copy_term(sub, src->query_term);

                    uint32_t term_bytes = static_cast<uint32_t>(
                        reinterpret_cast<uint8_t*>(sub.cur) -
                        reinterpret_cast<uint8_t*>(sub.base));
                    uint32_t new_size = static_cast<uint32_t>(sizeof(QueryEntry))
                                      + term_bytes;

                    QueryEntry* dst = reinterpret_cast<QueryEntry*>(dst_base);
                    dst->id         = src->id;
                    dst->byte_size  = new_size;
                    dst->query_term = copied;

                    write_pos += new_size;
                    read_pos  += src->byte_size;
                }

                head = write_pos;
                count--;
                return true;
            }
            pos += e->byte_size;
        }
        return false;  // not found
    }

    // Build agenda as a list term for passing to the next query.
    // Uses spine_arena for Pair nodes (reset after query starts).
    Term as_term(Arena& spine_arena) const {
        // Collect entry pointers (paper-scale: bounded by MAX_CHANGESET_OPS)
        const QueryEntry* entries[MAX_CHANGESET_OPS];
        uint32_t n   = 0;
        uint32_t pos = tail;
        for (uint32_t i = 0; i < count && n < MAX_CHANGESET_OPS; ++i) {
            entries[n++] = reinterpret_cast<const QueryEntry*>(buf + pos);
            pos += entries[i]->byte_size;
        }
        // Build list back-to-front.
        Term result = Term::nil();
        for (int32_t i = static_cast<int32_t>(n) - 1; i >= 0; --i) {
            PairNode* p = spine_arena.make<PairNode>();
            if (!p) break;
            p->car = entries[i]->query_term;
            p->cdr = result;
            result = Term::make_pair(p);
        }
        return result;
    }

private:
    uint32_t available_from_head() const {
        if (count == 0) return QUEUE2_ARENA_SIZE;
        return QUEUE2_ARENA_SIZE - head;
    }
};

static_assert(std::is_trivially_destructible_v<Agenda>);
```

---

## Part 3: Spine Arena

Implement `rap/spine.hpp`.

```cpp
// rap/spine.hpp
#pragma once
#include "../core/arena.hpp"

constexpr uint32_t SPINE_ARENA_SIZE = 4 * 1024;

struct SpineArena {
    uint8_t buf[SPINE_ARENA_SIZE];
    Arena   arena;

    SpineArena() : arena(buf, SPINE_ARENA_SIZE) {}

    Arena& get() { return arena; }
    void   reset() { arena.reset(); }
};

static_assert(std::is_trivially_destructible_v<SpineArena>);
```

---

## Part 4: Real no-ops and cons-ops in RapEvaluator

Replace the `// STAGE 0B STUB` implementations in `rap/rap.hpp`.

### RapEvaluator Constructor: Intern Three Symbols

Add `sym_empty_ops_` to the constructor alongside the existing two:

```cpp
class RapEvaluator : public Evaluator {
    const SymEntry* sym_no_ops_;
    const SymEntry* sym_cons_ops_;
    const SymEntry* sym_empty_ops_;  // ADD

public:
    RapEvaluator(Arena* arena, Intern* intern, const OutcomeSyms* syms)
        : Evaluator(arena, syms)
    {
        sym_no_ops_   = intern_cstr(*arena, *intern, "no-ops");
        sym_cons_ops_ = intern_cstr(*arena, *intern, "cons-ops");
        sym_empty_ops_= intern_cstr(*arena, *intern, "empty-ops");  // ADD
        // ... client region setup unchanged ...
    }
```

### ChangeSet Access Methods

Add to `RapEvaluator`:

```cpp
void init_changeset() {
    if (client_region_.id == ClientId::RAP &&
        client_region_.capacity >= sizeof(ChangeSet)) {
        new (client_region_.base) ChangeSet();
        client_region_.offset = sizeof(ChangeSet);
    }
}

ChangeSet* get_changeset() {
    if (client_region_.id != ClientId::RAP) return nullptr;
    if (client_region_.capacity < sizeof(ChangeSet)) return nullptr;
    return reinterpret_cast<ChangeSet*>(client_region_.base);
}
```

**Note on placement new:** `ChangeSet` contains an `Arena` member with a
constructor. This is the one place in the codebase using placement new.
It is correct here because the ChangeSet lives at a known fixed offset in
the client region and its lifetime is explicitly managed.

### Updated handle_no_ops

```cpp
// Replaces STAGE 0B STUB
inline StepResult RapEvaluator::handle_no_ops(
    const Term* args, uint32_t arg_count, State& st)
{
    if (arg_count != 1) return StepResult::NoYield;

    ChangeSet* cs = get_changeset();
    if (!cs) return StepResult::NoYield;

    // Unify args[0] with the empty-ops sentinel symbol.
    // sym_empty_ops_ was interned at construction time — no Intern needed.
    Term empty_term = Term::symbol(sym_empty_ops_);

    Term walked = resolve_bvar(args[0], st.env);
    walked      = walk(walked, st.subst);

    const Binding* new_subst = st.subst;
    if (!unify(*arena_, walked, empty_term, nullptr, new_subst))
        return StepResult::NoYield;
    st.subst = new_subst;

    // Allocate marker byte in client region for backtrack tracking.
    uint8_t* marker = static_cast<uint8_t*>(client_region_.alloc(1));
    if (!marker) return StepResult::NoYield;
    *marker = 0x00;

    return StepResult::Yield;
}
```

### Updated handle_cons_ops

```cpp
// Replaces STAGE 0B STUB
// cons-ops(Op, Rest, OpsOut)
// Op:     (add QueryTerm) | (remove QueryId) | (output Term)
// Rest:   existing ops (not inspected — we accumulate in ChangeSet directly)
// OpsOut: unified with a unique marker term so subsequent cons-ops calls
//         can chain. The actual ops accumulate in the ChangeSet struct.
inline StepResult RapEvaluator::handle_cons_ops(
    const Term* args, uint32_t arg_count, State& st)
{
    if (arg_count != 3) return StepResult::NoYield;

    ChangeSet* cs = get_changeset();
    if (!cs) return StepResult::NoYield;
    if (cs->op_count >= MAX_CHANGESET_OPS) return StepResult::NoYield;

    // Walk Op term (args[0]).
    Term op_term = resolve_bvar(args[0], st.env);
    op_term      = walk(op_term, st.subst);

    // Op must be a Pair: (tag . (arg . ()))
    if (op_term.tag != TermTag::Pair || !op_term.pair)
        return StepResult::NoYield;

    Term op_head = walk(op_term.pair->car, st.subst);
    Term op_rest = walk(op_term.pair->cdr, st.subst);

    // Extract single argument from the op list.
    if (op_rest.tag != TermTag::Pair || !op_rest.pair)
        return StepResult::NoYield;
    Term op_arg = walk(op_rest.pair->car, st.subst);

    if (op_head.tag != TermTag::Sym || !op_head.sym)
        return StepResult::NoYield;

    Op op;
    if (sym_lit_eq(op_head.sym, "add")) {
        op.tag        = OpTag::Add;
        op.query_term = deep_copy_term(cs->arena, op_arg);
    } else if (sym_lit_eq(op_head.sym, "remove")) {
        if (op_arg.tag != TermTag::Int) return StepResult::NoYield;
        op.tag      = OpTag::Remove;
        op.query_id = static_cast<uint32_t>(op_arg.value);
    } else if (sym_lit_eq(op_head.sym, "output")) {
        op.tag         = OpTag::Output;
        op.output_term = deep_copy_term(cs->arena, op_arg);
    } else {
        return StepResult::NoYield;  // unknown op tag
    }

    if (!cs->push(op)) return StepResult::NoYield;

    // Allocate marker byte for backtrack tracking.
    uint8_t* marker = static_cast<uint8_t*>(client_region_.alloc(1));
    if (!marker) return StepResult::NoYield;
    *marker = static_cast<uint8_t>(op.tag);

    // OpsOut (args[2]): unify with a unique Int term (current op count).
    // This lets subsequent cons-ops calls chain: the "Rest" they receive
    // is this Int, which they accept without inspecting (they only
    // accumulate ops into the ChangeSet). The Int value is unique per call.
    Term ops_out = Term::integer(static_cast<int32_t>(cs->op_count));
    Term walked_out = resolve_bvar(args[2], st.env);
    walked_out      = walk(walked_out, st.subst);

    const Binding* new_subst = st.subst;
    if (!unify(*arena_, walked_out, ops_out, nullptr, new_subst))
        return StepResult::NoYield;
    st.subst = new_subst;

    return StepResult::Yield;
}
```

**Note on OpsOut unification:**
The `qids->remove-opso` relation chains cons-ops calls where each call's
OpsOut becomes the next call's Rest. By unifying OpsOut with the current
op count (a unique Int per call), we give the next call something concrete
to receive as Rest without needing to build a real term representation of
the ops list. This works because cons-ops never inspects its Rest argument
— it only appends to the ChangeSet. If this approach fails for any case
study, the fallback is to build a proper list term representation.

**Note on unify() signature:**
The exact signature of `unify()` in `core.hpp` after Stage 0A may differ
from what is shown here. Use whatever signature actually exists. The key
is: unify two terms under a substitution, returning the extended
substitution or failing.

---

## Part 5: Reactive Execution Loop

Implement `rap/loop.hpp`.

### Query Term Representation

Queries in Queue 2 are stored as `Rel` terms — anonymous relations with
one parameter (the agenda term). This is Approach B from the spec design
discussion and is architecturally consistent: a query IS a relation, the
agenda stores relation terms, and executing a query is calling a relation
with the current agenda as argument.

```
query_term = Rel{ param_count=1, body=parsed_goal }
```

The execution loop calls the query as:
```
(call query_term current_agenda_list)
```

Inside the query body, `BVar(0)` is the agenda term.

This means `defrel`-defined queries that take the agenda as first argument
are called naturally:

```scheme
(defrel (my-query agenda)
  ; agenda is BVar(0) — the current pending-query list
  (membero ... agenda)
  ...)
```

### RapLoop Struct

```cpp
// rap/loop.hpp
#pragma once
#include "../core/mktypes.hpp"
#include "../core/core.hpp"
#include "../core/sexp_parser.hpp"
#include "rap.hpp"
#include "agenda.hpp"
#include "spine.hpp"
#include "changeset.hpp"
#include <cstdio>

constexpr uint32_t MAX_OUTPUT_TERMS  = 256;
constexpr uint32_t EVAL_ARENA_SIZE   = 128 * 1024;  // 128 KiB per query

struct OutputQueue {
    Term     terms[MAX_OUTPUT_TERMS];
    uint32_t count = 0;

    bool push(Term t) {
        if (count >= MAX_OUTPUT_TERMS) return false;
        terms[count++] = t;
        return true;
    }
    void reset() { count = 0; }
};

static_assert(std::is_trivially_destructible_v<OutputQueue>);

struct RapLoop {
    // Long-lived arenas (persist across queries)
    alignas(64) uint8_t  intern_buf[32 * 1024];
    Arena    intern_arena;
    Intern   intern;
    OutcomeSyms syms;

    // Per-query arena (reset between queries)
    alignas(64) uint8_t  eval_buf[EVAL_ARENA_SIZE];
    Arena    eval_arena;

    // Queues
    Agenda      agenda;
    SpineArena  spine;
    OutputQueue output;

    // Relation environment (populated by load_defs)
    RelEnv rel_env;

    // The evaluator lives in the intern arena (stable pointer)
    RapEvaluator* evaluator = nullptr;

    RapLoop()
        : intern_arena(intern_buf, sizeof(intern_buf))
        , eval_arena(eval_buf, sizeof(eval_buf))
    {}

    // Initialize. Must be called before any other method.
    bool init() {
        if (!intern_init(intern_arena, intern, 256)) return false;

        syms.s_true         = intern_cstr(intern_arena, intern, "true");
        syms.s_false        = intern_cstr(intern_arena, intern, "false");
        syms.s_insufficient = intern_cstr(intern_arena, intern, "insufficient");
        syms.s_bounded      = intern_cstr(intern_arena, intern, "bounded");

        void* p = intern_arena.alloc(sizeof(RapEvaluator),
                                     alignof(RapEvaluator));
        if (!p) return false;
        evaluator = new (p) RapEvaluator(&eval_arena, &intern, &syms);
        return evaluator != nullptr;
    }

    // Parse and register relation definitions (defrel forms).
    // src may contain multiple defrel forms — they are added to rel_env.
    // Returns true if all parse successfully.
    bool load_defs(const char* src) {
        ParsedQuery pq = parse_query(intern_arena, src);
        // Merge pq.rel_env into our rel_env.
        // [IMPL NOTE: RelEnv is a linked list. Merge by walking pq.rel_env
        //  and calling rel_env.define() for each entry, or by prepending
        //  the pq.rel_env.head to our rel_env.head if the Arena lifetimes
        //  are compatible. Use whichever approach is cleaner given the
        //  actual RelEnv implementation.]
        (void)pq;
        return true;  // stub — implement properly
    }

    // Enqueue a query by name (must be defined via load_defs).
    // The query relation takes one argument: the agenda term.
    // Returns assigned query_id, or 0 on failure.
    uint32_t enqueue_query(const char* rel_name) {
        const SymEntry* name_sym = intern_cstr(intern_arena, intern, rel_name);
        if (!name_sym) return 0;

        Term rel_term = rel_env.lookup(name_sym);
        if (rel_term.tag != TermTag::Rel) return 0;

        return agenda.enqueue(rel_term);
    }

    // Enqueue a query from a raw Rel term.
    uint32_t enqueue_term(Term rel_term) {
        if (rel_term.tag != TermTag::Rel) return 0;
        return agenda.enqueue(rel_term);
    }

    // Run until agenda empty or max_steps exceeded.
    void run_until_empty(uint32_t max_steps = 10000) {
        uint32_t steps = 0;
        while (!agenda.empty() && steps < max_steps) {
            run_one();
            ++steps;
        }
    }

    // Run one query from the front of the agenda.
    void run_one() {
        QueryEntry entry;
        if (!agenda.dequeue(entry)) return;

        // Build agenda list term from remaining pending entries.
        spine.reset();
        Term agenda_term = agenda.as_term(spine.get());

        // Initialize ChangeSet in client region.
        evaluator->init_changeset();

        // Execute: call the query relation with the agenda term.
        // The query is a Rel with param_count=1.
        // Build a (call query_term agenda_term) goal and run it.
        if (entry.query_term.tag == TermTag::Rel) {
            // Build GoalCall in eval_arena.
            Term* arg = eval_arena.make<Term>();
            if (!arg) return;
            *arg = agenda_term;

            GoalCall gc;
            gc.rel_term  = entry.query_term;
            gc.args      = arg;
            gc.arg_count = 1;

            Goal* call_goal = eval_arena.make<Goal>();
            if (!call_goal) return;
            call_goal->tag  = GoalTag::Call;
            call_goal->call = gc;

            // Run via evaluator.
            Term qvar     = Term::var(0);
            uint32_t vars = 1;

            evaluator->runN(1, call_goal, qvar, vars, rel_env,
                [&](Term, State) {
                    // Solution found — ChangeSet is now populated.
                    // (We only need one solution.)
                });
        }

        // Extract and apply ChangeSet.
        ChangeSet* cs = evaluator->get_changeset();
        if (cs) apply_changeset(*cs);

        // Reset per-query arena.
        eval_arena.reset();
    }

    // Print all output terms collected so far.
    void print_output() const {
        for (uint32_t i = 0; i < output.count; ++i) {
            print_term(output.terms[i]);
            std::printf("\n");
        }
    }

private:
    void apply_changeset(const ChangeSet& cs) {
        for (uint32_t i = 0; i < cs.op_count; ++i) {
            const Op& op = cs.ops[i];
            switch (op.tag) {
                case OpTag::Add:
                    agenda.enqueue(op.query_term);
                    break;
                case OpTag::Remove:
                    agenda.remove(op.query_id);
                    break;
                case OpTag::Output:
                    output.push(op.output_term);
                    std::printf("[output] ");
                    print_term(op.output_term);
                    std::printf("\n");
                    break;
            }
        }
    }
};
```

**Implementation notes for RapLoop:**

The `load_defs` method is stubbed. Claude Code must implement the
`RelEnv` merging properly. The key question is whether `pq.rel_env`
entries can be safely prepended to `rel_env` given arena lifetime
differences, or whether each entry must be re-interned. Look at the
actual `RelEnv` struct and `define()` method from Stage 0A to determine
the right approach.

The `eval_arena.reset()` at the end of `run_one()` invalidates the
`GoalCall` and `Term` arg allocated during execution. This is correct —
they are single-use per query. The ChangeSet has already been extracted
and applied before the reset.

---

## The strengthen-agendao Case Study

This is the validation target for Stage 2. The following program must
parse, run, and produce the expected ChangeSet.

```scheme
(defrel (membero x lst)
  (disj
    (fresh (rest) (== lst (x . rest)))
    (fresh (head rest)
      (conj (== lst (head . rest))
            (call membero x rest)))))

(defrel (weak-check-qido item H T qid)
  (== item (q qid (check H T))))

(defrel (collect-weak-qidso agenda H T qids)
  (disj
    (conj (== agenda ()) (== qids ()))
    (fresh (item rest qid tail)
      (conj
        (== agenda (item . rest))
        (disj
          (conj
            (call weak-check-qido item H T qid)
            (call collect-weak-qidso rest H T tail)
            (== qids (qid . tail)))
          (call collect-weak-qidso rest H T qids))))))

(defrel (qids->remove-opso qids ops)
  (disj
    (conj (== qids ()) (call no-ops ops))
    (fresh (qid rest ops-tail)
      (conj
        (== qids (qid . rest))
        (call qids->remove-opso rest ops-tail)
        (call cons-ops (remove qid) ops-tail ops)))))

(defrel (strengthen-agendao agenda ops)
  (fresh (H T R strong-qid weak-qids ops0)
    (conj
      (call membero (q strong-qid (check+ H T R)) agenda)
      (call collect-weak-qidso agenda H T weak-qids)
      (call qids->remove-opso weak-qids ops0)
      (call cons-ops (output (pruned H T)) ops0 ops))))
```

**Test scenario — initial agenda:**
```
entry id=10: (q 10 (check  hypA test1))
entry id=11: (q 11 (check+ hypA test1 refineX))
entry id=12: (q 12 (check  hypA test1))
entry id=13: (q 13 (explore hypB 2)))
```

**Expected ChangeSet after running strengthen-agendao:**
- `Remove(10)`
- `Remove(12)`
- `Output((pruned hypA test1))`

**Expected agenda after applying ChangeSet:**
- entry id=11 remains
- entry id=13 remains

---

## Validation Program: rap/test_stage2.cpp

```cpp
// rap/test_stage2.cpp
// Validates the strengthen-agendao case study (paper Section 5.2).
// Expected output: PASS: N tests, 0 failures

#include "rap/loop.hpp"
#include <cstdio>

static int passed = 0;
static int failed = 0;

#define EXPECT(cond, msg) \
    do { if (cond) { ++passed; std::printf("PASS: %s\n", msg); } \
         else { ++failed; std::printf("FAIL: %s\n", msg); } } while(0)

int main() {
    RapLoop loop;
    EXPECT(loop.init(), "RapLoop initializes");

    // Load relation definitions.
    const char* defs = R"(
        (defrel (membero x lst) ...)
        ; [full definitions as above — paste them here]
    )";
    EXPECT(loop.load_defs(defs), "Relation definitions load");

    // Manually construct the test agenda with 4 entries.
    // Each entry is a query term: the strengthen-agendao relation.
    // We construct the initial agenda entries as terms directly.

    // [IMPL NOTE: For testing, construct the 4 agenda entries as terms
    //  using the s-expression parser, then enqueue them manually.
    //  The strengthen-agendao query is then run with this agenda.]

    // Run strengthen-agendao.
    uint32_t qid = loop.enqueue_query("strengthen-agendao");
    EXPECT(qid != 0, "strengthen-agendao enqueued");

    loop.run_one();

    // Verify ChangeSet was applied correctly.
    // Check output queue for (pruned hypA test1).
    EXPECT(loop.output.count == 1, "One output term produced");

    // Check agenda has entries 11 and 13 remaining.
    EXPECT(loop.agenda.count == 2, "Two entries remain in agenda");

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
```

**Claude Code should fill in the full test implementation** based on the
actual APIs that exist after implementing the loop. The structure above
shows the intended shape; the exact calls depend on what `RapLoop`
exposes.

---

## Makefile Additions

```makefile
test_stage2: rap/test_stage2.cpp \
    rap/loop.hpp rap/agenda.hpp rap/spine.hpp rap/changeset.hpp \
    rap/rap.hpp core/sexp_parser.hpp core/core.hpp \
    core/intern.hpp core/arena.hpp core/mktypes.hpp
	$(CXX) $(CXXFLAGS) -o $@ $<
```

Add `test_stage2` to `all` and to `make test`.

---

## Implementation Order

1. `rap/changeset.hpp` — `Op`, `ChangeSet`, `deep_copy_term`
2. `rap/agenda.hpp` — `QueryEntry`, `Agenda` with pointer-rewriting remove
3. `rap/spine.hpp` — trivial
4. Update `rap/rap.hpp` — `init_changeset`, `get_changeset`, `sym_empty_ops_`,
   real `no-ops`/`cons-ops`
5. `rap/loop.hpp` — execution loop
6. `rap/test_stage2.cpp` — validation

Run `make` after each step. Run `make test` after step 4.

---

## Priority Order (for May 12 deadline)

**Must have for paper (Option Full):**
- `no-ops` and `cons-ops` work correctly (step 4)
- `strengthen-agendao` produces correct ChangeSet
- `test_stage2` passes

**Nice to have:**
- Full `RapLoop` with reactive execution
- `load_defs` properly merging RelEnv

**Option D fallback (if not complete by May 12):**
The paper draft already covers this case. The existing implementation
is a strong submission without Stage 2. Do not sacrifice correctness
to meet the deadline — report status honestly on May 12.

---

## What Does NOT Change

- `core/` — no changes
- `security/security_test.cpp` — must not be modified
- `core/test_extension.cpp` — no changes
- `rap/test_rap_extension.cpp` — no changes
- All Stage 0C tests must still pass

---

## Acceptance Criteria for Stage 2

- [ ] `rap/changeset.hpp` — `Op`, `ChangeSet`, `deep_copy_term`
- [ ] `rap/agenda.hpp` — `Agenda` with pointer-rewriting compaction
- [ ] `rap/spine.hpp` — `SpineArena`
- [ ] `rap/loop.hpp` — `RapLoop` with reactive execution loop
- [ ] `rap/rap.hpp` — stubs replaced, `sym_empty_ops_` added,
      `init_changeset`/`get_changeset` added
- [ ] `STAGE 0B STUB` markers removed
- [ ] `rap/test_stage2.cpp` validates `strengthen-agendao`
- [ ] ChangeSet contains Remove(10), Remove(12), Output(pruned hypA test1)
- [ ] Agenda after application contains entries 11 and 13
- [ ] `make` clean with `-Werror`
- [ ] `make test` passes all tests including `test_stage2`
- [ ] `security/security_test` all 10 cases pass
- [ ] `parse_run` all 12 programs unchanged
- [ ] This document updated if design decisions changed

---

*v1.0 May 8, 2026 — initial specification*  
*v1.1 May 8, 2026 — pointer-rewriting compaction, empty-ops fix,*  
*OpsOut unification approach documented*

