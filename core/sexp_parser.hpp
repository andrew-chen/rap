#pragma once
#include "mktypes.hpp"
#include "core.hpp"
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

// ============================================================================
// Tokenizer (no allocations) + iterative S-expression parser + compiler
// Stage 0A adds:
//   (rel (params...) body)  — anonymous relation term
//   (call f arg...)         — relation invocation goal
//   (defrel (name params...) body) — top-level named relation sugar
// ============================================================================

enum class TokTag : std::uint8_t { LParen, RParen, Dot, Int, Sym, End, Error };

struct Token {
  TokTag        tag;
  const char*   start;
  std::uint32_t len;
  std::int32_t  ival;
};
static_assert(std::is_trivially_destructible_v<Token>);

struct Lexer {
  const char* p;

  static bool is_space(char c) { return c==' ' || c=='\t' || c=='\n' || c=='\r'; }
  static bool is_digit(char c) { return c>='0' && c<='9'; }
  static bool is_delim(char c) {
    return is_space(c) || c=='(' || c==')' || c=='.' || c==';' || c=='\0';
  }

  Token next() {
    for (;;) {
      while (is_space(*p)) ++p;
      if (*p == ';') { while (*p && *p != '\n') ++p; continue; }
      break;
    }

    if (*p == '\0') return Token{TokTag::End,    p,   0, 0};
    if (*p == '(')  { ++p; return Token{TokTag::LParen, p-1, 1, 0}; }
    if (*p == ')')  { ++p; return Token{TokTag::RParen, p-1, 1, 0}; }
    if (*p == '.')  { ++p; return Token{TokTag::Dot,    p-1, 1, 0}; }

    const char* s = p;
    bool neg = false;
    if (*p == '-') { neg = true; ++p; }

    if (is_digit(*p)) {
      std::int32_t v = 0;
      while (is_digit(*p)) { v = v*10 + (*p - '0'); ++p; }
      if (is_delim(*p)) {
        if (neg) v = -v;
        return Token{TokTag::Int, s, (std::uint32_t)(p - s), v};
      }
      p = s;
    } else {
      p = s;
    }

    const char* b = p;
    while (!is_delim(*p)) ++p;
    if (p == b) return Token{TokTag::Error, p, 0, 0};
    return Token{TokTag::Sym, b, (std::uint32_t)(p - b), 0};
  }
};

// ============================================================================
// S-expressions (arena-only)
// ============================================================================
struct Sexp;
struct SexpCell { const Sexp* car; const SexpCell* cdr; };

enum class SexpTag : std::uint8_t { Int, Sym, List };
struct SexpListData { const SexpCell* head; const Sexp* dotted; };

struct Sexp {
  SexpTag tag;
  union {
    std::int32_t    i;
    const SymEntry* sym;
    SexpListData    list;
  };
};
static_assert(std::is_trivially_destructible_v<Sexp>);
static_assert(std::is_trivially_destructible_v<SexpCell>);

struct ListFrame {
  const SexpCell* head;
  const SexpCell* tail;
  bool            seen_dot;
  const Sexp*     dotted;
  ListFrame*      prev;
};
static_assert(std::is_trivially_destructible_v<ListFrame>);

inline const Sexp* make_sexp_int(Arena& a, std::int32_t v) {
  Sexp* x = a.make<Sexp>();
  if (!x) return nullptr;
  x->tag = SexpTag::Int; x->i = v;
  return x;
}

inline const Sexp* make_sexp_sym(Arena& a, const SymEntry* s) {
  Sexp* x = a.make<Sexp>();
  if (!x) return nullptr;
  x->tag = SexpTag::Sym; x->sym = s;
  return x;
}

inline const Sexp* make_sexp_list(Arena& a, const SexpCell* head, const Sexp* dotted) {
  Sexp* x = a.make<Sexp>();
  if (!x) return nullptr;
  x->tag = SexpTag::List; x->list = SexpListData{head, dotted};
  return x;
}

inline SexpCell* make_cell(Arena& a, const Sexp* car) {
  SexpCell* c = a.make<SexpCell>();
  if (!c) return nullptr;
  c->car = car; c->cdr = nullptr;
  return c;
}

// ============================================================================
// print_sexp
// ============================================================================
inline void print_sexp(const Sexp* x);

inline void print_sexp_list(const SexpCell* c, const Sexp* dotted) {
  std::printf("(");
  bool first = true;
  for (; c; c = c->cdr) {
    if (!first) std::printf(" ");
    print_sexp(c->car);
    first = false;
  }
  if (dotted) { std::printf(" . "); print_sexp(dotted); }
  std::printf(")");
}

inline void print_sexp(const Sexp* x) {
  if (!x) { std::printf("<null-sexp>"); return; }
  switch (x->tag) {
    case SexpTag::Int:  std::printf("%d", x->i); break;
    case SexpTag::Sym:  std::printf("%s", x->sym ? x->sym->str : "<null-sym>"); break;
    case SexpTag::List: print_sexp_list(x->list.head, x->list.dotted); break;
    default:            std::printf("<unknown-sexp>"); break;
  }
}

