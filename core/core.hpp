#pragma once
#include "mktypes.hpp"
#include "arena.hpp"
#include "intern.hpp"
#include <cstdint>

enum class StepResult : std::uint8_t {
  NoYield,
  Yield,
  OOM,
  NotHandled,   // returned only by handleKnownRelation: "I didn't recognize this name"
};

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
enum class GoalTag : std::uint8_t { Eq, Disj, Conj, Fresh, Probe, Call, Diseq };

struct GoalEq    { Term u; Term v; };
struct GoalDiseq { Term u; Term v; };
static_assert(std::is_trivially_destructible_v<GoalDiseq>);
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
    GoalDiseq diseq;  // Diseq (=/=)
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

// STAGE_ARITH: extended constraint store.
// A constraint fires (causes failure) when its condition is true.
// ConstraintRel::Eq is used for =/= : fire if walk(u) == walk(v) + offset.
enum class ConstraintRel : std::uint8_t {
  Eq,   // fires when walk(u) == walk(v) + offset  (used by =/=)
  Ne,   // fires when walk(u) != walk(v) + offset
  Lt,   // fires when walk(u) <  walk(v) + offset
  Le,   // fires when walk(u) <= walk(v) + offset
  Gt,   // fires when walk(u) >  walk(v) + offset
  Ge,   // fires when walk(u) >= walk(v) + offset
};

struct Constraint {
  Term              u;
  Term              v;
  std::int64_t      offset;
  ConstraintRel     rel;
  const Constraint* next;
};
static_assert(std::is_trivially_destructible_v<Constraint>);

// Runtime environment stack for bound vars (de Bruijn):
struct EnvFrame {
  std::uint32_t   var_id;
  const EnvFrame* next;
};
static_assert(std::is_trivially_destructible_v<EnvFrame>);

