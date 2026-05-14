#pragma once
#include "../core/sexp_parser.hpp"
#include "changeset.hpp"
#include <new>  // for placement new

// ============================================================================
// RapEvaluator: Evaluator subclass for the RAP layer (Stage 0B / Stage 2)
//
// Registers as ClientId::RAP and allocates a client region from the arena.
// Overrides handleUnknownRelation to handle 'no-ops' and 'cons-ops'.
// All other unknown relations fall through to StepResult::NoYield (failure).
//
// Relationship to constructor:
//   The constructor takes Intern* to intern the relation names "no-ops",
//   "cons-ops", and "empty-ops" once, storing the resulting SymEntry* for
//   O(1) pointer-identity comparison at runtime. The Intern* is NOT stored.
// ============================================================================

class RapEvaluator : public Evaluator {
public:
  RapEvaluator(Arena* arena, Intern* intern, const OutcomeSyms* syms)
    : Evaluator(arena, syms)
  {
    // Intern relation names once; compare by pointer identity at runtime.
    sym_no_ops_    = intern_cstr(*arena, *intern, "no-ops");
    sym_cons_ops_  = intern_cstr(*arena, *intern, "cons-ops");
    sym_empty_ops_ = intern_cstr(*arena, *intern, "empty-ops");

    // Allocate the client region from the main arena.
    std::uint8_t* region_base = static_cast<std::uint8_t*>(
        arena->alloc(MAX_CHANGESET_ARENA, alignof(std::max_align_t)));
    client_region_.id       = ClientId::RAP;
    client_region_.base     = region_base;
    client_region_.capacity = MAX_CHANGESET_ARENA;
    client_region_.offset   = 0;
  }

  // Initialize the ChangeSet in the client region.
  // Must be called before runN() when using Stage 2 ChangeSet machinery.
  // Sets client_region_.offset = sizeof(ChangeSet) so that runN's initial
  // State saves this offset — backtracking never rewinds below the header.
  void init_changeset() {
    if (client_region_.id != ClientId::RAP) return;
    if (client_region_.capacity < sizeof(ChangeSet)) return;

    ChangeSet* cs = new (client_region_.base) ChangeSet();

    // Initialize the ChangeSet's internal arena to use the space after
    // the ChangeSet header within client_region_.
    std::uint8_t* arena_buf = client_region_.base
                            + static_cast<std::uint32_t>(sizeof(ChangeSet));
    std::uint32_t arena_cap = client_region_.capacity
                            - static_cast<std::uint32_t>(sizeof(ChangeSet));
    new (&cs->arena) Arena(arena_buf, arena_cap);

    client_region_.offset = static_cast<std::uint32_t>(sizeof(ChangeSet));
  }

  // Return the ChangeSet pointer.
  // Valid after init_changeset() or after an internal get_or_init_changeset call.
  ChangeSet* get_changeset() {
    if (client_region_.id != ClientId::RAP) return nullptr;
    if (client_region_.capacity < sizeof(ChangeSet)) return nullptr;
    return reinterpret_cast<ChangeSet*>(client_region_.base);
  }

protected:
  StepResult handleUnknownRelation(
      const SymEntry* name,
      const Term*     args,
      std::uint32_t   arg_count,
      State&          st) override;

private:
  const SymEntry* sym_no_ops_    = nullptr;
  const SymEntry* sym_cons_ops_  = nullptr;
  const SymEntry* sym_empty_ops_ = nullptr;

  // Lazy init: if client_region_.offset == 0, auto-initialize the ChangeSet.
  // This ensures backward compatibility with Stage 0C tests that call runN
  // directly without calling init_changeset first.
  ChangeSet* get_or_init_changeset() {
    if (client_region_.id != ClientId::RAP) return nullptr;
    if (client_region_.capacity < sizeof(ChangeSet)) return nullptr;
    if (client_region_.offset == 0) init_changeset();
    return reinterpret_cast<ChangeSet*>(client_region_.base);
  }

  StepResult handle_no_ops(const Term* args, std::uint32_t arg_count, State& st);
  StepResult handle_cons_ops(const Term* args, std::uint32_t arg_count, State& st);
};

// ============================================================================
// handleUnknownRelation dispatch
// ============================================================================
inline StepResult RapEvaluator::handleUnknownRelation(
    const SymEntry* name, const Term* args,
    std::uint32_t arg_count, State& st)
{
  // Safety check: verify this is our client region.
  if (client_region_.id != ClientId::RAP)
    return StepResult::NoYield;

  // Pointer-identity comparison (both sides interned from the same Intern table).
  if (name == sym_no_ops_)
    return handle_no_ops(args, arg_count, st);
  if (name == sym_cons_ops_)
    return handle_cons_ops(args, arg_count, st);

  // Not recognized by the RAP layer either.
  return StepResult::NoYield;
}