// Iterative parse of exactly one s-expression.
// a     — arena for Sexp/SexpCell/ListFrame temporaries (may be a scratch arena)
// sym_a — arena for SymEntry allocations (must be permanent; usually intern_arena)
inline const Sexp* parse_sexp(Arena& a, Arena& sym_a, Intern& in, Lexer& lx, Token& tok) {
  ListFrame* frame = nullptr;

  auto push_frame = [&]() -> bool {
    ListFrame* f = a.make<ListFrame>();
    if (!f) {
        std::fprintf(stderr, "[parse_sexp] push_frame: arena OOM\n");
        return false;
    };
    f->head = nullptr; f->tail = nullptr;
    f->seen_dot = false; f->dotted = nullptr;
    f->prev = frame; frame = f;
    return true;
  };

  auto emit = [&](const Sexp* node) -> const Sexp* {
    if (!frame) return node;
    if (frame->seen_dot) {
      if (frame->dotted) return nullptr;
      frame->dotted = node;
      return reinterpret_cast<const Sexp*>(1);
    }
    SexpCell* c = make_cell(a, node);
    if (!c) {
        std::fprintf(stderr, "[parse_sexp] make_cell: arena OOM\n");
        return nullptr;
    };
    if (!frame->head) frame->head = c;
    if (frame->tail) const_cast<SexpCell*>(frame->tail)->cdr = c;
    frame->tail = c;
    return reinterpret_cast<const Sexp*>(1);
  };

  while (true) {
    switch (tok.tag) {
      case TokTag::LParen:
        if (!push_frame()) {
            std::fprintf(stderr, "[parse_sexp] TokTag::LParen push_frame: arena OOM\n");
            return nullptr;
        };
        tok = lx.next();
        break;

      case TokTag::RParen: {
        if (!frame) {
            std::fprintf(stderr, "[parse_sexp] TokTag::RParen frame: !frame\n");
            return nullptr;
        };
        if (frame->seen_dot && !frame->dotted) {
            std::fprintf(stderr, "[parse_sexp] TokTag::RParen frame->seen_dot && !frame->dotted\n");
            return nullptr;
        };
        const Sexp* list = make_sexp_list(a, frame->head,
                                          frame->seen_dot ? frame->dotted : nullptr);
        frame = frame->prev;
        tok   = lx.next();
        const Sexp* r = emit(list);
        if (r == reinterpret_cast<const Sexp*>(1)) break;
        return r;
      }

      case TokTag::Dot:
        if (!frame || frame->seen_dot) return nullptr;
        frame->seen_dot = true;
        tok = lx.next();
        break;

      case TokTag::Int: {
        const Sexp* node = make_sexp_int(a, tok.ival);
        if (!node) {
            std::fprintf(stderr, "[parse_sexp] TokTag::Int !node\n");
            return nullptr;
        };
        tok = lx.next();
        const Sexp* r = emit(node);
        if (r == reinterpret_cast<const Sexp*>(1)) break;
        return r;
      }

      case TokTag::Sym: {
        const SymEntry* se = intern_sym(sym_a, in, tok.start, tok.len);
        if (!se) {
            std::fprintf(stderr, "[parse_sexp] TokTag::Sym !se\n");
            return nullptr;
        };
        const Sexp* node = make_sexp_sym(a, se);
        if (!node) {
            std::fprintf(stderr, "[parse_sexp] TokTag::Sym !node\n");
            return nullptr;
        };
        tok = lx.next();
        const Sexp* r = emit(node);
        if (r == reinterpret_cast<const Sexp*>(1)) break;
        return r;
      }

      case TokTag::End:
      default:
        std::fprintf(stderr, "[parse_sexp] TokTag::End or default\n");
        return nullptr;
    }
  }
}

// ============================================================================
// Compile-time environments
// ============================================================================
struct GlobalBind { const SymEntry* name; std::uint32_t id; const GlobalBind* next; };
struct BoundBind  { const SymEntry* name; const BoundBind* next; };

static_assert(std::is_trivially_destructible_v<GlobalBind>);
static_assert(std::is_trivially_destructible_v<BoundBind>);

inline const GlobalBind* global_add(Arena& a, const GlobalBind* env,
                                    const SymEntry* name, std::uint32_t id) {
  GlobalBind* v = a.make<GlobalBind>();
  if (!v) return nullptr;
  v->name = name; v->id = id; v->next = env;
  return v;
}

inline bool global_find(const GlobalBind* env, const SymEntry* name, std::uint32_t& out_id) {
  for (auto* e = env; e; e = e->next)
    if (e->name == name) { out_id = e->id; return true; }
  return false;
}

inline bool bound_find(const BoundBind* env, const SymEntry* name, std::uint32_t& out_depth) {
  std::uint32_t depth = 0;
  for (auto* e = env; e; e = e->next, ++depth)
    if (e->name == name) { out_depth = depth; return true; }
  return false;
}

inline const BoundBind* bound_push(Arena& a, const BoundBind* env, const SymEntry* name) {
  BoundBind* b = a.make<BoundBind>();
  if (!b) return nullptr;
  b->name = name; b->next = env;
  return b;
}

// ============================================================================
// Forward declaration: compile_goal is called by compile_term for (rel ...) bodies
// ============================================================================
inline const Goal* compile_goal(Arena& a, const GlobalBind* genv,
                                const BoundBind* benv, const Sexp* x);