struct State {
  const Binding*    subst;
  const Constraint* constraints;  // STAGE_ARITH: renamed from diseqs
  const EnvFrame*   env;
  std::uint32_t     counter;
  std::uint32_t     client_offset;  // Stage 0B: saved/restored with State on backtrack
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
      case GoalTag::Diseq:
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

// ============================================================================
// walk_deep: recursively walk t through substitution s, allocating new
// PairNodes in arena for any Pair whose children are walked.
// Assumes BVars have already been resolved (via deep_resolve_bvar) so only
// Var → substitution lookup is needed here.
// Used to fully ground compound Call arguments before dispatch.
// ============================================================================
inline Term walk_deep(Arena& a, Term t, const Binding* s, const RelEnv& rel_env) {
  t = walk(t, s, rel_env);
  if (t.tag != TermTag::Pair || !t.pair) return t;
  PairNode* p = a.make<PairNode>();
  if (!p) return t;  // OOM — return partially-walked term; unify fails downstream
  p->car = walk_deep(a, t.pair->car, s, rel_env);
  p->cdr = walk_deep(a, t.pair->cdr, s, rel_env);
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

inline const Goal* make_diseq(Arena& a, Term u, Term v) {
  Goal* g = a.make<Goal>();
  if (!g) return nullptr;
  g->tag   = GoalTag::Diseq;
  g->diseq = GoalDiseq{u, v};
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


// ============================================================================
// deep_copy_term: deep-copies a term into dest, rewriting PairNode* pointers.
// Sym/Rel/scalar terms are returned as-is (their payloads are stable pointers).
// ============================================================================
inline Term deep_copy_term(Arena& dest, Term t) {
  switch (t.tag) {
    case TermTag::Nil:
    case TermTag::Int:
    case TermTag::Sym:
    case TermTag::Var:
    case TermTag::BVar:
    case TermTag::Rel:
      return t;
    case TermTag::Pair: {
      if (!t.pair) return t;
      PairNode* p = dest.make<PairNode>();
      if (!p) return Term::nil();
      p->car = deep_copy_term(dest, t.pair->car);
      p->cdr = deep_copy_term(dest, t.pair->cdr);
      return Term::make_pair(p);
    }
    default:
      return Term::nil();
  }
}

// ============================================================================
// deep_copy_goal: deep-copies a Goal tree from its arena into dest.
// Required to persist defrel bodies across query_arena resets.
// Recursively copies all Goal* and Term fields; RelNode bodies in Rel terms
// are also copied so they don't dangle when the source arena is reset.
// ============================================================================
inline const Goal* deep_copy_goal(Arena& dest, const Goal* g) {
  if (!g) return nullptr;
  Goal* copy = dest.make<Goal>();
  if (!copy) return nullptr;
  copy->tag = g->tag;
  switch (g->tag) {
    case GoalTag::Eq:
      copy->eq.u = deep_copy_term(dest, g->eq.u);
      copy->eq.v = deep_copy_term(dest, g->eq.v);
      break;
    case GoalTag::Diseq:
      copy->diseq.u = deep_copy_term(dest, g->diseq.u);
      copy->diseq.v = deep_copy_term(dest, g->diseq.v);
      break;
    case GoalTag::Disj:
    case GoalTag::Conj:
      copy->bin.g1 = deep_copy_goal(dest, g->bin.g1);
      copy->bin.g2 = deep_copy_goal(dest, g->bin.g2);
      break;
    case GoalTag::Fresh:
      copy->fresh.n    = g->fresh.n;
      copy->fresh.body = deep_copy_goal(dest, g->fresh.body);
      break;
    case GoalTag::Probe:
      copy->probe.sub        = deep_copy_goal(dest, g->probe.sub);
      copy->probe.condition  = deep_copy_term(dest, g->probe.condition);
      copy->probe.max_iter   = deep_copy_term(dest, g->probe.max_iter);
      copy->probe.sandbox    = deep_copy_term(dest, g->probe.sandbox);
      copy->probe.req_ground = deep_copy_term(dest, g->probe.req_ground);
      break;
    case GoalTag::Call: {
      Term rt = g->call.rel_term;
      // Deep-copy RelNode bodies reachable via Rel terms so they survive
      // a source-arena reset.
      if (rt.tag == TermTag::Rel && rt.rel) {
        RelNode* rn = dest.make<RelNode>();
        if (rn) {
          rn->param_count = rt.rel->param_count;
          rn->body        = deep_copy_goal(dest, rt.rel->body);
          rt = Term::relation(rn);
        }
      }
      copy->call.rel_term  = rt;
      copy->call.arg_count = g->call.arg_count;
      if (g->call.arg_count > 0 && g->call.args) {
        Term* args = static_cast<Term*>(
            dest.alloc(g->call.arg_count * sizeof(Term), alignof(Term)));
        if (args) {
          for (std::uint32_t i = 0; i < g->call.arg_count; ++i)
            args[i] = deep_copy_term(dest, g->call.args[i]);
        }
        copy->call.args = args;
      } else {
        copy->call.args = nullptr;
      }
      break;
    }
    default:
      break;
  }
  return copy;
}


// ---- Fair evaluator: explicit continuation + FIFO work queue ----
//
// KontTag::RestoreEnv is used by GoalTag::Call to restore the CALLER's env
// when transitioning from the callee back into the caller's continuation.
// Without this, the callee's env would persist into the caller's continuation
// goals, causing BVar indices compiled in the caller's scope to resolve against
// the callee's env chain (wrong variables).
enum class KontTag : std::uint8_t { Done, Then, RestoreEnv };
struct KontThen       { const Goal*       g2;  const struct Kont* next; };
struct KontRestoreEnv { const EnvFrame*   env; const struct Kont* next; };

struct Kont {
  KontTag tag;
  union { KontThen then_; KontRestoreEnv restore_env_; };
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

inline const Kont* kont_restore_env(Arena& a, const EnvFrame* env,
                                    const Kont* next) {
  Kont* k = a.make<Kont>();
  if (!k) return nullptr;
  k->tag          = KontTag::RestoreEnv;
  k->restore_env_ = KontRestoreEnv{env, next};
  return k;
}

struct Work {
  const Goal* g;
  State       st;
  const Kont* k;
  Work*       next;
};
static_assert(std::is_trivially_destructible_v<Work>);

// WorkQueue: FIFO (breadth-first) queue.
// push appends to the back (tail); pop takes from the front (head).
// Standard miniKanren BFS interleaving strategy: ensures completeness.
struct WorkQueue {
  Work* head = nullptr;  // front — pop from here
  Work* tail = nullptr;  // back  — push to here

  void push(Work* w) {
    w->next = nullptr;
    if (tail) tail->next = w; else head = w;
    tail = w;
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
  // RestoreEnv: adjust st.env to the saved caller env before continuing.
  // This is set by GoalTag::Call to ensure the caller's continuation goals
  // see BVar indices resolved against the caller's scope, not the callee's.
  while (k && k->tag == KontTag::RestoreEnv) {
    st.env = k->restore_env_.env;
    k = k->restore_env_.next;
  }
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
// check_constraints: verify no active constraint fires under substitution s.
// A constraint fires when its condition evaluates to true (causing failure).
// Returns true if all constraints hold (none fire); false if any fires.
// ============================================================================
inline bool check_constraints(Arena& a, const Constraint* cs,
                               const Binding* s, const RelEnv& rel_env) {
  for (const Constraint* c = cs; c; c = c->next) {
    Term u = walk(c->u, s, rel_env);
    Term v = walk(c->v, s, rel_env);

    // If either side is still an unbound Var, defer — not yet checkable.
    if (u.tag == TermTag::Var || v.tag == TermTag::Var) continue;

    // Both sides are ground.
    if (u.tag == TermTag::Int && v.tag == TermTag::Int) {
      std::int64_t uval = static_cast<std::int64_t>(u.value);
      std::int64_t vval = static_cast<std::int64_t>(v.value) + c->offset;
      bool fires = false;
      switch (c->rel) {
        case ConstraintRel::Eq: fires = (uval == vval); break;
        case ConstraintRel::Ne: fires = (uval != vval); break;
        case ConstraintRel::Lt: fires = (uval <  vval); break;
        case ConstraintRel::Le: fires = (uval <= vval); break;
        case ConstraintRel::Gt: fires = (uval >  vval); break;
        case ConstraintRel::Ge: fires = (uval >= vval); break;
      }
      if (fires) return false;
      continue;
    }

    // Both sides ground non-integers: only Eq (from =/=) is meaningful.
    if (c->rel == ConstraintRel::Eq) {
      const Binding* s2 = s;
      bool unified = unify(a, u, v, nullptr, s2, rel_env);
      if (unified && s2 == s) return false;  // already equal — fires
    }
    // Ne/Lt/Le/Gt/Ge on non-integer ground terms: skip (undefined behavior).
  }
  return true;
}

// ============================================================================
// Evaluator: base class for the µKanren engine (Stage 0B)
//
// step() and runN() live here as methods.
// handleUnknownRelation is the single protected virtual extension point.
// handleKnownRelation is a non-virtual method for core built-ins; it is
// called by step() before handleUnknownRelation and cannot be overridden.
// ============================================================================
class Evaluator {
public:
  // Legacy 2-arg constructor for backward compatibility.
  // (Used by test_extension.cpp, the backward-compat runN free function, etc.)
  // With no Intern provided, arithmetic built-ins are unavailable.
  Evaluator(Arena* arena, const OutcomeSyms* syms)
    : Evaluator(arena, arena, nullptr, syms) {}

  // Full constructor.
  // working_arena:  per-query scratch space (may be reset between queries).
  // sym_arena:      stable arena for built-in name interning and charo symbols.
  //                 Pass a permanent arena (e.g. intern_arena) when the
  //                 Evaluator outlives individual query_arena resets.
  // intern:         symbol table used to intern built-in names and charo chars.
  // syms:           outcome symbols for probe evaluation.
  Evaluator(Arena* working_arena, Arena* sym_arena, Intern* intern,
            const OutcomeSyms* syms)
    : arena_(working_arena), sym_arena_(sym_arena), intern_(intern), syms_(syms)
  {
    client_region_.id       = ClientId::None;
    client_region_.base     = nullptr;
    client_region_.capacity = 0;
    client_region_.offset   = 0;
    // Intern all nine built-in arithmetic/comparison relation names once.
    // With intern==nullptr (legacy path) all sym pointers stay nullptr and
    // handleKnownRelation never matches — arithmetic is simply unavailable.
    if (intern_ && sym_arena_) {
      sym_leqo_       = intern_cstr(*sym_arena_, *intern_, "leqo");
      sym_lto_        = intern_cstr(*sym_arena_, *intern_, "lto");
      sym_geqo_       = intern_cstr(*sym_arena_, *intern_, "geqo");
      sym_gto_        = intern_cstr(*sym_arena_, *intern_, "gto");
      sym_eqo_        = intern_cstr(*sym_arena_, *intern_, "eqo");
      sym_neqo_       = intern_cstr(*sym_arena_, *intern_, "neqo");
      sym_addsubo_    = intern_cstr(*sym_arena_, *intern_, "addsubo");
      sym_multaddiso_ = intern_cstr(*sym_arena_, *intern_, "multaddiso");
      sym_charo_      = intern_cstr(*sym_arena_, *intern_, "charo");
      sym_boundo_     = intern_cstr(*sym_arena_, *intern_, "boundo");
      sym_rel_arityo_ = intern_cstr(*sym_arena_, *intern_, "rel-arityo");
    }
  }

  // Non-copyable, non-movable.
  Evaluator(const Evaluator&)            = delete;
  Evaluator& operator=(const Evaluator&) = delete;

  virtual ~Evaluator() = default;

  // Run up to n answers, calling on_answer(Term, State) for each solution.
  // If oom_occurred is non-null, *oom_occurred is set to true when the loop
  // terminates due to arena exhaustion rather than a legitimately empty queue.
  template<typename OnAnswer>
  void runN(int n, const Goal* goal, Term qvar,
            std::uint32_t vars_used, const RelEnv& rel_env,
            OnAnswer&& on_answer,
            bool* oom_occurred = nullptr);

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
  Arena*             sym_arena_;  // STAGE_ARITH: for stable symbol allocation
  Intern*            intern_;     // STAGE_ARITH: for charo and built-in name interning
  const OutcomeSyms* syms_;
  ClientRegion       client_region_;

private:
  // ---- Core built-in dispatch (non-virtual; called before handleUnknownRelation) ----
  // Returns NotHandled if the name is not a known built-in.
  // Returns NoYield/OOM/Yield/apply_k_or_yield result when handled.
  StepResult handleKnownRelation(
      const SymEntry* name, const Term* args, std::uint32_t arg_count,
      State& st, Arena& a, WorkQueue& q,
      const Kont* k, Work* w, State& yielded, const RelEnv& rel_env);

  // ---- Built-in handlers (each returns NoYield on failure, or apply_k_or_yield) ----
  StepResult handle_leqo      (const Term* args, std::uint32_t ac, State& st, Arena& a, WorkQueue& q, const Kont* k, Work* w, State& y, const RelEnv& re);
  StepResult handle_lto       (const Term* args, std::uint32_t ac, State& st, Arena& a, WorkQueue& q, const Kont* k, Work* w, State& y, const RelEnv& re);
  StepResult handle_geqo      (const Term* args, std::uint32_t ac, State& st, Arena& a, WorkQueue& q, const Kont* k, Work* w, State& y, const RelEnv& re);
  StepResult handle_gto       (const Term* args, std::uint32_t ac, State& st, Arena& a, WorkQueue& q, const Kont* k, Work* w, State& y, const RelEnv& re);
  StepResult handle_eqo       (const Term* args, std::uint32_t ac, State& st, Arena& a, WorkQueue& q, const Kont* k, Work* w, State& y, const RelEnv& re);
  StepResult handle_neqo      (const Term* args, std::uint32_t ac, State& st, Arena& a, WorkQueue& q, const Kont* k, Work* w, State& y, const RelEnv& re);
  StepResult handle_addsubo   (const Term* args, std::uint32_t ac, State& st, Arena& a, WorkQueue& q, const Kont* k, Work* w, State& y, const RelEnv& re);
  StepResult handle_multaddiso(const Term* args, std::uint32_t ac, State& st, Arena& a, WorkQueue& q, const Kont* k, Work* w, State& y, const RelEnv& re);
  StepResult handle_charo     (const Term* args, std::uint32_t ac, State& st, Arena& a, WorkQueue& q, const Kont* k, Work* w, State& y, const RelEnv& re);
  StepResult handle_boundo    (const Term* args, std::uint32_t ac, State& st, Arena& a, WorkQueue& q, const Kont* k, Work* w, State& y, const RelEnv& re);
  StepResult handle_rel_arityo(const Term* args, std::uint32_t ac, State& st, Arena& a, WorkQueue& q, const Kont* k, Work* w, State& y, const RelEnv& re);

  // Helper: add a constraint and continue.
  StepResult add_constraint(Term u, Term v, std::int64_t offset, ConstraintRel rel,
                             State& st, Arena& a, WorkQueue& q,
                             const Kont* k, Work* w, State& yielded);

  // Helper: unify one term with another and continue (used by eqo/addsubo/etc.).
  StepResult unify_and_continue(Term lhs, Term rhs,
                                 State& st, Arena& a, WorkQueue& q,
                                 const Kont* k, Work* w, State& yielded,
                                 const RelEnv& rel_env);

  // Built-in name pointers (interned at construction; nullptr if intern==nullptr).
  const SymEntry* sym_leqo_       = nullptr;
  const SymEntry* sym_lto_        = nullptr;
  const SymEntry* sym_geqo_       = nullptr;
  const SymEntry* sym_gto_        = nullptr;
  const SymEntry* sym_eqo_        = nullptr;
  const SymEntry* sym_neqo_       = nullptr;
  const SymEntry* sym_addsubo_    = nullptr;
  const SymEntry* sym_multaddiso_ = nullptr;
  const SymEntry* sym_charo_      = nullptr;
  const SymEntry* sym_boundo_     = nullptr;
  const SymEntry* sym_rel_arityo_ = nullptr;

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


// ============================================================================
// STAGE_ARITH: built-in handler helpers
// ============================================================================

// add_constraint: record a constraint and continue with the updated state.
inline StepResult Evaluator::add_constraint(
    Term u, Term v, std::int64_t offset, ConstraintRel rel,
    State& st, Arena& a, WorkQueue& q,
    const Kont* k, Work* w, State& yielded)
{
  Constraint* c = a.make<Constraint>();
  if (!c) return StepResult::OOM;
  c->u      = u;
  c->v      = v;
  c->offset = offset;
  c->rel    = rel;
  c->next   = st.constraints;
  State st2{ st.subst, c, st.env, st.counter, st.client_offset };
  return apply_k_or_yield(a, q, st2, k, w, yielded);
}

// unify_and_continue: unify lhs with rhs, check constraints, and continue.
inline StepResult Evaluator::unify_and_continue(
    Term lhs, Term rhs,
    State& st, Arena& a, WorkQueue& q,
    const Kont* k, Work* w, State& yielded,
    const RelEnv& rel_env)
{
  const Binding* s = st.subst;
  if (!unify(a, lhs, rhs, nullptr, s, rel_env)) return StepResult::NoYield;
  if (!check_constraints(a, st.constraints, s, rel_env)) return StepResult::NoYield;
  State st2{ s, st.constraints, st.env, st.counter, st.client_offset };
  return apply_k_or_yield(a, q, st2, k, w, yielded);
}

// handleKnownRelation: dispatch to built-in handlers.
inline StepResult Evaluator::handleKnownRelation(
    const SymEntry* name, const Term* args, std::uint32_t arg_count,
    State& st, Arena& a, WorkQueue& q,
    const Kont* k, Work* w, State& yielded, const RelEnv& rel_env)
{
  if (name == sym_leqo_)       return handle_leqo      (args, arg_count, st, a, q, k, w, yielded, rel_env);
  if (name == sym_lto_)        return handle_lto       (args, arg_count, st, a, q, k, w, yielded, rel_env);
  if (name == sym_geqo_)       return handle_geqo      (args, arg_count, st, a, q, k, w, yielded, rel_env);
  if (name == sym_gto_)        return handle_gto       (args, arg_count, st, a, q, k, w, yielded, rel_env);
  if (name == sym_eqo_)        return handle_eqo       (args, arg_count, st, a, q, k, w, yielded, rel_env);
  if (name == sym_neqo_)       return handle_neqo      (args, arg_count, st, a, q, k, w, yielded, rel_env);
  if (name == sym_addsubo_)    return handle_addsubo   (args, arg_count, st, a, q, k, w, yielded, rel_env);
  if (name == sym_multaddiso_) return handle_multaddiso(args, arg_count, st, a, q, k, w, yielded, rel_env);
  if (name == sym_charo_)      return handle_charo     (args, arg_count, st, a, q, k, w, yielded, rel_env);
  if (name == sym_boundo_)     return handle_boundo    (args, arg_count, st, a, q, k, w, yielded, rel_env);
  if (name == sym_rel_arityo_) return handle_rel_arityo(args, arg_count, st, a, q, k, w, yielded, rel_env);
  return StepResult::NotHandled;
}

// ============================================================================
// Comparison built-ins
// ============================================================================

// leqo: (leqo a b) means a <= b
inline StepResult Evaluator::handle_leqo(
    const Term* args, std::uint32_t ac,
    State& st, Arena& a, WorkQueue& q, const Kont* k, Work* w, State& y,
    const RelEnv& re)
{
  if (ac != 2) return StepResult::NoYield;
  Term ua = args[0]; Term ub = args[1];
  bool a_bound = (ua.tag == TermTag::Int);
  bool b_bound = (ub.tag == TermTag::Int);
  bool a_var   = (ua.tag == TermTag::Var);
  bool b_var   = (ub.tag == TermTag::Var);
  if (a_bound && b_bound) {
    if (static_cast<std::int64_t>(ua.value) <= static_cast<std::int64_t>(ub.value))
      return apply_k_or_yield(a, q, st, k, w, y);
    return StepResult::NoYield;
  }
  if (a_var && b_bound)  return add_constraint(ua, ub, 0, ConstraintRel::Gt,  st, a, q, k, w, y);
  if (b_var && a_bound)  return add_constraint(ub, ua, 0, ConstraintRel::Lt,  st, a, q, k, w, y);
  (void)re; (void)b_var; (void)a_var;
  return StepResult::NoYield;
}

// lto: (lto a b) means a < b
inline StepResult Evaluator::handle_lto(
    const Term* args, std::uint32_t ac,
    State& st, Arena& a, WorkQueue& q, const Kont* k, Work* w, State& y,
    const RelEnv& re)
{
  if (ac != 2) return StepResult::NoYield;
  Term ua = args[0]; Term ub = args[1];
  bool a_bound = (ua.tag == TermTag::Int);
  bool b_bound = (ub.tag == TermTag::Int);
  bool a_var   = (ua.tag == TermTag::Var);
  bool b_var   = (ub.tag == TermTag::Var);
  if (a_bound && b_bound) {
    if (static_cast<std::int64_t>(ua.value) < static_cast<std::int64_t>(ub.value))
      return apply_k_or_yield(a, q, st, k, w, y);
    return StepResult::NoYield;
  }
  if (a_var && b_bound)  return add_constraint(ua, ub, 0, ConstraintRel::Ge,  st, a, q, k, w, y);
  if (b_var && a_bound)  return add_constraint(ub, ua, 0, ConstraintRel::Le,  st, a, q, k, w, y);
  (void)re; (void)b_var; (void)a_var;
  return StepResult::NoYield;
}

// geqo: (geqo a b) means a >= b
inline StepResult Evaluator::handle_geqo(
    const Term* args, std::uint32_t ac,
    State& st, Arena& a, WorkQueue& q, const Kont* k, Work* w, State& y,
    const RelEnv& re)
{
  if (ac != 2) return StepResult::NoYield;
  Term ua = args[0]; Term ub = args[1];
  bool a_bound = (ua.tag == TermTag::Int);
  bool b_bound = (ub.tag == TermTag::Int);
  bool a_var   = (ua.tag == TermTag::Var);
  bool b_var   = (ub.tag == TermTag::Var);
  if (a_bound && b_bound) {
    if (static_cast<std::int64_t>(ua.value) >= static_cast<std::int64_t>(ub.value))
      return apply_k_or_yield(a, q, st, k, w, y);
    return StepResult::NoYield;
  }
  if (a_var && b_bound)  return add_constraint(ua, ub, 0, ConstraintRel::Lt,  st, a, q, k, w, y);
  if (b_var && a_bound)  return add_constraint(ub, ua, 0, ConstraintRel::Gt,  st, a, q, k, w, y);
  (void)re; (void)b_var; (void)a_var;
  return StepResult::NoYield;
}

// gto: (gto a b) means a > b
inline StepResult Evaluator::handle_gto(
    const Term* args, std::uint32_t ac,
    State& st, Arena& a, WorkQueue& q, const Kont* k, Work* w, State& y,
    const RelEnv& re)
{
  if (ac != 2) return StepResult::NoYield;
  Term ua = args[0]; Term ub = args[1];
  bool a_bound = (ua.tag == TermTag::Int);
  bool b_bound = (ub.tag == TermTag::Int);
  bool a_var   = (ua.tag == TermTag::Var);
  bool b_var   = (ub.tag == TermTag::Var);
  if (a_bound && b_bound) {
    if (static_cast<std::int64_t>(ua.value) > static_cast<std::int64_t>(ub.value))
      return apply_k_or_yield(a, q, st, k, w, y);
    return StepResult::NoYield;
  }
  if (a_var && b_bound)  return add_constraint(ua, ub, 0, ConstraintRel::Le,  st, a, q, k, w, y);
  if (b_var && a_bound)  return add_constraint(ub, ua, 0, ConstraintRel::Ge,  st, a, q, k, w, y);
  (void)re; (void)b_var; (void)a_var;
  return StepResult::NoYield;
}

// eqo: (eqo a b) means a == b (numeric).
// With one unbound, reduces to unification (same as ==).
inline StepResult Evaluator::handle_eqo(
    const Term* args, std::uint32_t ac,
    State& st, Arena& a, WorkQueue& q, const Kont* k, Work* w, State& y,
    const RelEnv& re)
{
  if (ac != 2) return StepResult::NoYield;
  Term ua = args[0]; Term ub = args[1];
  bool a_int = (ua.tag == TermTag::Int);
  bool b_int = (ub.tag == TermTag::Int);
  bool a_var = (ua.tag == TermTag::Var);
  bool b_var = (ub.tag == TermTag::Var);
  if (a_int && b_int) {
    if (ua.value == ub.value) return apply_k_or_yield(a, q, st, k, w, y);
    return StepResult::NoYield;
  }
  // One unbound: unify (eqo with one unbound reduces to ==)
  if (a_var && b_int) return unify_and_continue(ua, ub, st, a, q, k, w, y, re);
  if (b_var && a_int) return unify_and_continue(ub, ua, st, a, q, k, w, y, re);
  return StepResult::NoYield;  // both unbound, or non-integers
}

// neqo: (neqo a b) means a != b (numeric).
// With one unbound, records an Eq constraint (same as =/= for integers).
inline StepResult Evaluator::handle_neqo(
    const Term* args, std::uint32_t ac,
    State& st, Arena& a, WorkQueue& q, const Kont* k, Work* w, State& y,
    const RelEnv& re)
{
  if (ac != 2) return StepResult::NoYield;
  Term ua = args[0]; Term ub = args[1];
  bool a_int = (ua.tag == TermTag::Int);
  bool b_int = (ub.tag == TermTag::Int);
  bool a_var = (ua.tag == TermTag::Var);
  bool b_var = (ub.tag == TermTag::Var);
  if (a_int && b_int) {
    if (ua.value != ub.value) return apply_k_or_yield(a, q, st, k, w, y);
    return StepResult::NoYield;
  }
  if (a_var && b_int) return add_constraint(ua, ub, 0, ConstraintRel::Eq, st, a, q, k, w, y);
  if (b_var && a_int) return add_constraint(ub, ua, 0, ConstraintRel::Eq, st, a, q, k, w, y);
  (void)re; (void)b_var; (void)a_var;
  return StepResult::NoYield;
}

// ============================================================================
// addsubo: (addsubo a b c) means a + b = c
// ============================================================================
inline StepResult Evaluator::handle_addsubo(
    const Term* args, std::uint32_t ac,
    State& st, Arena& a, WorkQueue& q, const Kont* k, Work* w, State& y,
    const RelEnv& re)
{
  if (ac != 3) return StepResult::NoYield;
  Term ta = args[0]; Term tb = args[1]; Term tc = args[2];
  bool a_var = (ta.tag == TermTag::Var);
  bool b_var = (tb.tag == TermTag::Var);
  bool c_var = (tc.tag == TermTag::Var);
  bool a_int = (ta.tag == TermTag::Int);
  bool b_int = (tb.tag == TermTag::Int);
  bool c_int = (tc.tag == TermTag::Int);

  int unbound = (a_var ? 1 : 0) + (b_var ? 1 : 0) + (c_var ? 1 : 0);
  if (unbound >= 2) return StepResult::NoYield;

  if (a_int && b_int && c_int) {
    std::int64_t av = ta.value, bv = tb.value, cv = tc.value;
    if (av + bv == cv) return apply_k_or_yield(a, q, st, k, w, y);
    return StepResult::NoYield;
  }
  if (c_var && a_int && b_int) {
    std::int32_t result = static_cast<std::int32_t>(
        static_cast<std::int64_t>(ta.value) + static_cast<std::int64_t>(tb.value));
    return unify_and_continue(tc, Term::integer(result), st, a, q, k, w, y, re);
  }
  if (a_var && b_int && c_int) {
    std::int32_t result = static_cast<std::int32_t>(
        static_cast<std::int64_t>(tc.value) - static_cast<std::int64_t>(tb.value));
    return unify_and_continue(ta, Term::integer(result), st, a, q, k, w, y, re);
  }
  if (b_var && a_int && c_int) {
    std::int32_t result = static_cast<std::int32_t>(
        static_cast<std::int64_t>(tc.value) - static_cast<std::int64_t>(ta.value));
    return unify_and_continue(tb, Term::integer(result), st, a, q, k, w, y, re);
  }
  return StepResult::NoYield;  // non-integer ground term
}

// ============================================================================
// multaddiso: (multaddiso a b c d) means a * b + c = d
// ============================================================================
inline StepResult Evaluator::handle_multaddiso(
    const Term* args, std::uint32_t ac,
    State& st, Arena& a, WorkQueue& q, const Kont* k, Work* w, State& y,
    const RelEnv& re)
{
  if (ac != 4) return StepResult::NoYield;
  Term ta = args[0]; Term tb = args[1]; Term tc = args[2]; Term td = args[3];
  bool a_var = (ta.tag == TermTag::Var);
  bool b_var = (tb.tag == TermTag::Var);
  bool c_var = (tc.tag == TermTag::Var);
  bool d_var = (td.tag == TermTag::Var);
  bool a_int = (ta.tag == TermTag::Int);
  bool b_int = (tb.tag == TermTag::Int);
  bool c_int = (tc.tag == TermTag::Int);
  bool d_int = (td.tag == TermTag::Int);

  int unbound = (a_var ? 1 : 0) + (b_var ? 1 : 0) + (c_var ? 1 : 0) + (d_var ? 1 : 0);
  if (unbound >= 2) return StepResult::NoYield;

  // Ensure any ground arg is actually Int.
  if (!a_var && !a_int) return StepResult::NoYield;
  if (!b_var && !b_int) return StepResult::NoYield;
  if (!c_var && !c_int) return StepResult::NoYield;
  if (!d_var && !d_int) return StepResult::NoYield;

  std::int64_t av = a_int ? static_cast<std::int64_t>(ta.value) : 0;
  std::int64_t bv = b_int ? static_cast<std::int64_t>(tb.value) : 0;
  std::int64_t cv = c_int ? static_cast<std::int64_t>(tc.value) : 0;
  std::int64_t dv = d_int ? static_cast<std::int64_t>(td.value) : 0;

  if (a_int && b_int && c_int && d_int) {
    if (av * bv + cv == dv) return apply_k_or_yield(a, q, st, k, w, y);
    return StepResult::NoYield;
  }
  if (d_var) { // a, b, c bound
    std::int32_t result = static_cast<std::int32_t>(av * bv + cv);
    return unify_and_continue(td, Term::integer(result), st, a, q, k, w, y, re);
  }
  if (c_var) { // a, b, d bound
    std::int32_t result = static_cast<std::int32_t>(dv - av * bv);
    return unify_and_continue(tc, Term::integer(result), st, a, q, k, w, y, re);
  }
  if (a_var) { // b, c, d bound
    if (bv == 0) return StepResult::NoYield;  // division by zero / unconstrained
    std::int64_t num = dv - cv;
    if (num % bv != 0) return StepResult::NoYield;  // not exactly divisible
    std::int32_t result = static_cast<std::int32_t>(num / bv);
    return unify_and_continue(ta, Term::integer(result), st, a, q, k, w, y, re);
  }
  if (b_var) { // a, c, d bound
    if (av == 0) return StepResult::NoYield;  // division by zero / unconstrained
    std::int64_t num = dv - cv;
    if (num % av != 0) return StepResult::NoYield;  // not exactly divisible
    std::int32_t result = static_cast<std::int32_t>(num / av);
    return unify_and_continue(tb, Term::integer(result), st, a, q, k, w, y, re);
  }
  return StepResult::NoYield;
}

// ============================================================================
// charo: (charo c n) — c is a single-char symbol, n is its ASCII int value
// ============================================================================
inline StepResult Evaluator::handle_charo(
    const Term* args, std::uint32_t ac,
    State& st, Arena& a, WorkQueue& q, const Kont* k, Work* w, State& y,
    const RelEnv& re)
{
  if (ac != 2) return StepResult::NoYield;
  Term tc = args[0]; Term tn = args[1];
  bool c_var = (tc.tag == TermTag::Var);
  bool n_var = (tn.tag == TermTag::Var);
  bool c_sym = (tc.tag == TermTag::Sym);
  bool n_int = (tn.tag == TermTag::Int);

  if (c_sym && n_int) {
    // Both bound: verify
    if (!tc.sym || tc.sym->len != 1) return StepResult::NoYield;
    std::int64_t n_val = static_cast<std::int64_t>(tn.value);
    std::int64_t c_val = static_cast<std::int64_t>(
        static_cast<unsigned char>(tc.sym->str[0]));
    if (n_val == c_val) return apply_k_or_yield(a, q, st, k, w, y);
    return StepResult::NoYield;
  }
  if (c_sym && n_var) {
    // c bound (sym), n unbound: compute n = ASCII(c[0])
    if (!tc.sym || tc.sym->len != 1) return StepResult::NoYield;
    std::int32_t n_val = static_cast<std::int32_t>(
        static_cast<unsigned char>(tc.sym->str[0]));
    return unify_and_continue(tn, Term::integer(n_val), st, a, q, k, w, y, re);
  }
  if (n_int && c_var) {
    // n bound, c unbound: look up character
    std::int64_t n_val = static_cast<std::int64_t>(tn.value);
    if (n_val < 1 || n_val > 127) return StepResult::NoYield;
    if (!intern_ || !sym_arena_) return StepResult::NoYield;
    char buf[2] = { static_cast<char>(static_cast<std::uint8_t>(n_val)), '\0' };
    const SymEntry* s = intern_cstr(*sym_arena_, *intern_, buf);
    if (!s) return StepResult::NoYield;
    return unify_and_continue(tc, Term::symbol(s), st, a, q, k, w, y, re);
  }
  if (c_var && n_var) {
    // Both unbound: enumerate printable ASCII 32..126
    if (!intern_ || !sym_arena_) return StepResult::NoYield;
    for (int n = 32; n <= 126; ++n) {
      char buf[2] = { static_cast<char>(n), '\0' };
      const SymEntry* s = intern_cstr(*sym_arena_, *intern_, buf);
      if (!s) continue;
      // Build goal: (conj (== tc sym_n) (== tn int_n))
      const Goal* g_c    = make_eq(a, tc, Term::symbol(s));
      const Goal* g_n    = make_eq(a, tn, Term::integer(n));
      if (!g_c || !g_n) return StepResult::OOM;
      const Goal* g_both = make_conj(a, g_c, g_n);
      if (!g_both) return StepResult::OOM;
      Work* wi = a.make<Work>();
      if (!wi) return StepResult::OOM;
      wi->g  = g_both;
      wi->st = st;
      wi->k  = k;
      q.push(wi);
    }
    // All 95 work items pushed; no single success here.
    return StepResult::NoYield;
  }
  (void)re;
  return StepResult::NoYield;
}

// ============================================================================
// boundo: (boundo x) — succeeds if x is bound (not an unbound Var)
// ============================================================================
inline StepResult Evaluator::handle_boundo(
    const Term* args, std::uint32_t ac,
    State& st, Arena& a, WorkQueue& q, const Kont* k, Work* w, State& y,
    const RelEnv& re)
{
  if (ac != 1) return StepResult::NoYield;
  // args[0] is already walk_deep'd; an unbound var stays as TermTag::Var.
  if (args[0].tag == TermTag::Var) return StepResult::NoYield;
  (void)re;
  return apply_k_or_yield(a, q, st, k, w, y);
}

// ============================================================================
// rel-arityo: (rel-arityo rel-term n)
// Unifies n with the param_count of rel-term. Reads a public structural fact;
// does not involve closures or captured values.
// ============================================================================
inline StepResult Evaluator::handle_rel_arityo(
    const Term* args, std::uint32_t ac,
    State& st, Arena& a, WorkQueue& q, const Kont* k, Work* w, State& y,
    const RelEnv& re)
{
  if (ac != 2) return StepResult::NoYield;
  Term rel_t = resolve_bvar(args[0], st.env);
  rel_t = walk(rel_t, st.subst, RelEnv{});
  if (rel_t.tag != TermTag::Rel || !rel_t.rel) return StepResult::NoYield;

  Term n     = Term::integer(static_cast<std::int32_t>(rel_t.rel->param_count));
  Term n_out = resolve_bvar(args[1], st.env);
  n_out = walk(n_out, st.subst, RelEnv{});
  return unify_and_continue(n_out, n, st, a, q, k, w, y, re);
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
      if (!check_constraints(a, st.constraints, s, rel_env)) return StepResult::NoYield;
      State st2{ s, st.constraints, st.env, st.counter, st.client_offset };
      return apply_k_or_yield(a, q, st2, k, w, yielded);
    }

    case GoalTag::Disj: {
      // BFS (FIFO): push g1 first so it is dequeued before g2.
      Work* w2 = a.make<Work>();
      if (!w2) return StepResult::OOM;
      w2->g = g->bin.g2; w2->st = st; w2->k = k;

      Work* w1 = w;
      w1->g = g->bin.g1; w1->st = st; w1->k = k;

      q.push(w1);
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

      // Wrap k with RestoreEnv so that when the fresh body finishes and
      // apply_k_or_yield fires the outer continuation, st.env is restored to
      // the pre-fresh scope. Without this, the fresh body's extended env (with
      // the newly introduced vars) persists into the outer continuation, causing
      // BVar indices compiled in the outer scope to resolve against the wrong env.
      const Kont* wrapped_k = kont_restore_env(a, st.env, k);
      if (!wrapped_k) return StepResult::OOM;
      State st2{ st.subst, st.constraints, env2, st.counter, st.client_offset };
      w->g  = g->fresh.body;
      w->st = st2;
      w->k  = wrapped_k;
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
            // Not in RelEnv: try core built-ins, then virtual extension point.
            // Deep-resolve BVars → Vars, then walk Vars through the
            // substitution, so handlers receive fully-grounded terms.
            Term* resolved = static_cast<Term*>(
                a.alloc(g->call.arg_count * sizeof(Term), alignof(Term)));
            if (!resolved) return StepResult::OOM;
            for (std::uint32_t ri = 0; ri < g->call.arg_count; ++ri) {
              Term t = deep_resolve_bvar(a, g->call.args[ri], st.env);
              resolved[ri] = walk_deep(a, t, st.subst, rel_env);
            }
            // Try core built-ins (non-virtual, cannot be overridden).
            {
              StepResult sr = handleKnownRelation(
                  rel_t.sym, resolved, g->call.arg_count, st,
                  a, q, k, w, yielded, rel_env);
              if (sr != StepResult::NotHandled) return sr;
            }
            // Fall through to virtual extension point (Stage 0B).
            StepResult sr = handleUnknownRelation(
                rel_t.sym, resolved, g->call.arg_count, st);
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
      // deep_resolve_bvar replaces BVar(k) → Var(env[k]) throughout the full
      // term tree (not just the top level), so compound args like
      // (q strong-qid (check+ H T R)) have all inner BVars converted to
      // runtime Vars before we pass them to unify (which uses env=nullptr).
      const Binding* s = st.subst;
      for (std::uint32_t i = 0; i < rel->param_count; ++i) {
        Term arg   = deep_resolve_bvar(a, g->call.args[i], st.env);
        arg        = walk(arg, s, rel_env);
        Term param = Term::var(base + i);
        if (!unify(a, param, arg, nullptr, s, rel_env))
          return StepResult::NoYield;
      }

      // Continue with body under fresh closed env.
      // Wrap k with RestoreEnv so that when the callee's body finishes and
      // apply_k_or_yield fires the caller's continuation, st.env is restored
      // to the caller's scope. Without this, the callee's body_env would
      // persist into the continuation, causing BVar indices compiled in the
      // caller's scope to resolve against the wrong env chain.
      const Kont* wrapped_k = kont_restore_env(a, st.env, k);
      if (!wrapped_k) return StepResult::OOM;
      State st2{ s, st.constraints, body_env, st.counter, st.client_offset };
      w->g  = rel->body;
      w->st = st2;
      w->k  = wrapped_k;
      q.push(w);
      return StepResult::NoYield;
    }

    case GoalTag::Diseq: {
      // Deep-resolve BVars throughout both sides (handles compound terms like
      // (check H T) where H and T are inner BVars), then shallow-walk Vars.
      Term u = deep_resolve_bvar(a, g->diseq.u, st.env);
      Term v = deep_resolve_bvar(a, g->diseq.v, st.env);
      u = walk(u, st.subst, rel_env);
      v = walk(v, st.subst, rel_env);

      // Attempt unification to determine the constraint's status.
      const Binding* s2 = st.subst;
      bool ok = unify(a, u, v, nullptr, s2, rel_env);

      if (!ok) {
        // u and v can never be equal — constraint trivially satisfied.
        return apply_k_or_yield(a, q, st, k, w, yielded);
      }

      if (s2 == st.subst) {
        // No new bindings needed: u and v already equal — violated immediately.
        return StepResult::NoYield;
      }

      // Constraint not yet violated; record it for future equality checks.
      Constraint* c = a.make<Constraint>();
      if (!c) return StepResult::OOM;
      c->u      = u;
      c->v      = v;
      c->offset = 0;
      c->rel    = ConstraintRel::Eq;
      c->next   = st.constraints;

      State st2{ st.subst, c, st.env, st.counter, st.client_offset };
      return apply_k_or_yield(a, q, st2, k, w, yielded);
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
                             OnAnswer&& on_answer,
                             bool* oom_occurred)
{
  Arena& a = *arena_;
  WorkQueue q;
  const Kont* kd = kont_done(a);
  if (!kd) return;

  // Initialize state. client_offset reflects the current region offset
  // (typically 0 for a fresh evaluator, or whatever was allocated before).
  // Second nullptr is for const Constraint* constraints (empty at start).
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
    if (StepResult::OOM == r) {
      if (oom_occurred) *oom_occurred = true;
      break;
    }
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
  // Uses 2-arg Evaluator constructor (no intern → arithmetic unavailable).
  // security_test.cpp calls this form and cannot be modified.
  Evaluator eval(&a, &syms);
  eval.runN(n, query_goal, query_var, vars_used, RelEnv{},
            std::forward<OnAnswer>(on_answer));
}
