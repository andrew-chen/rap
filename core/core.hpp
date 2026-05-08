#pragma once
#include "mktypes.hpp"
#include "arena.hpp"
#include "intern.hpp"
#include <cstdint>

enum class StepResult : std::uint8_t { NoYield, Yield, OOM };

// Maximum bytes in the client region (Stage 0B).
// 16 KiB is sufficient for all paper examples.
constexpr std::uint32_t MAX_CHANGESET_ARENA = 16 * 1024;
// Maximum number of ChangeSet operations in a single query result (Stage 0B).
constexpr std::uint32_t MAX_CHANGESET_OPS   = 64;

// ---- Terms ----
enum class TermTag : std::uint8_t { Var, BVar, Int, Sym, Nil, Pair, Rel };

// RelNode: anonymous relation value (Stage 0A)
struct RelNode {
  std::uint32_t param_count;  // number of parameters
  const Goal*   body;         // compiled body (uses BVar for parameters)
};
static_assert(std::is_trivially_destructible_v<RelNode>);

struct Term {
  TermTag tag;
  union {
    std::uint32_t   id;        // Var or BVar
    std::int32_t    value;     // Int
    const SymEntry* sym;       // Sym
    const PairNode* pair;      // Pair
    const RelNode*  rel;       // Rel (Stage 0A)
  };

  static Term var(std::uint32_t v)         { Term t; t.tag = TermTag::Var;  t.id    = v; return t; }
  static Term bvar(std::uint32_t k)        { Term t; t.tag = TermTag::BVar; t.id    = k; return t; }
  static Term integer(std::int32_t v)      { Term t; t.tag = TermTag::Int;  t.value = v; return t; }
  static Term symbol(const SymEntry* s)    { Term t; t.tag = TermTag::Sym;  t.sym   = s; return t; }
  static Term nil()                        { Term t; t.tag = TermTag::Nil;  t.id    = 0; return t; }
  static Term make_pair(const PairNode* p) { Term t; t.tag = TermTag::Pair; t.pair  = p; return t; }
  static Term relation(const RelNode* r)   { Term t; t.tag = TermTag::Rel;  t.rel   = r; return t; }
};
static_assert(std::is_trivially_destructible_v<Term>);

struct PairNode { Term car; Term cdr; };
static_assert(std::is_trivially_destructible_v<PairNode>);


// ---- Goals ----
enum class GoalTag : std::uint8_t { Eq, Disj, Conj, Fresh, Probe, Call };

struct GoalEq    { Term u; Term v; };
struct GoalBin   { const struct Goal* g1; const struct Goal* g2; };
struct GoalFresh { std::uint32_t n; const Goal* body; };

struct GoalProbe {
  const struct Goal* sub;
  Term condition;   // must be Sym or Var already bound to Sym (true/false/insufficient/bounded)
  Term max_iter;    // must be Int or Var already bound to Int
  Term sandbox;     // must be Sym(true/false) or Var already bound to Sym
  Term req_ground;  // must be Sym(true/false) or Var already bound to Sym
};

// GoalCall: invoke a relation term with arguments (Stage 0A)
struct GoalCall {
  Term          rel_term;    // Rel, Var, BVar, or Sym — all resolved at runtime
  const Term*   args;        // arena-allocated array of argument terms
  std::uint32_t arg_count;
};
static_assert(std::is_trivially_destructible_v<GoalCall>);

struct Goal {
  GoalTag tag;
  union {
    GoalEq    eq;
    GoalBin   bin;
    GoalFresh fresh;
    GoalProbe probe;
    GoalCall  call;   // Stage 0A
  };
};
static_assert(std::is_trivially_destructible_v<Goal>);


// ============================================================================
// 4-valued outcome used by probe/meta-evaluation
// ============================================================================
enum class Outcome : std::uint8_t { True, False, Insufficient, Bounded };

struct OutcomeSyms {
  const SymEntry* s_true         = nullptr;
  const SymEntry* s_false        = nullptr;
  const SymEntry* s_insufficient = nullptr;
  const SymEntry* s_bounded      = nullptr;
};