// ============================================================================
// Compile Sexp -> Term
// ============================================================================
inline Term compile_term(Arena& a, const GlobalBind* genv,
                         const BoundBind* benv, const Sexp* x) {
  if (!x) return Term::nil();

  if (x->tag == SexpTag::Int) return Term::integer(x->i);

  if (x->tag == SexpTag::Sym) {
    std::uint32_t depth = 0;
    if (bound_find(benv, x->sym, depth)) return Term::bvar(depth);
    std::uint32_t id = 0;
    if (global_find(genv, x->sym, id)) return Term::var(id);
    return Term::symbol(x->sym);
  }

  // List — check for special forms first
  const SexpCell* c = x->list.head;

  // () → Nil
  if (!c && !x->list.dotted) return Term::nil();

  // (rel (params...) body)  — anonymous relation term (Stage 0A)
  if (c && c->car && c->car->tag == SexpTag::Sym &&
      sym_lit_eq(c->car->sym, "rel")) {
    const SexpCell* rest = c->cdr;

    if (!rest || !rest->car || rest->car->tag != SexpTag::List) {
      std::printf("[compile_term] ERROR: 'rel' requires a parameter list: ");
      print_sexp(x);
      std::printf("\n");
      return Term::nil();
    }

    // Collect parameter names
    const SymEntry* param_names[64];
    std::uint32_t   param_count = 0;
    for (const SexpCell* p = rest->car->list.head; p; p = p->cdr) {
      if (!p->car || p->car->tag != SexpTag::Sym) {
        std::printf("[compile_term] ERROR: 'rel' parameter must be a symbol: ");
        print_sexp(x);
        std::printf("\n");
        return Term::nil();
      }
      if (param_count >= 64) {
        std::printf("[compile_term] ERROR: 'rel' has too many parameters\n");
        return Term::nil();
      }
      param_names[param_count++] = p->car->sym;
    }

    // Build fresh closed benv from nullptr (enforces closed-relation property)
    const BoundBind* rel_benv = nullptr;
    for (std::uint32_t i = 0; i < param_count; ++i) {
      rel_benv = bound_push(a, rel_benv, param_names[i]);
      if (!rel_benv) return Term::nil();
    }

    rest = rest->cdr;
    if (!rest || !rest->car) {
      std::printf("[compile_term] ERROR: 'rel' has no body: ");
      print_sexp(x);
      std::printf("\n");
      return Term::nil();
    }

    // Extend genv with outer benv entries so outer fresh variables are visible
    // inside the rel body as Var references (not as BVar or unknown symbols).
    //
    // At runtime, GoalTag::Fresh allocates `n` vars starting at counter=base_id,
    // pushing them as BVar(0)=Var(base+n-1), BVar(1)=Var(base+n-2), ..., BVar(n-1)=Var(base).
    // We mirror that mapping here so that names from the outer fresh scope compile
    // to the correct predicted Var IDs inside the rel body.

    // Step 1: compute base_id = one past the highest Var ID visible in genv.
    std::uint32_t base_id = 0;
    for (const GlobalBind* g = genv; g; g = g->next)
      if (g->id >= base_id) base_id = g->id + 1;

    // Step 2: count outer benv entries.
    std::uint32_t benv_count = 0;
    for (const BoundBind* b = benv; b; b = b->next) ++benv_count;

    // Step 3: add each outer benv name to body_genv mapped to its predicted Var ID.
    // BVar at depth d in a fresh of size n starting at base_id →
    //   Var(base_id + benv_count - 1 - d)
    const GlobalBind* body_genv = genv;
    {
      std::uint32_t d = 0;
      for (const BoundBind* b = benv; b; b = b->next, ++d) {
        std::uint32_t var_id = base_id + benv_count - 1 - d;
        body_genv = global_add(a, body_genv, b->name, var_id);
        if (!body_genv) return Term::nil();
      }
    }

    // Compile body with body_genv (outer vars visible) and fresh closed rel_benv.
    const Goal* body = compile_goal(a, body_genv, rel_benv, rest->car);
    if (!body) return Term::nil();

    RelNode* rn = a.make<RelNode>();
    if (!rn) return Term::nil();
    rn->param_count = param_count;
    rn->body        = body;
    return Term::relation(rn);
  }

  // General list → build pair spine
  std::uint32_t n = 0;
  for (auto* t = c; t; t = t->cdr) ++n;

  Term* arr = static_cast<Term*>(a.alloc(sizeof(Term) * (n ? n : 1), alignof(Term)));
  if (!arr) return Term::nil();

  std::uint32_t i = 0;
  for (auto* t = c; t; t = t->cdr)
    arr[i++] = compile_term(a, genv, benv, t->car);

  Term tail = x->list.dotted
                  ? compile_term(a, genv, benv, x->list.dotted)
                  : Term::nil();

  for (std::int32_t k = (std::int32_t)n - 1; k >= 0; --k) {
    PairNode* p = a.make<PairNode>();
    if (!p) return Term::nil();
    p->car = arr[k];
    p->cdr = tail;
    tail   = Term::make_pair(p);
  }

  return tail;
}

