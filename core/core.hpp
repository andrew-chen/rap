#pragma once
#include "arena.hpp"
#include "intern.hpp"
#include <cstdint>

enum class StepResult : std::uint8_t { NoYield, Yield, OOM };

// ---- Terms ----
enum class TermTag : std::uint8_t { Var, BVar, Int, Sym, Nil, Pair };
struct PairNode;

struct Term {
  TermTag tag;
  union {
    std::uint32_t id;        // Var or BVar
    std::int32_t value;      // Int
    const SymEntry* sym;     // Sym
    const PairNode* pair;    // Pair
  };

  static Term var(std::uint32_t v)        { Term t; t.tag = TermTag::Var;  t.id = v; return t; }
  static Term bvar(std::uint32_t k)       { Term t; t.tag = TermTag::BVar; t.id = k; return t; }
  static Term integer(std::int32_t v)     { Term t; t.tag = TermTag::Int;  t.value = v; return t; }
  static Term symbol(const SymEntry* s)   { Term t; t.tag = TermTag::Sym;  t.sym = s; return t; }
  static Term nil()                       { Term t; t.tag = TermTag::Nil;  t.id = 0; return t; }
  static Term make_pair(const PairNode* p){ Term t; t.tag = TermTag::Pair; t.pair = p; return t; }
};

struct PairNode { Term car; Term cdr; };


// ---- Goals ----
enum class GoalTag : std::uint8_t { Eq, Disj, Conj, Fresh, Probe };

struct GoalEq   { Term u; Term v; };
struct GoalBin  { const struct Goal* g1; const struct Goal* g2; };
struct GoalFresh{ std::uint32_t n; const Goal* body; };


struct GoalProbe{
  const struct Goal* sub;
  Term condition;   // must be Sym or Var already bound to Sym (true/false/insufficient/bounded)
  Term max_iter;    // must be Int or Var already bound to Int
  Term sandbox;     // must be Sym(true/false) or Var already bound to Sym
  Term req_ground;  // must be Sym(true/false) or Var already bound to Sym
};

struct Goal {
  GoalTag tag;
  union {
    GoalEq eq;
    GoalBin bin;
    GoalFresh fresh;
    GoalProbe probe;
  };
};

// ============================================================================
// 4-valued outcome used by probe/meta-evaluation
// ============================================================================
enum class Outcome : std::uint8_t { True, False, Insufficient, Bounded };

struct OutcomeSyms {
  const SymEntry* s_true = nullptr;
  const SymEntry* s_false = nullptr;
  const SymEntry* s_insufficient = nullptr;
  const SymEntry* s_bounded = nullptr;
};

inline const SymEntry* outcome_to_sym(Outcome o, const OutcomeSyms& syms) {
  switch (o) {
    case Outcome::True:        return syms.s_true;
    case Outcome::False:       return syms.s_false;
    case Outcome::Insufficient:return syms.s_insufficient;
    case Outcome::Bounded:     return syms.s_bounded;
    default:                   return nullptr;
  }
}

inline bool sym_to_outcome(const SymEntry* s, const OutcomeSyms& syms, Outcome& out) {
  if (s == syms.s_true)        { out = Outcome::True; return true; }
  if (s == syms.s_false)       { out = Outcome::False; return true; }
  if (s == syms.s_insufficient){ out = Outcome::Insufficient; return true; }
  if (s == syms.s_bounded)     { out = Outcome::Bounded; return true; }
  return false;
}

// ============================================================================
// Core µKanren-ish kernel (arena-only; POD terms; iterative unify; fair eval)
// Adds:
//   - TermTag::BVar (de Bruijn bound variable index)
//   - GoalTag::Fresh (binder introducing new logic vars without closures)
//   - State.env runtime stack to resolve BVar -> Var(id)
// ============================================================================


static_assert(std::is_trivially_destructible_v<Term>);
static_assert(std::is_trivially_destructible_v<PairNode>);

// ---- Substitution / State ----
struct Binding {
  std::uint32_t var;
  Term value;
  const Binding* next;
};

static_assert(std::is_trivially_destructible_v<Binding>);

struct Diseq {
  Term u;
  Term v;
  const Diseq* next;
};

static_assert(std::is_trivially_destructible_v<Diseq>);


// Runtime environment stack for bound vars (de Bruijn):
// top frame corresponds to BVar(0), next is BVar(1), etc.
struct EnvFrame {
  std::uint32_t var_id;
  const EnvFrame* next;
};