// ============================================================================
// handle_no_ops: (no-ops OpsChain)
// Marks the empty end of an ops chain. Succeeds without binding OpsChain.
// The chain variable is used by cons-ops as its Rest argument (which is
// never inspected), so leaving it unbound is correct.
// ============================================================================
inline StepResult RapEvaluator::handle_no_ops(
    const Term* args, std::uint32_t arg_count, State& st)
{
  if (arg_count != 1) return StepResult::NoYield;

  ChangeSet* cs = get_or_init_changeset();
  if (!cs) return StepResult::NoYield;

  // no-ops succeeds without binding its argument.
  // The chain variable may later be passed as Rest to cons-ops, which ignores it.
  (void)args; (void)st;

  // Marker byte in ChangeSet's arena for consistency tracking.
  void* marker = cs->arena.alloc(1, 1);
  if (!marker) return StepResult::NoYield;
  *static_cast<std::uint8_t*>(marker) = 0x00;

  return StepResult::Yield;
}

// ============================================================================
// handle_cons_ops: (cons-ops Op Rest OpsOut)
//   Op:     (add QueryTerm) | (remove QueryId) | (output Term)
//   Rest:   existing ops chain (not inspected — ops accumulate in ChangeSet)
//   OpsOut: unified with current op_count (unique Int per call)
// ============================================================================
inline StepResult RapEvaluator::handle_cons_ops(
    const Term* args, std::uint32_t arg_count, State& st)
{
  if (arg_count != 3) return StepResult::NoYield;

  ChangeSet* cs = get_or_init_changeset();
  if (!cs) return StepResult::NoYield;
  if (cs->op_count >= MAX_CHANGESET_OPS) return StepResult::NoYield;

  // Walk the Op term (args[0]).
  Term op_term = resolve_bvar(args[0], st.env);
  op_term      = walk(op_term, st.subst, RelEnv{});

  // Extract Op structure if it is a Pair (tag . (arg . ())).
  // Guard all pointer dereferences — op_term may be any term type.
  Term op_head = Term::nil();
  Term op_rest = Term::nil();
  Term op_arg  = Term::nil();
  if (op_term.tag == TermTag::Pair && op_term.pair) {
    op_head = walk(op_term.pair->car, st.subst, RelEnv{});
    op_rest = walk(op_term.pair->cdr, st.subst, RelEnv{});
    if (op_rest.tag == TermTag::Pair && op_rest.pair)
      op_arg = walk(op_rest.pair->car, st.subst, RelEnv{});
  }

  // If Op is not a valid Pair (tag . (arg . ())), succeed without pushing
  // to the ChangeSet. This preserves backward compatibility for callers that
  // pass opaque symbols as the Op argument (e.g., in Stage 0C tests).
  bool push_this_op = false;
  Op op;

  if (op_term.tag == TermTag::Pair && op_term.pair &&
      op_head.tag == TermTag::Sym && op_head.sym &&
      op_rest.tag == TermTag::Pair && op_rest.pair) {

    if (sym_lit_eq(op_head.sym, "add")) {
      op.tag        = OpTag::Add;
      op.query_term = deep_copy_term(cs->arena, op_arg);
      push_this_op  = true;
    } else if (sym_lit_eq(op_head.sym, "remove")) {
      if (op_arg.tag != TermTag::Int) return StepResult::NoYield;
      op.tag      = OpTag::Remove;
      op.query_id = static_cast<std::uint32_t>(op_arg.value);
      push_this_op = true;
    } else if (sym_lit_eq(op_head.sym, "output")) {
      op.tag         = OpTag::Output;
      op.output_term = deep_copy_term(cs->arena, op_arg);
      push_this_op   = true;
    }
    // Unknown op tag: push_this_op stays false — succeed without pushing.
  }
  // Non-Pair Op arg: succeed without pushing.

  if (push_this_op && !cs->push(op)) return StepResult::NoYield;

  // Marker byte for consistency tracking.
  void* marker = cs->arena.alloc(1, 1);
  if (!marker) return StepResult::NoYield;
  *static_cast<std::uint8_t*>(marker) = static_cast<std::uint8_t>(op.tag);

  // OpsOut (args[2]): unify with current op_count as a unique Int.
  // This lets subsequent cons-ops calls chain (each OpsOut becomes next call's Rest).
  Term ops_out    = Term::integer(static_cast<std::int32_t>(cs->op_count));
  Term walked_out = resolve_bvar(args[2], st.env);
  walked_out      = walk(walked_out, st.subst, RelEnv{});

  const Binding* new_subst = st.subst;
  if (!unify(*arena_, walked_out, ops_out, nullptr, new_subst, RelEnv{}))
    return StepResult::NoYield;
  st.subst = new_subst;

  return StepResult::Yield;
}