// ============================================================================
// Compile Sexp -> Goal
// ============================================================================
inline const Goal* compile_goal(Arena& a, const GlobalBind* genv,
                                const BoundBind* benv, const Sexp* x) {
  if (!x) {
    std::printf("[compile_goal] ERROR: null sexp\n");
    return nullptr;
  }
  if (x->tag != SexpTag::List) {
    std::printf("[compile_goal] ERROR: expected list, got ");
    print_sexp(x);
    std::printf("\n");
    return nullptr;
  }

  const SexpCell* c = x->list.head;
  if (!c || !c->car) {
    std::printf("[compile_goal] ERROR: empty list or null head\n");
    return nullptr;
  }
  if (c->car->tag != SexpTag::Sym) {
    std::printf("[compile_goal] ERROR: goal head is not a symbol, got ");
    print_sexp(c->car);
    std::printf("\n");
    return nullptr;
  }

  const SymEntry* op = c->car->sym;
  c = c->cdr;

  // ---- == ----
  if (sym_lit_eq(op, "==")) {
    if (!c || !c->cdr || c->cdr->cdr) {
      std::printf("[compile_goal] ERROR: '==' requires exactly 2 args: ");
      print_sexp(x);
      std::printf("\n");
      return nullptr;
    }
    Term u = compile_term(a, genv, benv, c->car);
    Term v = compile_term(a, genv, benv, c->cdr->car);
    return make_eq(a, u, v);
  }

  // ---- =/= ----
  if (sym_lit_eq(op, "=/=")) {
    if (!c || !c->cdr || c->cdr->cdr) {
      std::printf("[compile_goal] ERROR: '=/=' requires exactly 2 args: ");
      print_sexp(x);
      std::printf("\n");
      return nullptr;
    }
    Term u = compile_term(a, genv, benv, c->car);
    Term v = compile_term(a, genv, benv, c->cdr->car);
    return make_diseq(a, u, v);
  }

  // ---- fresh ----
  if (sym_lit_eq(op, "fresh")) {
    if (!c || !c->car || c->car->tag != SexpTag::List) {
      std::printf("[compile_goal] ERROR: 'fresh' requires a variable list: ");
      print_sexp(x);
      std::printf("\n");
      return nullptr;
    }
    const SexpCell* vc = c->car->list.head;
    if (!vc) {
      std::printf("[compile_goal] ERROR: 'fresh' variable list is empty: ");
      print_sexp(x);
      std::printf("\n");
      return nullptr;
    }

    const SymEntry* names[64];
    std::uint32_t   n = 0;
    for (; vc; vc = vc->cdr) {
      if (!vc->car || vc->car->tag != SexpTag::Sym) {
        std::printf("[compile_goal] ERROR: 'fresh' variable list contains non-symbol: ");
        print_sexp(c->car);
        std::printf("\n");
        return nullptr;
      }
      if (n >= 64) {
        std::printf("[compile_goal] ERROR: 'fresh' variable list exceeds 64 names\n");
        return nullptr;
      }
      names[n++] = vc->car->sym;
    }

    c = c->cdr;
    if (!c || !c->car) {
      std::printf("[compile_goal] ERROR: 'fresh' has no body: ");
      print_sexp(x);
      std::printf("\n");
      return nullptr;
    }

    const BoundBind* benv2 = benv;
    for (std::uint32_t i = 0; i < n; ++i) {
      benv2 = bound_push(a, benv2, names[i]);
      if (!benv2) {
        std::printf("[compile_goal] ERROR: OOM pushing bound variable '%s'\n",
                    names[i]->str);
        return nullptr;
      }
    }

    // Compile body: single goal, or fold multiple goals as conj.
    const Goal* body = compile_goal(a, genv, benv2, c->car);
    if (!body) {
      std::printf("[compile_goal] ERROR: 'fresh' body failed to compile: ");
      print_sexp(c->car);
      std::printf("\n");
      return nullptr;
    }
    c = c->cdr;
    while (c) {
      const Goal* rhs = compile_goal(a, genv, benv2, c->car);
      if (!rhs) {
        std::printf("[compile_goal] ERROR: 'fresh' body goal failed to compile: ");
        print_sexp(c->car);
        std::printf("\n");
        return nullptr;
      }
      body = make_conj(a, body, rhs);
      if (!body) return nullptr;
      c = c->cdr;
    }

    return make_fresh(a, n, body);
  }

  // ---- disj / conj ----
  if (sym_lit_eq(op, "disj") || sym_lit_eq(op, "conj")) {
    const bool is_disj = sym_lit_eq(op, "disj");
    if (!c || !c->cdr) {
      std::printf("[compile_goal] ERROR: '%s' requires at least 2 args: ",
                  is_disj ? "disj" : "conj");
      print_sexp(x);
      std::printf("\n");
      return nullptr;
    }

    const Goal* acc = compile_goal(a, genv, benv, c->car);
    if (!acc) {
      std::printf("[compile_goal] ERROR: '%s' first arg failed to compile: ",
                  is_disj ? "disj" : "conj");
      print_sexp(c->car);
      std::printf("\n");
      return nullptr;
    }
    c = c->cdr;

    std::uint32_t arg_idx = 1;
    while (c) {
      const Goal* rhs = compile_goal(a, genv, benv, c->car);
      if (!rhs) {
        std::printf("[compile_goal] ERROR: '%s' arg %u failed to compile: ",
                    is_disj ? "disj" : "conj", arg_idx);
        print_sexp(c->car);
        std::printf("\n");
        return nullptr;
      }
      acc = is_disj ? make_disj(a, acc, rhs) : make_conj(a, acc, rhs);
      if (!acc) {
        std::printf("[compile_goal] ERROR: OOM allocating '%s' node\n",
                    is_disj ? "disj" : "conj");
        return nullptr;
      }
      c = c->cdr;
      ++arg_idx;
    }
    return acc;
  }

  // ---- probe ----
  if (sym_lit_eq(op, "probe")) {
    if (!c || !c->cdr || !c->cdr->cdr || !c->cdr->cdr->cdr ||
        !c->cdr->cdr->cdr->cdr || c->cdr->cdr->cdr->cdr->cdr) {
      std::printf("[compile_goal] ERROR: 'probe' requires exactly 5 args "
                  "(GOAL CONDITION MAX_ITER SANDBOX REQ_GROUND): ");
      print_sexp(x);
      std::printf("\n");
      return nullptr;
    }

    const Goal* sub = compile_goal(a, genv, benv, c->car);
    if (!sub) {
      std::printf("[compile_goal] ERROR: 'probe' sub-goal failed to compile: ");
      print_sexp(c->car);
      std::printf("\n");
      return nullptr;
    }

    Term condition  = compile_term(a, genv, benv, c->cdr->car);
    Term max_iter   = compile_term(a, genv, benv, c->cdr->cdr->car);
    Term sandbox    = compile_term(a, genv, benv, c->cdr->cdr->cdr->car);
    Term req_ground = compile_term(a, genv, benv, c->cdr->cdr->cdr->cdr->car);

    return make_probe(a, sub, condition, max_iter, sandbox, req_ground);
  }

  // ---- call (Stage 0A) ----
  if (sym_lit_eq(op, "call")) {
    if (!c) {
      std::printf("[compile_goal] ERROR: 'call' requires at least a relation term: ");
      print_sexp(x);
      std::printf("\n");
      return nullptr;
    }

    // Compile the relation term in the current scope
    Term rel_term = compile_term(a, genv, benv, c->car);
    c = c->cdr;

    // Count and compile arguments
    std::uint32_t arg_count = 0;
    for (const SexpCell* t = c; t; t = t->cdr) ++arg_count;

    const Term* args = nullptr;
    if (arg_count > 0) {
      Term* arr = static_cast<Term*>(
          a.alloc(sizeof(Term) * arg_count, alignof(Term)));
      if (!arr) return nullptr;
      std::uint32_t i = 0;
      for (const SexpCell* t = c; t; t = t->cdr)
        arr[i++] = compile_term(a, genv, benv, t->car);
      args = arr;
    }

    return make_call(a, rel_term, args, arg_count);
  }

  // ---- defrel inside a goal (diagnostic error) ----
  if (sym_lit_eq(op, "defrel")) {
    std::printf("[compile_goal] ERROR: 'defrel' is only valid at the top level, "
                "not inside a goal body: ");
    print_sexp(x);
    std::printf("\n");
    return nullptr;
  }

  // Unrecognized operator: compile as an implicit GoalCall.
  // This allows arithmetic built-ins (leqo, addsubo, charo, etc.) and
  // any user-defined relation to be invoked without the explicit (call ...)
  // wrapper.  Unknown names fall through to handleKnownRelation /
  // handleUnknownRelation at runtime.
  {
    Term rel_term = Term::symbol(op);
    const SexpCell* c = x->list.head->cdr;  // args start after the operator

    std::uint32_t arg_count = 0;
    for (const SexpCell* t = c; t; t = t->cdr) ++arg_count;

    const Term* args = nullptr;
    if (arg_count > 0) {
      Term* arr = static_cast<Term*>(
          a.alloc(sizeof(Term) * arg_count, alignof(Term)));
      if (!arr) return nullptr;
      std::uint32_t i = 0;
      for (const SexpCell* t = c; t; t = t->cdr)
        arr[i++] = compile_term(a, genv, benv, t->car);
      args = arr;
    }
    return make_call(a, rel_term, args, arg_count);
  }
}

// ============================================================================
// ParsedQuery
// ============================================================================
struct ParsedQuery {
  int           n            = 0;
  Term          qvar         = Term::nil();
  std::uint32_t vars_used    = 0;
  const Goal*   goal         = nullptr;
  Intern        intern;
  OutcomeSyms   outcome_syms;
  RelEnv        rel_env;      // Stage 0A: populated by defrel, used at runtime
};