static_assert(std::is_trivially_destructible_v<EnvFrame>);

struct State {
  const Binding* subst;
  const Diseq* diseqs;   // disequality constraints
  const EnvFrame* env;   // runtime env for resolving BVar
  std::uint32_t counter; // fresh var supply
};

static_assert(std::is_trivially_destructible_v<State>);

inline Term walk(Term t, const Binding* s) {
  while (t.tag == TermTag::Var) {
    const Binding* cur = s;
    bool found = false;
    while (cur) {
      if (cur->var == t.id) { t = cur->value; found = true; break; }
      cur = cur->next;
    }
    if (!found) break;
  }
  return t;
}

inline const Binding* ext_s(Arena& a, const Binding* s, std::uint32_t v, Term val) {
  Binding* b = a.make<Binding>();
  if (!b) return nullptr;
  b->var = v;
  b->value = val;
  b->next = s;
  return b;
}

inline Term resolve_bvar(Term t, const EnvFrame* env) {
  if (t.tag != TermTag::BVar) return t;
  std::uint32_t k = t.id;
  const EnvFrame* e = env;
  while (e && k) { e = e->next; --k; }
  if (!e) return t; // compile-time bug or malformed term
  return Term::var(e->var_id);
}

inline Term eval_probe_arg(Term t, const State& st) {
  // Resolve bound vars then substitution
  t = resolve_bvar(t, st.env);
  t = walk(t, st.subst);
  return t;
}

// ============================================================================
// Ground-enough check used by probe(req_ground=true).
// Strict v1 rules:
//   - Any unbound Var => Insufficient
//   - Any BVar that cannot be resolved by current env => Insufficient
//   - Fresh/Probe nodes in the goal => Insufficient (introduces vars / meta)
//   - Allocation failure while checking => Bounded
// ============================================================================

struct TermJob { Term t; TermJob* next; };
struct GoalJob { const Goal* g; GoalJob* next; };

inline Outcome ground_check_term(Arena& a, Term t, const State& st) {
  TermJob* jobs = nullptr;

  auto push = [&](Term x) -> bool {
    TermJob* j = a.make<TermJob>();
    if (!j) return false;
    j->t = x;
    j->next = jobs;
    jobs = j;
    return true;
  };

  if (!push(t)) return Outcome::Bounded;

  while (jobs) {
    TermJob* j = jobs;
    jobs = jobs->next;

    Term x = j->t;

    // Resolve BVar using env, but if it's still BVar after resolution, treat as insufficient.
    if (x.tag == TermTag::BVar) {
      Term r = resolve_bvar(x, st.env);
      if (r.tag == TermTag::BVar) return Outcome::Insufficient;
      x = r;
    }

    // Apply substitution
    x = walk(x, st.subst);

    // After walking, any remaining Var is unbound => not ground
    if (x.tag == TermTag::Var) return Outcome::Insufficient;

    if (x.tag == TermTag::Pair) {
      const PairNode* p = x.pair;
      if (!p) return Outcome::Insufficient;
      if (!push(p->car)) return Outcome::Bounded;
      if (!push(p->cdr)) return Outcome::Bounded;
      continue;
    }

    // Int/Sym/Nil are ground; ignore others
  }

  return Outcome::True;
}

inline Outcome ground_check_goal(Arena& a, const Goal* g0, const State& st) {
  GoalJob* jobs = nullptr;

  auto push = [&](const Goal* g) -> bool {
    GoalJob* j = a.make<GoalJob>();
    if (!j) return false;
    j->g = g;
    j->next = jobs;
    jobs = j;
    return true;
  };

  if (!g0) return Outcome::Insufficient;
  if (!push(g0)) return Outcome::Bounded;

  while (jobs) {
    GoalJob* j = jobs;
    jobs = jobs->next;

    const Goal* g = j->g;
    if (!g) return Outcome::Insufficient;

    switch (g->tag) {
      case GoalTag::Eq: {
        Outcome ou = ground_check_term(a, g->eq.u, st);
        if (ou != Outcome::True) return ou;
        Outcome ov = ground_check_term(a, g->eq.v, st);
        if (ov != Outcome::True) return ov;
        break;
      }

      case GoalTag::Disj:
      case GoalTag::Conj: {
        if (!push(g->bin.g1)) return Outcome::Bounded;
        if (!push(g->bin.g2)) return Outcome::Bounded;
        break;
      }

      case GoalTag::Fresh:
        // Strict: fresh introduces new vars, so not ground enough for now
        return Outcome::Insufficient;

      case GoalTag::Probe:
        // Strict: do not allow meta inside meta under req_ground for now
        return Outcome::Insufficient;

      default:
        return Outcome::Insufficient;
    }
  }

  return Outcome::True;
}