inline const SymEntry* outcome_to_sym(Outcome o, const OutcomeSyms& syms) {
  switch (o) {
    case Outcome::True:         return syms.s_true;
    case Outcome::False:        return syms.s_false;
    case Outcome::Insufficient: return syms.s_insufficient;
    case Outcome::Bounded:      return syms.s_bounded;
    default:                    return nullptr;
  }
}

inline bool sym_to_outcome(const SymEntry* s, const OutcomeSyms& syms, Outcome& out) {
  if (s == syms.s_true)        { out = Outcome::True;        return true; }
  if (s == syms.s_false)       { out = Outcome::False;       return true; }
  if (s == syms.s_insufficient){ out = Outcome::Insufficient; return true; }
  if (s == syms.s_bounded)     { out = Outcome::Bounded;     return true; }
  return false;
}


// ============================================================================
// ClientId and ClientRegion (Stage 0B): per-evaluator extension region
// The core's only obligations: save offset when saving a choice point,
// restore offset when backtracking. Never read/write/interpret the bytes.
// Never reference ClientId::RAP by name inside core/.
// ============================================================================
enum class ClientId : std::uint32_t {
  None = 0,   // uninitialized / no client registered
  RAP  = 1,   // Relational Agenda Programming layer (rap/)
  // Future clients: add new entries here. Never reuse or renumber.
};

struct ClientRegion {
  ClientId       id       = ClientId::None;
  std::uint8_t*  base     = nullptr;
  std::uint32_t  capacity = 0;
  std::uint32_t  offset   = 0;   // saved/restored on backtrack via State

  void* alloc(std::uint32_t n) {
    if (offset + n > capacity) return nullptr;
    void* p = base + offset;
    offset += n;
    return p;
  }

  void restore(std::uint32_t saved_offset) {
    offset = saved_offset;
  }
};
static_assert(std::is_trivially_destructible_v<ClientRegion>);


// ============================================================================
// Core µKanren-ish kernel (arena-only; POD terms; iterative unify; fair eval)
// ============================================================================

// ---- Substitution / State ----
struct Binding {
  std::uint32_t var;
  Term          value;
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
struct EnvFrame {
  std::uint32_t   var_id;
  const EnvFrame* next;
};
static_assert(std::is_trivially_destructible_v<EnvFrame>);

struct State {
  const Binding*  subst;
  const Diseq*    diseqs;
  const EnvFrame* env;
  std::uint32_t   counter;
  std::uint32_t   client_offset;  // Stage 0B: saved/restored with State on backtrack
};
static_assert(std::is_trivially_destructible_v<State>);


// ============================================================================
// Global relation environment (Stage 0A)
// Populated by defrel during parsing; passed to step() for runtime lookup.
// ============================================================================
struct RelEnvEntry {
  const SymEntry*    name;
  Term               rel_term;    // TermTag::Rel
  const RelEnvEntry* next;
};
static_assert(std::is_trivially_destructible_v<RelEnvEntry>);

struct RelEnv {
  const RelEnvEntry* head = nullptr;

  // Define a named relation. Also sets rel_cache on the SymEntry for O(1) lookup.
  void define(Arena& a, const SymEntry* name, Term rel_term) {
    RelEnvEntry* e = a.make<RelEnvEntry>();
    if (!e) return;  // OOM
    e->name     = name;
    e->rel_term = rel_term;
    e->next     = head;
    head        = e;
    // Set inline cache for O(1) runtime lookup.
    if (rel_term.tag == TermTag::Rel)
      const_cast<SymEntry*>(name)->rel_cache = rel_term.rel;
  }