// ============================================================================
// parse_query: handles (defrel ...)* (run N (q) GOAL) or goal-only form
// ============================================================================
inline ParsedQuery parse_query(Arena& a, const char* src) {
  ParsedQuery out{};
  out.n         = 10;
  out.qvar      = Term::var(0);
  out.vars_used = 1;
  out.goal      = nullptr;
  out.intern    = Intern{0, nullptr};

  if (!intern_init(a, out.intern, 256)) {
    std::printf("[parse_query] ERROR: intern_init failed (OOM?)\n");
    return out;
  }

  out.outcome_syms.s_true         = intern_cstr(a, out.intern, "true");
  out.outcome_syms.s_false        = intern_cstr(a, out.intern, "false");
  out.outcome_syms.s_insufficient = intern_cstr(a, out.intern, "insufficient");
  out.outcome_syms.s_bounded      = intern_cstr(a, out.intern, "bounded");
  if (!out.outcome_syms.s_true || !out.outcome_syms.s_false ||
      !out.outcome_syms.s_insufficient || !out.outcome_syms.s_bounded) {
    std::printf("[parse_query] ERROR: failed to intern outcome symbols (OOM?)\n");
    out.goal = nullptr;
    return out;
  }

  Lexer lx{src};
  Token tok = lx.next();

  // Multi-form loop: defrel* then run (or goal-only for backward compat)
  while (tok.tag != TokTag::End) {
    const Sexp* top = parse_sexp(a, a, out.intern, lx, tok);
    if (!top) {
      std::printf("[parse_query] ERROR: parse_sexp failed — malformed s-expression\n");
      std::printf("[parse_query] Source: %s\n", src);
      return out;
    }

    // Determine what kind of top-level form this is
    bool is_list = (top->tag == SexpTag::List);
    const SymEntry* head_sym = nullptr;
    if (is_list && top->list.head && top->list.head->car &&
        top->list.head->car->tag == SexpTag::Sym)
      head_sym = top->list.head->car->sym;

    // ---- defrel ----
    if (head_sym && sym_lit_eq(head_sym, "defrel")) {
      const SexpCell* dc = top->list.head->cdr;  // rest after "defrel"

      // (defrel (name param1 param2 ...) body)
      if (!dc || !dc->car || dc->car->tag != SexpTag::List) {
        std::printf("[parse_query] ERROR: 'defrel' requires a name+param list: ");
        print_sexp(top);
        std::printf("\n");
        return out;
      }

      const SexpCell* name_and_params = dc->car->list.head;
      if (!name_and_params || !name_and_params->car ||
          name_and_params->car->tag != SexpTag::Sym) {
        std::printf("[parse_query] ERROR: 'defrel' name must be a symbol: ");
        print_sexp(top);
        std::printf("\n");
        return out;
      }

      const SymEntry* rel_name   = name_and_params->car->sym;
      const SexpCell* param_list = name_and_params->cdr;

      // Collect parameter names
      const SymEntry* param_names[64];
      std::uint32_t   param_count = 0;
      for (const SexpCell* p = param_list; p; p = p->cdr) {
        if (!p->car || p->car->tag != SexpTag::Sym) {
          std::printf("[parse_query] ERROR: 'defrel' parameter must be a symbol: ");
          print_sexp(top);
          std::printf("\n");
          return out;
        }
        if (param_count >= 64) {
          std::printf("[parse_query] ERROR: 'defrel' has too many parameters\n");
          return out;
        }
        param_names[param_count++] = p->car->sym;
      }

      // Body
      dc = dc->cdr;
      if (!dc || !dc->car) {
        std::printf("[parse_query] ERROR: 'defrel' has no body: ");
        print_sexp(top);
        std::printf("\n");
        return out;
      }

      // Build fresh closed benv for parameters
      const BoundBind* rel_benv = nullptr;
      for (std::uint32_t i = 0; i < param_count; ++i) {
        rel_benv = bound_push(a, rel_benv, param_names[i]);
        if (!rel_benv) {
          std::printf("[parse_query] ERROR: OOM building rel benv\n");
          return out;
        }
      }

      // Compile body with empty genv (defrel is top-level; q not yet defined).
      // Multi-goal bodies are chained with conj (same as fresh bodies).
      const Goal* body = compile_goal(a, nullptr, rel_benv, dc->car);
      if (!body) {
        std::printf("[parse_query] ERROR: 'defrel' body failed to compile\n");
        return out;
      }
      dc = dc->cdr;
      while (dc && dc->car) {
        const Goal* g2 = compile_goal(a, nullptr, rel_benv, dc->car);
        if (!g2) {
          std::printf("[parse_query] ERROR: 'defrel' body goal failed to compile\n");
          return out;
        }
        body = make_conj(a, body, g2);
        if (!body) return out;
        dc = dc->cdr;
      }

      RelNode* rn = a.make<RelNode>();
      if (!rn) {
        std::printf("[parse_query] ERROR: OOM allocating RelNode\n");
        return out;
      }
      rn->param_count = param_count;
      rn->body        = body;

      out.rel_env.define(a, rel_name, Term::relation(rn));
      continue;  // parse next top-level form
    }

    // ---- run ----
    if (head_sym && sym_lit_eq(head_sym, "run")) {
      const SexpCell* c = top->list.head->cdr;

      if (!c || !c->car || c->car->tag != SexpTag::Int) {
        std::printf("[parse_query] ERROR: 'run' requires an integer count as second element\n");
        return out;
      }
      out.n = c->car->i;
      c = c->cdr;

      if (!c || !c->car || c->car->tag != SexpTag::List) {
        std::printf("[parse_query] ERROR: 'run' requires a variable list as third element\n");
        return out;
      }
      const SexpCell* qcells = c->car->list.head;
      if (!qcells || qcells->cdr) {
        std::printf("[parse_query] ERROR: 'run' variable list must contain exactly one variable\n");
        return out;
      }
      if (!qcells->car || qcells->car->tag != SexpTag::Sym) {
        std::printf("[parse_query] ERROR: 'run' query variable must be a symbol\n");
        return out;
      }

      const GlobalBind* genv = nullptr;
      genv = global_add(a, genv, qcells->car->sym, 0);
      if (!genv) {
        std::printf("[parse_query] ERROR: OOM adding query variable to global env\n");
        return out;
      }

      out.qvar      = Term::var(0);
      out.vars_used = 1;

      c = c->cdr;
      if (!c || !c->car) {
        std::printf("[parse_query] ERROR: 'run' has no goal\n");
        return out;
      }

      out.goal = compile_goal(a, genv, nullptr, c->car);
      if (!out.goal) {
        std::printf("[parse_query] ERROR: goal compilation failed (see above for details)\n");
        return out;
      }
      // Chain additional goals (if any) as left-nested conjunctions.
      // (run N (q) g1 g2 g3) is sugar for (run N (q) (conj g1 (conj g2 g3))).
      c = c->cdr;
      while (c && c->car) {
        const Goal* g2 = compile_goal(a, genv, nullptr, c->car);
        if (!g2) {
          std::printf("[parse_query] ERROR: goal compilation failed (see above for details)\n");
          return out;
        }
        out.goal = make_conj(a, out.goal, g2);
        if (!out.goal) return out;
        c = c->cdr;
      }
      return out;
    }

    // ---- goal-only form (backward compatibility for programs without 'run') ----
    {
      const SymEntry* qsym = intern_cstr(a, out.intern, "q");
      const GlobalBind* genv = global_add(a, nullptr, qsym, 0);
      out.goal = compile_goal(a, genv, nullptr, top);
      if (!out.goal)
        std::printf("[parse_query] ERROR: goal compilation failed (see above for details)\n");
      return out;
    }
  }

  // Defrel-only success: no 'run' form, but at least one defrel was defined.
  if (out.rel_env.head != nullptr) {
    out.n    = 0;
    out.goal = nullptr;
    return out;
  }

  std::printf("[parse_query] ERROR: unexpected end of input (expected 'run' form)\n");
  return out;
}