// ---- Iterative unify (no recursion) ----
struct UnifyJob { Term u; Term v; UnifyJob* next; };
static_assert(std::is_trivially_destructible_v<UnifyJob>);

inline bool unify(Arena& a, Term u0, Term v0, const EnvFrame* env, const Binding*& s) {
  UnifyJob* jobs = nullptr;

	auto push = [&](Term u, Term v) -> bool {
	  UnifyJob* j = a.make<UnifyJob>();
	  if (!j) return false;
	  j->u = u; j->v = v; j->next = jobs;
	  jobs = j;
	  return true;
	};

  if (!push(u0, v0)) return false;

  while (jobs) {
    UnifyJob* j = jobs;
    jobs = jobs->next;

    Term u = walk(resolve_bvar(j->u, env), s);
    Term v = walk(resolve_bvar(j->v, env), s);

    if (u.tag == TermTag::Var) {
      s = ext_s(a, s, u.id, v);
      if (!s) return false;
      continue;
    }
    if (v.tag == TermTag::Var) {
      s = ext_s(a, s, v.id, u);
      if (!s) return false;
      continue;
    }

    if (u.tag != v.tag) return false;

    switch (u.tag) {
      case TermTag::Nil:
        break;
      case TermTag::Int:
        if (u.value != v.value) return false;
        break;
      case TermTag::Sym:
        if (u.sym != v.sym) return false; // interned pointer identity
        break;
      case TermTag::Pair: {
        const PairNode* pu = u.pair;
        const PairNode* pv = v.pair;
        if (!pu || !pv) return false;
        if (!push(pu->cdr, pv->cdr)) return false;
        if (!push(pu->car, pv->car)) return false;
        break;
      }
      default:
        return false;
    }
  }

  return true;
}



static_assert(std::is_trivially_destructible_v<Goal>);

inline const Goal* make_eq(Arena& a, Term u, Term v) {
  Goal* g = a.make<Goal>();
  if (!g) return nullptr;
  g->tag = GoalTag::Eq;
  g->eq = GoalEq{u, v};
  return g;
}

inline const Goal* make_disj(Arena& a, const Goal* g1, const Goal* g2) {
  Goal* g = a.make<Goal>();
  if (!g) return nullptr;
  g->tag = GoalTag::Disj;
  g->bin = GoalBin{g1, g2};
  return g;
}

inline const Goal* make_conj(Arena& a, const Goal* g1, const Goal* g2) {
  Goal* g = a.make<Goal>();
  if (!g) return nullptr;
  g->tag = GoalTag::Conj;
  g->bin = GoalBin{g1, g2};
  return g;
}

inline const Goal* make_fresh(Arena& a, std::uint32_t n, const Goal* body) {
  Goal* g = a.make<Goal>();
  if (!g) return nullptr;
  g->tag = GoalTag::Fresh;
  g->fresh = GoalFresh{n, body};
  return g;
}

inline const Goal* make_probe(
    Arena& a,
    const Goal* sub,
    Term condition,
    Term max_iter,
    Term sandbox,
    Term req_ground
) {
  Goal* g = a.make<Goal>();
  if (!g) return nullptr;
  g->tag = GoalTag::Probe;
  g->probe = GoalProbe{sub, condition, max_iter, sandbox, req_ground};
  return g;
}

// ---- Fair evaluator: explicit continuation + FIFO work queue ----
enum class KontTag : std::uint8_t { Done, Then };
struct KontThen { const Goal* g2; const struct Kont* next; };

struct Kont {
  KontTag tag;
  union { KontThen then_; };
};

static_assert(std::is_trivially_destructible_v<Kont>);

inline const Kont* kont_done(Arena& a) {
  Kont* k = a.make<Kont>();
  if (!k) return nullptr;
  k->tag = KontTag::Done;
  return k;
}

inline const Kont* kont_then(Arena& a, const Goal* g2, const Kont* next) {
  Kont* k = a.make<Kont>();
  if (!k) return nullptr;
  k->tag = KontTag::Then;
  k->then_ = KontThen{g2, next};
  return k;
}

