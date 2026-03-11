#pragma once
#include "core.hpp"
#include <cstdint>
#include <cstdio>

// ============================================================================
// Tokenizer (no allocations) + iterative S-expression parser + compiler
// Adds surface-language support for:
//   (fresh (x y ...) GOAL)
// where x,y are bound names compiled to TermTag::BVar (de Bruijn indices).
// ============================================================================

enum class TokTag : std::uint8_t { LParen, RParen, Dot, Int, Sym, End, Error };

struct Token {
  TokTag tag;
  const char* start;
  std::uint32_t len;
  std::int32_t ival;
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
    // skip whitespace and ';' line comments
    for (;;) {
      while (is_space(*p)) ++p;
      if (*p == ';') { while (*p && *p != '\n') ++p; continue; }
      break;
    }

    if (*p == '\0') return Token{TokTag::End, p, 0, 0};
    if (*p == '(') { ++p; return Token{TokTag::LParen, p-1, 1, 0}; }
    if (*p == ')') { ++p; return Token{TokTag::RParen, p-1, 1, 0}; }
    if (*p == '.') { ++p; return Token{TokTag::Dot,  p-1, 1, 0}; }

    // integer: optional '-' then digits, must be delimited
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
      // fallthrough to symbol if not delimited
      p = s;
    } else {
      p = s;
    }

    // symbol
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
    std::int32_t i;
    const SymEntry* sym;
    SexpListData list;
  };
};

static_assert(std::is_trivially_destructible_v<Sexp>);
static_assert(std::is_trivially_destructible_v<SexpCell>);

// Frame for building a list
struct ListFrame {
  const SexpCell* head;
  const SexpCell* tail;
  bool seen_dot;
  const Sexp* dotted;
  ListFrame* prev;
};

static_assert(std::is_trivially_destructible_v<ListFrame>);

inline const Sexp* make_sexp_int(Arena& a, std::int32_t v) {
  Sexp* x = a.make<Sexp>();
  if (!x) return nullptr;
  x->tag = SexpTag::Int;
  x->i = v;
  return x;
}

inline const Sexp* make_sexp_sym(Arena& a, const SymEntry* s) {
  Sexp* x = a.make<Sexp>();
  if (!x) return nullptr;
  x->tag = SexpTag::Sym;
  x->sym = s;
  return x;
}

inline const Sexp* make_sexp_list(Arena& a, const SexpCell* head, const Sexp* dotted) {
  Sexp* x = a.make<Sexp>();
  if (!x) return nullptr;
  x->tag = SexpTag::List;
  x->list = SexpListData{head, dotted};
  return x;
}

inline SexpCell* make_cell(Arena& a, const Sexp* car) {
  SexpCell* c = a.make<SexpCell>();
  if (!c) return nullptr;
  c->car = car;
  c->cdr = nullptr;
  return c;
}

// Iterative parse of exactly one s-expression.
inline const Sexp* parse_sexp(Arena& a, Intern& in, Lexer& lx, Token& tok) {
  ListFrame* frame = nullptr;

	auto push_frame = [&]() -> bool {
	  ListFrame* f = a.make<ListFrame>();
	  if (!f) return false;
	  f->head = nullptr;
	  f->tail = nullptr;
	  f->seen_dot = false;
	  f->dotted = nullptr;
	  f->prev = frame;
	  frame = f;
	  return true;
	};


	auto emit = [&](const Sexp* node) -> const Sexp* {
	  if (!frame) return node;

	  if (frame->seen_dot) {
	    if (frame->dotted) return nullptr; // multiple dotted tails
	    frame->dotted = node;
	    return reinterpret_cast<const Sexp*>(1); // sentinel: continue
	  }

	  SexpCell* c = make_cell(a, node);
	  if (!c) return nullptr;
	  if (!frame->head) frame->head = c;
	  if (frame->tail) const_cast<SexpCell*>(frame->tail)->cdr = c;
	  frame->tail = c;
	  return reinterpret_cast<const Sexp*>(1);
	};

  while (true) {
    switch (tok.tag) {
      case TokTag::LParen:
        if (!push_frame()) return nullptr;
        tok = lx.next();
        break;

      case TokTag::RParen: {
        if (!frame) return nullptr;
        if (frame->seen_dot && !frame->dotted) return nullptr;

        const Sexp* list = make_sexp_list(a, frame->head, frame->seen_dot ? frame->dotted : nullptr);
        frame = frame->prev;
        tok = lx.next();

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
        if (!node) return nullptr;
        tok = lx.next();
        const Sexp* r = emit(node);
        if (r == reinterpret_cast<const Sexp*>(1)) break;
        return r;
      }

      case TokTag::Sym: {
        const SymEntry* se = intern_sym(a, in, tok.start, tok.len);
        if (!se) return nullptr;
        const Sexp* node = make_sexp_sym(a, se);
        if (!node) return nullptr;
        tok = lx.next();
        const Sexp* r = emit(node);
        if (r == reinterpret_cast<const Sexp*>(1)) break;
        return r;
      }

      case TokTag::End:
      default:
        return nullptr;
    }
  }
}

