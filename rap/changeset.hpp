// rap/changeset.hpp — Stage 2: ChangeSet, Op, deep_copy_term
#pragma once
#include "../core/mktypes.hpp"
#include "../core/core.hpp"

// ============================================================================
// deep_copy_term: deep-copies a term into dest_arena, rewriting all interior
// PairNode* pointers to point into dest_arena.
// Sym pointers are shared (interned symbols are stable — no copy needed).
// RelNode bodies are NOT copied (they live in parse-time arena, stable).
// ============================================================================
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

// ============================================================================
// Op types
// ============================================================================
enum class OpTag : std::uint8_t {
    Add    = 0,  // add(QueryTerm)   — enqueue a query
    Remove = 1,  // remove(QueryId)  — remove pending query by ID
    Output = 2,  // output(Term)     — append to output queue
};

struct Op {
    OpTag tag;
    union {
        Term     query_term;   // Add
        std::uint32_t query_id;     // Remove
        Term     output_term;  // Output
    };
};
static_assert(std::is_trivially_destructible_v<Op>);

// ============================================================================
// ChangeSet: accumulates ops during a single query execution.
//
// NOTE: This struct does NOT embed its own arena buffer. The Arena member
// is initialized by RapEvaluator::init_changeset to point into the
// client_region_ buffer after sizeof(ChangeSet). This keeps sizeof(ChangeSet)
// well under MAX_CHANGESET_ARENA.
// ============================================================================
struct ChangeSet {
    Op            ops[MAX_CHANGESET_OPS];
    std::uint32_t op_count = 0;
    Arena         arena;   // initialized externally to point into client_region_

    // Constructor: arena is set to nullptr/0 until init_changeset runs.
    ChangeSet() : arena(nullptr, 0) {}

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