// ============================================================================
// parse_query overload for REPL session use.
// Uses an externally provided Intern and base RelEnv so the caller's long-lived
// symbol table is shared.  New defrels are returned in pq.rel_env (empty base).
// The caller is responsible for merging pq.rel_env into the session rel_env.
//
// a     — scratch arena for parse temporaries (Sexp nodes, Goal trees, etc.);
//          may be query_arena (reset between calls). Nothing allocated here
//          needs to outlive the current dispatch call.
// sym_a — arena for SymEntry allocations; must be permanent (intern_arena).
//          SymEntry pointers are used for pointer-identity equality checks
//          across the lifetime of the session, so they must never be freed.
// ============================================================================
inline ParsedQuery parse_query(Arena& a, Arena& sym_a, const char* src,
                               Intern& sess_intern,
                               const RelEnv& sess_rel_env) {
  ParsedQuery out{};
  out.n         = 10;
  out.qvar      = Term::nil();
  out.vars_used = 0;
  out.goal      = nullptr;
  // Share the caller's intern table — new SymEntries go into sym_a.
  out.intern    = sess_intern;

  // Intern outcome symbols (no-ops if already present in the shared table).
  out.outcome_syms.s_true         = intern_cstr(sym_a, out.intern, "true");
  out.outcome_syms.s_false        = intern_cstr(sym_a, out.intern, "false");
  out.outcome_syms.s_insufficient = intern_cstr(sym_a, out.intern, "insufficient");
  out.outcome_syms.s_bounded      = intern_cstr(sym_a, out.intern, "bounded");
  if (!out.outcome_syms.s_true || !out.outcome_syms.s_false ||
      !out.outcome_syms.s_insufficient || !out.outcome_syms.s_bounded) {
    std::printf("[parse_query] ERROR: failed to intern outcome symbols (OOM?)\n");
    out.goal = nullptr;
    return out;
  }

  // Propagate any new intern entries back to the caller's Intern struct
  // (capacity may have changed if the table was rehashed).
  // We do this at the end; for now just work with out.intern.

  // Use sess_rel_env as a read-only base for runtime call resolution.
  // We start out.rel_env empty so pq.rel_env contains only NEW defrels.
  // (Runtime lookup works via SymEntry::rel_cache set by define().)
  (void)sess_rel_env;  // rel_cache on SymEntry provides runtime lookup

  Lexer lx{src};
  Token tok = lx.next();

  while (tok.tag != TokTag::End) {
    const Sexp* top = parse_sexp(a, sym_a, out.intern, lx, tok);
    if (!top) {
      std::printf("[parse_query] ERROR: parse_sexp failed — malformed s-expression\n");
      std::printf("[parse_query] Source: %s\n", src);
      sess_intern = out.intern;
      return out;
    }

    bool is_list = (top->tag == SexpTag::List);
    const SymEntry* head_sym = nullptr;
    if (is_list && top->list.head && top->list.head->car &&
        top->list.head->car->tag == SexpTag::Sym)
      head_sym = top->list.head->car->sym;

    // ---- defrel ----
    if (head_sym && sym_lit_eq(head_sym, "defrel")) {
      const SexpCell* dc = top->list.head->cdr;

      if (!dc || !dc->car || dc->car->tag != SexpTag::List) {
        std::printf("[parse_query] ERROR: 'defrel' requires a name+param list\n");
        sess_intern = out.intern;
        return out;
      }

      const SexpCell* name_and_params = dc->car->list.head;
      if (!name_and_params || !name_and_params->car ||
          name_and_params->car->tag != SexpTag::Sym) {
        std::printf("[parse_query] ERROR: 'defrel' name must be a symbol\n");
        sess_intern = out.intern;
        return out;
      }

      const SymEntry* rel_name   = name_and_params->car->sym;
      const SexpCell* param_list = name_and_params->cdr;

      const SymEntry* param_names[64];
      std::uint32_t   param_count = 0;
      for (const SexpCell* p = param_list; p; p = p->cdr) {
        if (!p->car || p->car->tag != SexpTag::Sym) {
          std::printf("[parse_query] ERROR: 'defrel' parameter must be a symbol\n");
          sess_intern = out.intern;
          return out;
        }
        if (param_count >= 64) {
          std::printf("[parse_query] ERROR: 'defrel' has too many parameters\n");
          sess_intern = out.intern;
          return out;
        }
        param_names[param_count++] = p->car->sym;
      }

      dc = dc->cdr;
      if (!dc || !dc->car) {
        std::printf("[parse_query] ERROR: 'defrel' has no body\n");
        sess_intern = out.intern;
        return out;
      }

      const BoundBind* rel_benv = nullptr;
      for (std::uint32_t i = 0; i < param_count; ++i) {
        rel_benv = bound_push(a, rel_benv, param_names[i]);
        if (!rel_benv) {
          std::printf("[parse_query] ERROR: OOM building rel benv\n");
          sess_intern = out.intern;
          return out;
        }
      }

      const Goal* body = compile_goal(a, nullptr, rel_benv, dc->car);
      if (!body) {
        std::printf("[parse_query] ERROR: 'defrel' body failed to compile\n");
        sess_intern = out.intern;
        return out;
      }
      dc = dc->cdr;
      while (dc && dc->car) {
        const Goal* g2 = compile_goal(a, nullptr, rel_benv, dc->car);
        if (!g2) {
          std::printf("[parse_query] ERROR: 'defrel' body goal failed to compile\n");
          sess_intern = out.intern;
          return out;
        }
        body = make_conj(a, body, g2);
        if (!body) { sess_intern = out.intern; return out; }
        dc = dc->cdr;
      }

      RelNode* rn = a.make<RelNode>();
      if (!rn) {
        std::printf("[parse_query] ERROR: OOM allocating RelNode\n");
        sess_intern = out.intern;
        return out;
      }
      rn->param_count = param_count;
      rn->body        = body;

      out.rel_env.define(a, rel_name, Term::relation(rn));
      continue;
    }

    // ---- run ----
    if (head_sym && sym_lit_eq(head_sym, "run")) {
      const SexpCell* c = top->list.head->cdr;

      if (!c || !c->car || c->car->tag != SexpTag::Int) {
        std::printf("[parse_query] ERROR: 'run' requires an integer count\n");
        sess_intern = out.intern;
        return out;
      }
      out.n = c->car->i;
      c = c->cdr;

      if (!c || !c->car || c->car->tag != SexpTag::List) {
        std::printf("[parse_query] ERROR: 'run' requires a variable list\n");
        sess_intern = out.intern;
        return out;
      }
      const SexpCell* qcells = c->car->list.head;
      if (!qcells || qcells->cdr) {
        std::printf("[parse_query] ERROR: 'run' variable list must have exactly one variable\n");
        sess_intern = out.intern;
        return out;
      }
      if (!qcells->car || qcells->car->tag != SexpTag::Sym) {
        std::printf("[parse_query] ERROR: 'run' query variable must be a symbol\n");
        sess_intern = out.intern;
        return out;
      }

      const GlobalBind* genv = nullptr;
      genv = global_add(a, genv, qcells->car->sym, 0);
      if (!genv) {
        std::printf("[parse_query] ERROR: OOM adding query variable\n");
        sess_intern = out.intern;
        return out;
      }

      out.qvar      = Term::var(0);
      out.vars_used = 1;

      c = c->cdr;
      if (!c || !c->car) {
        std::printf("[parse_query] ERROR: 'run' has no goal\n");
        sess_intern = out.intern;
        return out;
      }

      out.goal = compile_goal(a, genv, nullptr, c->car);
      if (!out.goal) {
        std::printf("[parse_query] ERROR: goal compilation failed\n");
        sess_intern = out.intern;
        return out;
      }
      // Chain additional goals (multi-goal run sugar).
      c = c->cdr;
      while (c && c->car) {
        const Goal* g2 = compile_goal(a, genv, nullptr, c->car);
        if (!g2) {
          std::printf("[parse_query] ERROR: goal compilation failed\n");
          sess_intern = out.intern;
          return out;
        }
        out.goal = make_conj(a, out.goal, g2);
        if (!out.goal) { sess_intern = out.intern; return out; }
        c = c->cdr;
      }
      sess_intern = out.intern;
      return out;
    }

    // goal-only form (backward compat)
    {
      const SymEntry* qsym = intern_cstr(sym_a, out.intern, "q");
      const GlobalBind* genv = global_add(a, nullptr, qsym, 0);
      out.goal = compile_goal(a, genv, nullptr, top);
      if (!out.goal)
        std::printf("[parse_query] ERROR: goal compilation failed\n");
      sess_intern = out.intern;
      return out;
    }
  }

  // Defrel-only success
  if (out.rel_env.head != nullptr) {
    out.n    = 0;
    out.goal = nullptr;
    sess_intern = out.intern;
    return out;
  }

  std::printf("[parse_query] ERROR: unexpected end of input\n");
  sess_intern = out.intern;
  return out;
}

