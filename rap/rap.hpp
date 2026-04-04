#pragma once
#include "../core/sexp_parser.hpp"
#include "work_queue.hpp"

// ============================================================================
// RapEngine: bundles a core µKanren evaluator with interned outcome symbols.
// Sits above core/ and will eventually host work-queue coordination, probe
// policy, and agent-facing interfaces.
// ============================================================================
struct RapEngine {
  Arena*      arena = nullptr;
  Intern      intern{};
  OutcomeSyms syms{};

  // Initialise engine against an already-constructed arena.
  // Returns false if arena is too small to intern the outcome symbols.
  static bool init(Arena& a, RapEngine& out) {
    out.arena  = &a;
    out.intern = Intern{0, nullptr};
    if (!intern_init(a, out.intern, 256)) return false;

    out.syms.s_true         = intern_cstr(a, out.intern, "true");
    out.syms.s_false        = intern_cstr(a, out.intern, "false");
    out.syms.s_insufficient = intern_cstr(a, out.intern, "insufficient");
    out.syms.s_bounded      = intern_cstr(a, out.intern, "bounded");

    return out.syms.s_true && out.syms.s_false &&
           out.syms.s_insufficient && out.syms.s_bounded;
  }

  // Run up to n answers for query_goal, calling on_answer(Term, State) each time.
  template<class OnAnswer>
  void run(int n, const Goal* query_goal, Term query_var,
           std::uint32_t vars_used, OnAnswer&& on_answer) {
    runN(*arena, n, query_goal, query_var, vars_used, syms,
         std::forward<OnAnswer>(on_answer));
  }
};