// ============================================================================
// Compile-time environments
//   - GlobalBind: symbol -> concrete Var(id)  (e.g., query vars like q)
//   - BoundBind:  stack of bound names for de Bruijn indices (fresh)
// ============================================================================
struct GlobalBind { const SymEntry* name; std::uint32_t id; const GlobalBind* next; };
struct BoundBind  { const SymEntry* name; const BoundBind* next; };

static_assert(std::is_trivially_destructible_v<GlobalBind>);
static_assert(std::is_trivially_destructible_v<BoundBind>);

inline const GlobalBind* global_add(Arena& a, const GlobalBind* env, const SymEntry* name, std::uint32_t id) {
  GlobalBind* v = a.make<GlobalBind>();
  if (!v) return nullptr;
  v->name = name; v->id = id; v->next = env;
  return v;
}

inline bool global_find(const GlobalBind* env, const SymEntry* name, std::uint32_t& out_id) {
  for (auto* e = env; e; e = e->next) {
    if (e->name == name) { out_id = e->id; return true; }
  }
  return false;
}

inline bool bound_find(const BoundBind* env, const SymEntry* name, std::uint32_t& out_depth) {
  std::uint32_t depth = 0;
  for (auto* e = env; e; e = e->next, ++depth) {
    if (e->name == name) { out_depth = depth; return true; }
  }
  return false;
}

inline const BoundBind* bound_push(Arena& a, const BoundBind* env, const SymEntry* name) {
  BoundBind* b = a.make<BoundBind>();
  if (!b) return nullptr;
  b->name = name; b->next = env;
  return b;
}

// ============================================================================
// Compile Sexp -> Term (iterative list build)
// ============================================================================
inline Term compile_term(Arena& a, const GlobalBind* genv, const BoundBind* benv, const Sexp* x) {
  if (!x) return Term::nil();

  if (x->tag == SexpTag::Int) return Term::integer(x->i);

  if (x->tag == SexpTag::Sym) {
    std::uint32_t depth = 0;
    if (bound_find(benv, x->sym, depth)) return Term::bvar(depth);

    std::uint32_t id = 0;
    if (global_find(genv, x->sym, id)) return Term::var(id);

    return Term::symbol(x->sym);
  }

  // list
  const SexpCell* c = x->list.head;
  if (!c && !x->list.dotted) return Term::nil(); // ()

  // count elements
  std::uint32_t n = 0;
  for (auto* t = c; t; t = t->cdr) ++n;

  // temp array in arena
  Term* arr = static_cast<Term*>(a.alloc(sizeof(Term) * (n ? n : 1), alignof(Term)));
  if (!arr) return Term::nil();

  std::uint32_t i = 0;
  for (auto* t = c; t; t = t->cdr) arr[i++] = compile_term(a, genv, benv, t->car);

  Term tail = x->list.dotted ? compile_term(a, genv, benv, x->list.dotted) : Term::nil();

  for (std::int32_t k = (std::int32_t)n - 1; k >= 0; --k) {
    PairNode* p = a.make<PairNode>();
    if (!p) return Term::nil();
    p->car = arr[k];
    p->cdr = tail;
    tail = Term::make_pair(p);
  }

  return tail;
}