// ============================================================================
// Printing
// ============================================================================
inline void print_term(Term t);

inline void print_list(Term t) {
  std::printf("(");
  bool first = true;
  while (true) {
    if (t.tag == TermTag::Nil) { std::printf(")"); return; }
    if (t.tag != TermTag::Pair || !t.pair) {
      std::printf(" . "); print_term(t); std::printf(")"); return;
    }
    const PairNode* p = t.pair;
    if (!first) std::printf(" ");
    print_term(p->car);
    t     = p->cdr;
    first = false;
  }
}

inline void print_term(Term t) {
  switch (t.tag) {
    case TermTag::Int:  std::printf("%d", t.value); break;
    case TermTag::Nil:  std::printf("()"); break;
    case TermTag::Var:  std::printf("_.%u", t.id); break;
    case TermTag::BVar: std::printf("b_.%u", t.id); break;
    case TermTag::Sym:  std::printf("%s", t.sym ? t.sym->str : "<sym?>"); break;
    case TermTag::Pair: print_list(t); break;
    case TermTag::Rel:  std::printf("#<rel/%u>", t.rel ? t.rel->param_count : 0); break;
    default:            std::printf("<term?>"); break;
  }
}

inline void print_goal(const Goal* g, int indent = 0) {
  if (!g) { std::printf("<null-goal>"); return; }

  switch (g->tag) {
    case GoalTag::Eq:
      std::printf("(== ");
      print_term(g->eq.u);
      std::printf(" ");
      print_term(g->eq.v);
      std::printf(")");
      break;

    case GoalTag::Disj:
      std::printf("(disj ");
      print_goal(g->bin.g1, indent + 6);
      std::printf("\n%*s      ", indent, "");
      print_goal(g->bin.g2, indent + 6);
      std::printf(")");
      break;

    case GoalTag::Conj:
      std::printf("(conj ");
      print_goal(g->bin.g1, indent + 6);
      std::printf("\n%*s      ", indent, "");
      print_goal(g->bin.g2, indent + 6);
      std::printf(")");
      break;

    case GoalTag::Fresh:
      std::printf("(fresh/%u ", g->fresh.n);
      print_goal(g->fresh.body, indent + 9);
      std::printf(")");
      break;

    case GoalTag::Probe:
      std::printf("(probe ");
      print_goal(g->probe.sub, indent + 7);
      std::printf(" ");
      print_term(g->probe.condition);
      std::printf(" ");
      print_term(g->probe.max_iter);
      std::printf(" ");
      print_term(g->probe.sandbox);
      std::printf(" ");
      print_term(g->probe.req_ground);
      std::printf(")");
      break;

    case GoalTag::Call:
      std::printf("(call ");
      print_term(g->call.rel_term);
      for (std::uint32_t i = 0; i < g->call.arg_count; ++i) {
        std::printf(" ");
        print_term(g->call.args[i]);
      }
      std::printf(")");
      break;

    case GoalTag::Diseq:
      std::printf("(=/= ");
      print_term(g->diseq.u);
      std::printf(" ");
      print_term(g->diseq.v);
      std::printf(")");
      break;

    default:
      std::printf("<unknown-goal-tag:%d>", (int)g->tag);
      break;
  }
}

