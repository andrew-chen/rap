#pragma once
#include "../core/sexp_parser.hpp"

// ============================================================================
// RapEvaluator: Evaluator subclass for the RAP layer (Stage 0B)
//
// Registers as ClientId::RAP and allocates a client region from the arena.
// Overrides handleUnknownRelation to handle 'no-ops' and 'cons-ops'.
// All other unknown relations fall through to StepResult::NoYield (failure).
//
// Relationship to constructor:
//   The constructor takes Intern* to intern the relation names "no-ops" and
//   "cons-ops" once, stores the resulting SymEntry* for O(1) pointer-identity
//   comparison at runtime. The Intern* is NOT stored after construction.
// ============================================================================

class RapEvaluator : public Evaluator {
public:
  RapEvaluator(Arena* arena, Intern* intern, const OutcomeSyms* syms)
    : Evaluator(arena, syms)
  {
    // Intern relation names once; compare by pointer identity at runtime.
    sym_no_ops_   = intern_cstr(*arena, *intern, "no-ops");
    sym_cons_ops_ = intern_cstr(*arena, *intern, "cons-ops");

    // Allocate the client region from the main arena.
    std::uint8_t* region_base = static_cast<std::uint8_t*>(
        arena->alloc(MAX_CHANGESET_ARENA, alignof(std::max_align_t)));
    client_region_.id       = ClientId::RAP;
    client_region_.base     = region_base;
    client_region_.capacity = MAX_CHANGESET_ARENA;
    client_region_.offset   = 0;
  }

protected:
  StepResult handleUnknownRelation(
      const SymEntry* name,
      const Term*     args,
      std::uint32_t   arg_count,
      State&          st) override;

private:
  const SymEntry* sym_no_ops_   = nullptr;
  const SymEntry* sym_cons_ops_ = nullptr;

  // STAGE 0B STUB — replace in Stage 2 with real ChangeSet construction
  StepResult handle_no_ops(const Term* args, std::uint32_t arg_count, State& st);
  // STAGE 0B STUB — replace in Stage 2 with real ChangeSet construction
  StepResult handle_cons_ops(const Term* args, std::uint32_t arg_count, State& st);
};

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

// STAGE 0B STUB — replace in Stage 2 with real ChangeSet construction
inline StepResult RapEvaluator::handle_no_ops(
    const Term* args, std::uint32_t arg_count, State& st)
{
  if (arg_count != 1) return StepResult::NoYield;

  // Allocate a marker byte to prove backtrack rewind works.
  std::uint8_t* marker = static_cast<std::uint8_t*>(client_region_.alloc(1));
  if (!marker) return StepResult::NoYield;  // region full
  *marker = 0x00;  // sentinel: empty ops list

  (void)args; (void)st;
  return StepResult::Yield;  // STAGE 0B STUB — replace in Stage 2
}

// STAGE 0B STUB — replace in Stage 2 with real ChangeSet construction
inline StepResult RapEvaluator::handle_cons_ops(
    const Term* args, std::uint32_t arg_count, State& st)
{
  if (arg_count != 3) return StepResult::NoYield;

  // Allocate a marker byte to prove backtrack rewind works.
  std::uint8_t* marker = static_cast<std::uint8_t*>(client_region_.alloc(1));
  if (!marker) return StepResult::NoYield;  // region full
  *marker = 0x01;  // sentinel: cons op

  (void)args; (void)st;
  return StepResult::Yield;  // STAGE 0B STUB — replace in Stage 2
}