// ============================================================================
// Compile Sexp -> Goal (supports n-ary conj/disj by folding, plus fresh)
// ============================================================================
inline const Goal* compile_goal(Arena& a, const GlobalBind* genv, const BoundBind* benv, const Sexp* x) {
  if (!x || x->tag != SexpTag::List) return nullptr;

  const SexpCell* c = x->list.head;
  if (!c || !c->car || c->car->tag != SexpTag::Sym) return nullptr;

  const SymEntry* op = c->car->sym;
  c = c->cdr;

  if (sym_lit_eq(op, "==")) {
    if (!c || !c->cdr || c->cdr->cdr) return nullptr; // exactly 2 args
    Term u = compile_term(a, genv, benv, c->car);
    Term v = compile_term(a, genv, benv, c->cdr->car);
    return make_eq(a, u, v);
  }

  if (sym_lit_eq(op, "fresh")) {
    // syntax: (fresh (x y ...) GOAL)
    if (!c || !c->car || c->car->tag != SexpTag::List) return nullptr;
    const Sexp* vlist = c->car;
    const SexpCell* vc = vlist->list.head;
    if (!vc) return nullptr;

    // collect names (cap at 64 just to keep it bounded)
    const SymEntry* names[64];
    std::uint32_t n = 0;
    for (; vc; vc = vc->cdr) {
      if (!vc->car || vc->car->tag != SexpTag::Sym) return nullptr;
      if (n >= 64) return nullptr;
      names[n++] = vc->car->sym;
    }

    c = c->cdr;
    if (!c || !c->car || c->cdr) return nullptr; // exactly one body goal

    // Extend bound env by pushing in order: x then y => y becomes depth 0 in body.
    const BoundBind* benv2 = benv;
    for (std::uint32_t i = 0; i < n; ++i) {
      benv2 = bound_push(a, benv2, names[i]);
      if (!benv2) return nullptr;
    }

    const Goal* body = compile_goal(a, genv, benv2, c->car);
    if (!body) return nullptr;

    // Wrap with nested Fresh so outer binder is first name:
    // (fresh (x y) body) => Fresh(x, Fresh(y, body))
    const Goal* g = body;
    for (std::int32_t i = (std::int32_t)n - 1; i >= 0; --i) {
      g = make_fresh(a, names[(std::uint32_t)i], g);
      if (!g) return nullptr;
    }
    return g;
  }

  if (sym_lit_eq(op, "disj") || sym_lit_eq(op, "conj")) {
    const bool is_disj = sym_lit_eq(op, "disj");
    if (!c || !c->cdr) return nullptr; // at least 2

    const Goal* acc = compile_goal(a, genv, benv, c->car);
    if (!acc) return nullptr;
    c = c->cdr;

    while (c) {
      const Goal* rhs = compile_goal(a, genv, benv, c->car);
      if (!rhs) return nullptr;
      acc = is_disj ? make_disj(a, acc, rhs) : make_conj(a, acc, rhs);
      if (!acc) return nullptr;
      c = c->cdr;
    }
    return acc;
  }

  return nullptr;
}

// ============================================================================
// parse_query: (run N (q) GOAL) or GOAL only
// ============================================================================
struct ParsedQuery {
  int n;
  Term qvar;
  std::uint32_t vars_used;
  const Goal* goal;
  Intern intern;
};

inline ParsedQuery parse_query(Arena& a, const char* src) {
  ParsedQuery out{};
  out.n = 10;
  out.qvar = Term::var(0);
  out.vars_used = 1;
  out.goal = nullptr;
  out.intern = Intern{0, nullptr};

  if (!intern_init(a, out.intern, 256)) return out;

  Lexer lx{src};
  Token tok = lx.next();
  const Sexp* top = parse_sexp(a, out.intern, lx, tok);
  if (!top) return out;

  // (run N (q) GOAL)
  if (top->tag == SexpTag::List) {
    const SexpCell* c = top->list.head;
    if (c && c->car && c->car->tag == SexpTag::Sym && sym_lit_eq(c->car->sym, "run")) {
      c = c->cdr;
      if (!c || !c->car || c->car->tag != SexpTag::Int) return out;
      out.n = c->car->i;
      c = c->cdr;

      // (q) one query var
      if (!c || !c->car || c->car->tag != SexpTag::List) return out;
      const Sexp* qlist = c->car;
      const SexpCell* qcells = qlist->list.head;
      if (!qcells || qcells->cdr) return out;
      if (!qcells->car || qcells->car->tag != SexpTag::Sym) return out;

      // global env: q -> Var(0)
      const GlobalBind* genv = nullptr;
      genv = global_add(a, genv, qcells->car->sym, 0);
      if (!genv) return out;

      out.qvar = Term::var(0);
      out.vars_used = 1;

      c = c->cdr;
      if (!c || !c->car) return out;

      out.goal = compile_goal(a, genv, nullptr, c->car);
      return out;
    }
  }

  // Goal-only form: bind symbol 'q' to Var(0)
  const SymEntry* qsym = intern_cstr(a, out.intern, "q");
  const GlobalBind* genv = nullptr;
  genv = global_add(a, genv, qsym, 0);
  out.goal = compile_goal(a, genv, nullptr, top);
  return out;
}

// ============================================================================
// Printing (list-aware)
// ============================================================================
inline void print_term(Term t);

inline void print_list(Term t) {
  std::printf("(");
  bool first = true;
  while (true) {
    if (t.tag == TermTag::Nil) {
      std::printf(")");
      return;
    }
    if (t.tag != TermTag::Pair || !t.pair) {
      std::printf(" . ");
      print_term(t);
      std::printf(")");
      return;
    }
    const PairNode* p = t.pair;
    if (!first) std::printf(" ");
    print_term(p->car);
    t = p->cdr;
    first = false;
  }
}

inline void print_term(Term t) {
  switch (t.tag) {
    case TermTag::Int:  std::printf("%d", t.value); break;
    case TermTag::Nil:  std::printf("()"); break;
    case TermTag::Var:  std::printf("_.%u", t.id); break;
    case TermTag::BVar: std::printf("b_.%u", t.id); break; // should rarely appear at output
    case TermTag::Sym:  std::printf("%s", t.sym ? t.sym->str : "<sym?>"); break;
    case TermTag::Pair: print_list(t); break;
    default:            std::printf("<term?>"); break;
  }
}