inline void print_query(const ParsedQuery& pq) {
  std::printf("=== parse_query diagnostic ===\n");

  if (!pq.goal) {
    std::printf("Status: PARSE FAILED (pq.goal is null)\n");
    std::printf("==============================\n\n");
    return;
  }

  std::printf("Status:    OK\n");
  std::printf("n:         %d\n", pq.n);
  std::printf("vars_used: %u\n", pq.vars_used);
  std::printf("qvar:      ");
  print_term(pq.qvar);
  std::printf("\n");
  std::printf("Goal tree:\n  ");
  print_goal(pq.goal, 2);
  std::printf("\n");
  std::printf("==============================\n\n");
}

// Print a compiled relation body — used by --verbose in the REPL and raprunner.
// name may be nullptr (prints "<anonymous>"). param_count is the arity.
inline void print_rel_body(const char* name, std::uint32_t param_count,
                           const Goal* body) {
  std::printf("=== defrel: %s/%u ===\n",
              name ? name : "<anonymous>", param_count);
  std::printf("Body:\n  ");
  if (body) print_goal(body, 2);
  else      std::printf("<null>");
  std::printf("\n");
  std::printf("==============================\n\n");
}

// Print used/total bytes for an arena in a consistent one-line format.
// label should be a short descriptive name, e.g. "intern_arena".
inline void print_arena_usage(const char* label, const Arena& a) {
  std::size_t used  = static_cast<std::size_t>(a.cur  - a.base);
  std::size_t total = static_cast<std::size_t>(a.end  - a.base);
  std::printf("[arena] %-20s %6zu / %6zu bytes\n", label, used, total);
}

// ============================================================================
// dump_oom_work_queue — OOM diagnostic, analogous to a stack trace.
// Printed to stdout (consistent with print_goal/print_term) at the moment
// StepResult::OOM is detected in runN's main loop.
//
// Covers:
//   - Total count of pending Work items in the queue at that moment.
//   - Breakdown by goal type / relation name where determinable.
//   - A sample of the first 5 and last 5 items: goal + kont-chain depth.
//
// Relation-name attribution: for GoalTag::Call goals where the relation was
// referenced by symbol (rel_term.tag == TermTag::Sym), the name is available
// directly.  For anonymous rels (TermTag::Rel) or unresolved vars, we report
// what is structurally determinable (arity, tag) rather than guessing.
// ============================================================================
inline void dump_oom_work_queue(const WorkQueue& q) {
  // --- Collect all pending items (no modification to queue) ---
  std::vector<const Work*> items;
  for (const Work* w = q.head; w; w = w->next)
    items.push_back(w);

  std::printf("\n[oom-dump] %zu pending work items at arena exhaustion\n",
              items.size());

  // --- Kont depth helper ---
  auto kont_depth = [](const Kont* k) -> int {
    int d = 0;
    while (k && k->tag != KontTag::Done) {
      ++d;
      if (k->tag == KontTag::Then) k = k->then_.next;
      else                          k = k->restore_env_.next;
    }
    return d;
  };

  // --- Goal label helper: returns a string key for breakdown counting ---
  auto goal_label = [](const Goal* g) -> std::string {
    if (!g) return "<null>";
    switch (g->tag) {
      case GoalTag::Call:
        if (g->call.rel_term.tag == TermTag::Sym && g->call.rel_term.sym)
          return std::string(g->call.rel_term.sym->str);
        if (g->call.rel_term.tag == TermTag::Rel)
          return std::string("(anonymous-rel/") +
                 std::to_string(g->call.arg_count) + ")";
        return std::string("call(unresolved-") +
               std::to_string(static_cast<int>(g->call.rel_term.tag)) + ")";
      case GoalTag::Eq:    return "==";
      case GoalTag::Diseq: return "=/=";
      case GoalTag::Disj:  return "disj";
      case GoalTag::Conj:  return "conj";
      case GoalTag::Fresh: return "fresh";
      case GoalTag::Probe: return "probe";
      default:             return "?";
    }
  };

  // --- Breakdown by label ---
  std::map<std::string, int> counts;
  for (const Work* w : items)
    counts[goal_label(w->g)]++;

  std::printf("[oom-dump] breakdown:\n");
  for (const auto& [label, cnt] : counts)
    std::printf("[oom-dump]   %5d  %s\n", cnt, label.c_str());

  // --- Sample: first 5 and last 5 ---
  constexpr std::size_t SAMPLE = 5;
  std::size_t n = items.size();

  auto print_item = [&](std::size_t idx) {
    const Work* w = items[idx];
    std::printf("[oom-dump]   [%zu] kont-depth=%d  goal: ",
                idx, kont_depth(w->k));
    print_goal(w->g, 0);
    std::printf("\n");
  };

  std::printf("[oom-dump] first %zu item(s):\n", n < SAMPLE ? n : SAMPLE);
  for (std::size_t i = 0; i < n && i < SAMPLE; ++i)
    print_item(i);

  if (n > 2 * SAMPLE) {
    std::printf("[oom-dump] ... (%zu omitted) ...\n", n - 2 * SAMPLE);
    std::printf("[oom-dump] last %zu items:\n", SAMPLE);
    for (std::size_t i = n - SAMPLE; i < n; ++i)
      print_item(i);
  } else if (n > SAMPLE) {
    std::printf("[oom-dump] last %zu item(s):\n", n - SAMPLE);
    for (std::size_t i = SAMPLE; i < n; ++i)
      print_item(i);
  }

  std::printf("[oom-dump] end\n\n");
}