struct Work {
  const Goal* g;
  State st;
  const Kont* k;
  Work* next;
};

static_assert(std::is_trivially_destructible_v<Work>);

struct WorkQueue {
  Work* head = nullptr;
  Work* tail = nullptr;

  void push(Work* w) {
    w->next = nullptr;
    if (!tail) head = tail = w;
    else { tail->next = w; tail = w; }
  }

  Work* pop() {
    Work* w = head;
    if (!w) return nullptr;
    head = w->next;
    if (!head) tail = nullptr;
    w->next = nullptr;
    return w;
  }
};

inline StepResult apply_k_or_yield(Arena& a, WorkQueue& q, State st, const Kont* k, Work* reuse, State& yielded) {
  if (k && k->tag == KontTag::Then) {
    Work* w = reuse ? reuse : a.make<Work>();
    if (!w) return StepResult::OOM;
    w->g = k->then_.g2;
    w->st = st;
    w->k = k->then_.next;
    q.push(w);
    return StepResult::NoYield; // not yielded yet
  }
  if (k && k->tag == KontTag::Done) {
    yielded = st;
    return StepResult::Yield; // yielded an answer
  }
  return StepResult::NoYield;
}

inline Outcome probe_run(Arena& a, const OutcomeSyms& syms, const Goal* sub, State start, std::uint32_t max_iter, State& witness_out);

inline StepResult step(Arena& a, WorkQueue& q, Work* w, State& yielded, const OutcomeSyms& syms) {
  const Goal* g = w->g;
  State st = w->st;
  const Kont* k = w->k;

  switch (g->tag) {
    case GoalTag::Eq: {
      const Binding* s = st.subst;
      if (!unify(a, g->eq.u, g->eq.v, st.env, s)) return StepResult::NoYield;
      State st2{ s, st.diseqs, st.env, st.counter };
      return apply_k_or_yield(a, q, st2, k, w, yielded);
    }

    case GoalTag::Disj: {
      // Reuse w for left branch.
      Work* w1 = w;
      w1->g = g->bin.g1; w1->st = st; w1->k = k;
      q.push(w1);

      // Allocate one Work for right branch.
      Work* w2 = a.make<Work>();
      if (!w2) return StepResult::OOM;
      w2->g = g->bin.g2; w2->st = st; w2->k = k;
      q.push(w2);
      return StepResult::NoYield;
    }

    case GoalTag::Conj: {
      const Kont* k2 = kont_then(a, g->bin.g2, k);
      if (!k2) return StepResult::OOM;
      w->g = g->bin.g1; w->st = st; w->k = k2;
      q.push(w);
      return StepResult::NoYield;
    }

    case GoalTag::Fresh: {
	// allocate n fresh var ids from st.counter
	  std::uint32_t n = g->fresh.n;
	  std::uint32_t base = st.counter;
	  st.counter += n;

	  // extend env: push vars in order so the LAST becomes BVar(0)
	  const EnvFrame* env2 = st.env;
	  for (std::uint32_t i = 0; i < n; ++i) {
	    EnvFrame* ef = a.make<EnvFrame>();
	    if (!ef) return StepResult::OOM;
	    ef->var_id = base + i;
	    ef->next = env2;
	    env2 = ef;
	  }

	// continue with body under extended env
	State st2{ st.subst, st.diseqs, env2, st.counter };

      // Continue with body under extended env.
      w->g = g->fresh.body;
      w->st = st2;
      w->k = k;
      q.push(w);
      return StepResult::NoYield;
    }

	case GoalTag::Probe: {
	  // Evaluate/resolve args (must already be values if Vars are used)
	  Term cond_t = eval_probe_arg(g->probe.condition, st);
	  Term max_t  = eval_probe_arg(g->probe.max_iter, st);
	  Term sand_t = eval_probe_arg(g->probe.sandbox, st);
	  Term reqg_t = eval_probe_arg(g->probe.req_ground, st);

	  // Parse condition enum
	  Outcome want{};
	  const SymEntry* cond_sym = (cond_t.tag == TermTag::Sym) ? cond_t.sym : nullptr;
	  if (!cond_sym || !sym_to_outcome(cond_sym, syms, want)) {
	    // ill-formed condition => insufficient
	    Outcome got = Outcome::Insufficient;
	    if (got != want) return StepResult::NoYield; // if want wasn't parseable, treat as fail
	    // If want itself wasn't parseable, we can't match; fail.
	    return StepResult::NoYield;
	  }

	  // Parse max_iter
	  if (max_t.tag != TermTag::Int) {
	    // max_iter not an integer => insufficient
	    Outcome got = Outcome::Insufficient;
	    if (got == want) return apply_k_or_yield(a, q, st, k, w, yielded);
	    return StepResult::NoYield;
	  }
	  std::uint32_t max_iter = (max_t.value < 0) ? 0u : (std::uint32_t)max_t.value;

	  // Parse booleans (symbols "true"/"false")
	  bool sandbox = false;
	  bool req_ground = false;

	  if (sand_t.tag != TermTag::Sym || (sand_t.sym != syms.s_true && sand_t.sym != syms.s_false)) {
	    Outcome got = Outcome::Insufficient;
	    if (got == want) return apply_k_or_yield(a, q, st, k, w, yielded);
	    return StepResult::NoYield;
	  }
	  sandbox = (sand_t.sym == syms.s_true);

	  if (reqg_t.tag != TermTag::Sym || (reqg_t.sym != syms.s_true && reqg_t.sym != syms.s_false)) {
	    Outcome got = Outcome::Insufficient;
	    if (got == want) return apply_k_or_yield(a, q, st, k, w, yielded);
	    return StepResult::NoYield;
	  }
	  req_ground = (reqg_t.sym == syms.s_true);

	  if (req_ground) {
      Outcome gr = ground_check_goal(a, g->probe.sub, st);
      if (gr != Outcome::True) {
        Outcome got = gr; // Insufficient or Bounded
        if (got == want) return apply_k_or_yield(a, q, st, k, w, yielded);
        return StepResult::NoYield;
      }
    }

	  // Run bounded probe
	  State witness{};
	  Outcome got = probe_run(a, syms, g->probe.sub, st, max_iter, witness);

	  // Success iff outcomes match
	  if (got != want) return StepResult::NoYield;

	  // Commit witness only when:
	  // - got==True (must have witness)
	  // - sandbox==false
	  if (!sandbox && got == Outcome::True) {
	    State st2 = witness;
	    return apply_k_or_yield(a, q, st2, k, w, yielded);
	  }

	  // Otherwise, succeed without changing state
	  return apply_k_or_yield(a, q, st, k, w, yielded);
	}

    default:
      return StepResult::NoYield;
  }
}