  // Linear scan fallback (used only on cache miss).
  Term lookup(const SymEntry* name) const {
    for (auto* e = head; e; e = e->next)
      if (e->name == name) return e->rel_term;
    return Term::nil();
  }
};
static_assert(std::is_trivially_destructible_v<RelEnv>);


// ============================================================================
// walk: follows Var chains, then resolves Sym through rel_env (Stage 0A)
// ============================================================================
inline Term walk(Term t, const Binding* s, const RelEnv& rel_env) {
  while (t.tag == TermTag::Var) {
    const Binding* cur = s;
    bool found = false;
    while (cur) {
      if (cur->var == t.id) { t = cur->value; found = true; break; }
      cur = cur->next;
    }
    if (!found) break;
  }
  // Extended (Stage 0A): if result is a named Sym, resolve through rel_env.
  if (t.tag == TermTag::Sym && t.sym) {
    Term found = rel_env.lookup(t.sym);
    if (found.tag == TermTag::Rel) return found;
  }
  return t;
}

inline const Binding* ext_s(Arena& a, const Binding* s, std::uint32_t v, Term val) {
  Binding* b = a.make<Binding>();
  if (!b) return nullptr;
  b->var   = v;
  b->value = val;
  b->next  = s;
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

inline Term eval_probe_arg(Term t, const State& st, const RelEnv& rel_env) {
  t = resolve_bvar(t, st.env);
  t = walk(t, st.subst, rel_env);
  return t;
}

// ============================================================================
// Ground-enough check used by probe(req_ground=true).
// ============================================================================

struct TermJob { Term t; TermJob* next; };
struct GoalJob { const Goal* g; GoalJob* next; };

inline Outcome ground_check_term(Arena& a, Term t, const State& st, const RelEnv& rel_env) {
  TermJob* jobs = nullptr;

  auto push = [&](Term x) -> bool {
    TermJob* j = a.make<TermJob>();
    if (!j) return false;
    j->t    = x;
    j->next = jobs;
    jobs    = j;
    return true;
  };

  if (!push(t)) return Outcome::Bounded;

  while (jobs) {
    TermJob* j = jobs;
    jobs = jobs->next;

    Term x = j->t;

    if (x.tag == TermTag::BVar) {
      Term r = resolve_bvar(x, st.env);
      if (r.tag == TermTag::BVar) return Outcome::Insufficient;
      x = r;
    }

    x = walk(x, st.subst, rel_env);

    if (x.tag == TermTag::Var) return Outcome::Insufficient;

    if (x.tag == TermTag::Pair) {
      const PairNode* p = x.pair;
      if (!p) return Outcome::Insufficient;
      if (!push(p->car)) return Outcome::Bounded;
      if (!push(p->cdr)) return Outcome::Bounded;
      continue;
    }
    // Int/Sym/Nil/Rel are ground; fall through
  }

  return Outcome::True;
}

inline Outcome ground_check_goal(Arena& a, const Goal* g0, const State& st, const RelEnv& rel_env) {
  GoalJob* jobs = nullptr;

  auto push = [&](const Goal* g) -> bool {
    GoalJob* j = a.make<GoalJob>();
    if (!j) return false;
    j->g    = g;
    j->next = jobs;
    jobs    = j;
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
        Outcome ou = ground_check_term(a, g->eq.u, st, rel_env);
        if (ou != Outcome::True) return ou;
        Outcome ov = ground_check_term(a, g->eq.v, st, rel_env);
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
        return Outcome::Insufficient;

      case GoalTag::Probe:
        return Outcome::Insufficient;

      case GoalTag::Call:
        return Outcome::Insufficient;

      default:
        return Outcome::Insufficient;
    }
  }

  return Outcome::True;
}


// ============================================================================
// deep_resolve_bvar: recursively replace BVar(k) → Var(env[k]) throughout a term.
// Required to keep the substitution free of BVar terms (which are compile-time
// indices whose env context is lost after the enclosing scope exits).
// ============================================================================
inline Term deep_resolve_bvar(Arena& a, Term t, const EnvFrame* env) {
  if (t.tag == TermTag::BVar) return resolve_bvar(t, env);
  if (t.tag != TermTag::Pair || !t.pair) return t;  // Int/Sym/Nil/Var/Rel: unchanged
  Term car = deep_resolve_bvar(a, t.pair->car, env);
  Term cdr = deep_resolve_bvar(a, t.pair->cdr, env);
  PairNode* p = a.make<PairNode>();
  if (!p) return t;  // OOM — return original; unify will fail downstream
  p->car = car;
  p->cdr = cdr;
  return Term::make_pair(p);
}

// ---- Iterative unify (no recursion) ----
struct UnifyJob { Term u; Term v; UnifyJob* next; };
static_assert(std::is_trivially_destructible_v<UnifyJob>);

inline bool unify(Arena& a, Term u0, Term v0, const EnvFrame* env,
                  const Binding*& s, const RelEnv& rel_env) {
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

    Term u = walk(resolve_bvar(j->u, env), s, rel_env);
    Term v = walk(resolve_bvar(j->v, env), s, rel_env);

    if (u.tag == TermTag::Var) {
      // Deep-resolve BVars so the substitution never stores compile-time indices.
      Term val = deep_resolve_bvar(a, v, env);
      s = ext_s(a, s, u.id, val);
      if (!s) return false;
      continue;
    }
    if (v.tag == TermTag::Var) {
      Term val = deep_resolve_bvar(a, u, env);
      s = ext_s(a, s, v.id, val);
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
        if (u.sym != v.sym) return false;
        break;
      case TermTag::Rel:
        if (u.rel != v.rel) return false;
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


inline const Goal* make_eq(Arena& a, Term u, Term v) {
  Goal* g = a.make<Goal>();
  if (!g) return nullptr;
  g->tag = GoalTag::Eq;
  g->eq  = GoalEq{u, v};
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
  g->tag   = GoalTag::Fresh;
  g->fresh = GoalFresh{n, body};
  return g;
}

inline const Goal* make_probe(Arena& a, const Goal* sub, Term condition,
                               Term max_iter, Term sandbox, Term req_ground) {
  Goal* g = a.make<Goal>();
  if (!g) return nullptr;
  g->tag   = GoalTag::Probe;
  g->probe = GoalProbe{sub, condition, max_iter, sandbox, req_ground};
  return g;
}

inline const Goal* make_call(Arena& a, Term rel_term, const Term* args,
                              std::uint32_t arg_count) {
  Goal* g = a.make<Goal>();
  if (!g) return nullptr;
  g->tag  = GoalTag::Call;
  g->call = GoalCall{rel_term, args, arg_count};
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
  k->tag   = KontTag::Then;
  k->then_ = KontThen{g2, next};
  return k;
}

struct Work {
  const Goal* g;
  State       st;
  const Kont* k;
  Work*       next;
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

inline StepResult apply_k_or_yield(Arena& a, WorkQueue& q, State st,
                                   const Kont* k, Work* reuse, State& yielded) {
  if (k && k->tag == KontTag::Then) {
    Work* w = reuse ? reuse : a.make<Work>();
    if (!w) return StepResult::OOM;
    w->g  = k->then_.g2;
    w->st = st;
    w->k  = k->then_.next;
    q.push(w);
    return StepResult::NoYield;
  }
  if (k && k->tag == KontTag::Done) {
    yielded = st;
    return StepResult::Yield;
  }
  return StepResult::NoYield;
}


// ============================================================================
// Evaluator: base class for the µKanren engine (Stage 0B)
//
// step() and runN() live here as methods.
// handleUnknownRelation is the single protected virtual extension point.
// ============================================================================
class Evaluator {
public:
  // Caller provides arena and outcome syms (injected, not owned).
  // The Evaluator base class does NOT store Intern* — all symbol comparison
  // at runtime uses SymEntry* pointer identity (interning is done at parse time).
  Evaluator(Arena* arena, const OutcomeSyms* syms)
    : arena_(arena), syms_(syms)
  {
    client_region_.id       = ClientId::None;
    client_region_.base     = nullptr;
    client_region_.capacity = 0;
    client_region_.offset   = 0;
  }

  // Non-copyable, non-movable.
  Evaluator(const Evaluator&)            = delete;
  Evaluator& operator=(const Evaluator&) = delete;

  virtual ~Evaluator() = default;

  // Run up to n answers, calling on_answer(Term, State) for each solution.
  template<typename OnAnswer>
  void runN(int n, const Goal* goal, Term qvar,
            std::uint32_t vars_used, const RelEnv& rel_env,
            OnAnswer&& on_answer);

  // 6-arg overload without rel_env (backward compat for callers that don't
  // have a RelEnv, e.g. pre-Stage-0A call sites in rap/).
  template<typename OnAnswer>
  void runN(int n, const Goal* goal, Term qvar,
            std::uint32_t vars_used,
            OnAnswer&& on_answer) {
    runN(n, goal, qvar, vars_used, RelEnv{},
         std::forward<OnAnswer>(on_answer));
  }

  // Expose client region offset for testing / introspection.
  std::uint32_t client_region_offset() const { return client_region_.offset; }

protected:
  // Extension point: called when call dispatch fails to find a relation.
  // Default returns StepResult::NoYield (failure).
  //
  // CONTRACT for overrides:
  //   - Check client_region_.id before using the client region
  //   - Return StepResult::NoYield (failure) or StepResult::Yield (success)
  //   - Allocations into client_region_ are automatically rewound on backtrack
  //     (the caller updates st.client_offset and calls client_region_.restore)
  //   - Do NOT recurse infinitely; do NOT crash on unexpected ClientId values
  virtual StepResult handleUnknownRelation(
      const SymEntry* name,
      const Term*     args,
      std::uint32_t   arg_count,
      State&          st);

  Arena*             arena_;
  const OutcomeSyms* syms_;
  ClientRegion       client_region_;

private:
  StepResult step(Work* w, WorkQueue& q,
                  const RelEnv& rel_env, State& yielded);
  void probe_run(const Goal* goal, State st,
                 std::uint32_t max_iter, State& out_st,
                 const RelEnv& rel_env, Outcome& out);
};

// Default handleUnknownRelation — failure.
inline StepResult Evaluator::handleUnknownRelation(
    const SymEntry* name, const Term* args,
    std::uint32_t arg_count, State& st)
{
  (void)name; (void)args; (void)arg_count; (void)st;
  return StepResult::NoYield;
}

// probe_run defined before step because step() calls probe_run().
inline void Evaluator::probe_run(const Goal* goal, State st,
                                  std::uint32_t max_iter, State& out_st,
                                  const RelEnv& rel_env, Outcome& out)
{
  Arena& a = *arena_;
  WorkQueue q2;
  const Kont* kd = kont_done(a);
  if (!kd) { out = Outcome::Bounded; return; }

  Work* w0 = a.make<Work>();
  if (!w0) { out = Outcome::Bounded; return; }

  w0->g  = goal;
  w0->st = st;
  w0->k  = kd;
  q2.push(w0);

  for (std::uint32_t i = 0; i < max_iter; ++i) {
    Work* w = q2.pop();
    if (!w) { out = Outcome::False; return; }

    State      y{};
    StepResult r = step(w, q2, rel_env, y);
    if (r == StepResult::Yield) {
      out_st = y;
      out    = Outcome::True;
      return;
    }
    if (r == StepResult::OOM) { out = Outcome::Bounded; return; }
  }

  out = Outcome::Bounded;
}


inline StepResult Evaluator::step(Work* w, WorkQueue& q,
                                   const RelEnv& rel_env, State& yielded) {
  Arena&             a    = *arena_;
  const OutcomeSyms& syms = *syms_;
  const Goal* g = w->g;
  State       st = w->st;
  const Kont* k  = w->k;

  // Restore client region to the saved offset for this branch.
  // Rewinds any allocations made by a sibling branch after the choice point.
  client_region_.restore(st.client_offset);

  switch (g->tag) {

    case GoalTag::Eq: {
      const Binding* s = st.subst;
      if (!unify(a, g->eq.u, g->eq.v, st.env, s, rel_env)) return StepResult::NoYield;
      State st2{ s, st.diseqs, st.env, st.counter, st.client_offset };
      return apply_k_or_yield(a, q, st2, k, w, yielded);
    }

    case GoalTag::Disj: {
      Work* w1 = w;
      w1->g = g->bin.g1; w1->st = st; w1->k = k;
      q.push(w1);

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
      std::uint32_t n    = g->fresh.n;
      std::uint32_t base = st.counter;
      st.counter += n;

      const EnvFrame* env2 = st.env;
      for (std::uint32_t i = 0; i < n; ++i) {
        EnvFrame* ef = a.make<EnvFrame>();
        if (!ef) return StepResult::OOM;
        ef->var_id = base + i;
        ef->next   = env2;
        env2       = ef;
      }

      State st2{ st.subst, st.diseqs, env2, st.counter, st.client_offset };
      w->g  = g->fresh.body;
      w->st = st2;
      w->k  = k;
      q.push(w);
      return StepResult::NoYield;
    }

    case GoalTag::Probe: {
      Term cond_t = eval_probe_arg(g->probe.condition, st, rel_env);
      Term max_t  = eval_probe_arg(g->probe.max_iter,  st, rel_env);
      Term sand_t = eval_probe_arg(g->probe.sandbox,   st, rel_env);
      Term reqg_t = eval_probe_arg(g->probe.req_ground,st, rel_env);

      Outcome want{};
      const SymEntry* cond_sym = (cond_t.tag == TermTag::Sym) ? cond_t.sym : nullptr;
      if (!cond_sym || !sym_to_outcome(cond_sym, syms, want)) {
        return StepResult::NoYield;
      }

      if (max_t.tag != TermTag::Int) {
        Outcome got = Outcome::Insufficient;
        if (got == want) return apply_k_or_yield(a, q, st, k, w, yielded);
        return StepResult::NoYield;
      }
      std::uint32_t max_iter = (max_t.value < 0) ? 0u : (std::uint32_t)max_t.value;

      bool sandbox    = false;
      bool req_ground = false;

      if (sand_t.tag != TermTag::Sym ||
          (sand_t.sym != syms.s_true && sand_t.sym != syms.s_false)) {
        Outcome got = Outcome::Insufficient;
        if (got == want) return apply_k_or_yield(a, q, st, k, w, yielded);
        return StepResult::NoYield;
      }
      sandbox = (sand_t.sym == syms.s_true);

      if (reqg_t.tag != TermTag::Sym ||
          (reqg_t.sym != syms.s_true && reqg_t.sym != syms.s_false)) {
        Outcome got = Outcome::Insufficient;
        if (got == want) return apply_k_or_yield(a, q, st, k, w, yielded);
        return StepResult::NoYield;
      }
      req_ground = (reqg_t.sym == syms.s_true);

      if (req_ground) {
        Outcome gr = ground_check_goal(a, g->probe.sub, st, rel_env);
        if (gr != Outcome::True) {
          Outcome got = gr;
          if (got == want) return apply_k_or_yield(a, q, st, k, w, yielded);
          return StepResult::NoYield;
        }
      }

      State   witness{};
      Outcome got{};
      probe_run(g->probe.sub, st, max_iter, witness, rel_env, got);

      if (got != want) return StepResult::NoYield;

      if (!sandbox && got == Outcome::True) {
        return apply_k_or_yield(a, q, witness, k, w, yielded);
      }
      return apply_k_or_yield(a, q, st, k, w, yielded);
    }

    // -----------------------------------------------------------------------
    // Stage 0A: Call dispatch
    // -----------------------------------------------------------------------
    case GoalTag::Call: {
      // Step 1: Walk the relation term.
      Term rel_t = g->call.rel_term;
      rel_t = resolve_bvar(rel_t, st.env);
      rel_t = walk(rel_t, st.subst, rel_env);

      // Step 2-5: Resolve to a RelNode through the dispatch chain.
      const RelNode* rel = nullptr;

      if (rel_t.tag == TermTag::Rel) {
        rel = rel_t.rel;

      } else if (rel_t.tag == TermTag::Sym && rel_t.sym) {
        // Check inline cache first (O(1)).
        if (rel_t.sym->rel_cache) {
          rel = rel_t.sym->rel_cache;
        } else {
          // Cache miss: scan RelEnv.
          Term found = rel_env.lookup(rel_t.sym);
          if (found.tag == TermTag::Rel) {
            rel = found.rel;
          } else {
            // Not in RelEnv: virtual extension point (Stage 0B).
            StepResult sr = handleUnknownRelation(
                rel_t.sym, g->call.args, g->call.arg_count, st);
            if (sr == StepResult::NoYield) return StepResult::NoYield;
            // Relation succeeded; commit any client region allocations.
            st.client_offset = client_region_.offset;
            return apply_k_or_yield(a, q, st, k, w, yielded);
          }
        }
      } else {
        return StepResult::NoYield;
      }

      // Arity check.
      if (g->call.arg_count != rel->param_count)
        return StepResult::NoYield;

      // Allocate fresh variables for parameters.
      std::uint32_t base = st.counter;
      st.counter += rel->param_count;

      // Build fresh closed env for body (nullptr base = closed relation).
      const EnvFrame* body_env = nullptr;
      for (std::uint32_t i = 0; i < rel->param_count; ++i) {
        EnvFrame* ef = a.make<EnvFrame>();
        if (!ef) return StepResult::OOM;
        ef->var_id = base + i;
        ef->next   = body_env;
        body_env   = ef;
      }

      // Unify fresh vars with caller's arguments.
      const Binding* s = st.subst;
      for (std::uint32_t i = 0; i < rel->param_count; ++i) {
        Term arg   = resolve_bvar(g->call.args[i], st.env);
        arg        = walk(arg, s, rel_env);
        Term param = Term::var(base + i);
        if (!unify(a, param, arg, nullptr, s, rel_env))
          return StepResult::NoYield;
      }

      // Continue with body under fresh closed env.
      State st2{ s, st.diseqs, body_env, st.counter, st.client_offset };
      w->g  = rel->body;
      w->st = st2;
      w->k  = k;
      q.push(w);
      return StepResult::NoYield;
    }

    default:
      return StepResult::NoYield;
  }
}

inline Term reify(Term t, const Binding* s, const RelEnv& rel_env) {
  return walk(t, s, rel_env);
}

// Deep reify: recursively substitute all Vars embedded in a term.
// Required when answers are compound terms (e.g., (1 2 3 4) built by appendo)
// where inner Vars are bound in the substitution but not reached by a flat walk.
inline Term reify_deep(Arena& a, Term t, const Binding* s, const RelEnv& rel_env) {
  t = walk(t, s, rel_env);
  if (t.tag == TermTag::Pair && t.pair) {
    Term car = reify_deep(a, t.pair->car, s, rel_env);
    Term cdr = reify_deep(a, t.pair->cdr, s, rel_env);
    PairNode* p = a.make<PairNode>();
    if (!p) return t;  // OOM: return partially-reified term
    p->car = car;
    p->cdr = cdr;
    return Term::make_pair(p);
  }
  return t;
}


// Evaluator::runN — primary implementation.
template<typename OnAnswer>
inline void Evaluator::runN(int n, const Goal* query_goal, Term query_var,
                             std::uint32_t vars_used, const RelEnv& rel_env,
                             OnAnswer&& on_answer)
{
  Arena& a = *arena_;
  WorkQueue q;
  const Kont* kd = kont_done(a);
  if (!kd) return;

  // Initialize state. client_offset reflects the current region offset
  // (typically 0 for a fresh evaluator, or whatever was allocated before).
  State st0{ nullptr, nullptr, nullptr, vars_used, client_region_.offset };

  Work* w0 = a.make<Work>();
  if (!w0) return;
  w0->g = query_goal; w0->st = st0; w0->k = kd;
  q.push(w0);

  int produced = 0;
  while (produced < n) {
    Work* w = q.pop();
    if (!w) break;

    State      y{};
    StepResult r = step(w, q, rel_env, y);
    if (StepResult::Yield == r) {
      Term ans = reify_deep(a, query_var, y.subst, rel_env);
      on_answer(ans, y);
      ++produced;
    }
    if (StepResult::OOM == r) break;
  }
}

// ============================================================================
// Backward-compat free function for security/security_test.cpp
// (that file cannot be modified; it calls runN as a free function with 7 args)
// ============================================================================
template<class OnAnswer>
inline void runN(Arena& a, int n, const Goal* query_goal, Term query_var,
                 std::uint32_t vars_used, const OutcomeSyms& syms,
                 OnAnswer&& on_answer) {
  Evaluator eval(&a, &syms);
  eval.runN(n, query_goal, query_var, vars_used, RelEnv{},
            std::forward<OnAnswer>(on_answer));
}