inline Term reify(Term t, const Binding* s) { return walk(t, s); }


inline Outcome probe_run(Arena& a, const OutcomeSyms& syms, const Goal* sub, State start, std::uint32_t max_iter, State& witness_out) {
  WorkQueue q;
  const Kont* kd = kont_done(a);
  if (!kd) return Outcome::Bounded; // treat as resource bound

  Work* w0 = a.make<Work>();
  if (!w0) return Outcome::Bounded;

  w0->g = sub;
  w0->st = start;
  w0->k = kd;
  q.push(w0);

  for (std::uint32_t i = 0; i < max_iter; ++i) {
    Work* w = q.pop();
    if (!w) return Outcome::False; // exhausted search

    State y{};
    StepResult r = step(a, q, w, y, syms);
    if (r == StepResult::Yield) {
      witness_out = y;
      return Outcome::True;
    }
    if (r == StepResult::OOM) return Outcome::Bounded;
    // else NoYield: continue
  }

  return Outcome::Bounded;
}

template<class OnAnswer>
inline void runN(Arena& a, int n, const Goal* query_goal, Term query_var,
                 std::uint32_t vars_used, const OutcomeSyms& syms, OnAnswer&& on_answer) {
  WorkQueue q;
  const Kont* kd = kont_done(a);
  if (!kd) return;

  State st0{ nullptr, nullptr, nullptr, vars_used };

  Work* w0 = a.make<Work>();
  if (!w0) return;
  w0->g = query_goal; w0->st = st0; w0->k = kd;
  q.push(w0);

  int produced = 0;
  while (produced < n) {
    Work* w = q.pop();
    if (!w) break;

    State y{};
    StepResult r = step(a, q, w, y, syms);
    if (StepResult::Yield == r) {
      Term ans = reify(query_var, y.subst);
      on_answer(ans, y);
      ++produced;
    };
    if (StepResult::OOM == r) break;
  }
}
