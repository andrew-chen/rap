// repl.cpp — Interactive query shell for the RAP core engine
//
// Usage:
//   ./repl                   # interactive
//   ./repl --verbose         # show parsed goal tree before running
//   ./repl --timing          # print µs per query after each result
//   ./repl < examples/foo.rkt  # scripted via redirection
//
// Compile (from the rap/ root):
//   clang++ -std=c++20 -O2 -Wall -Wextra -pedantic -Werror -o repl repl.cpp
//
// Covers core/ only — no RAP agenda layer.

#include "core/sexp_parser.hpp"
#include "core/core.hpp"
#include "core/intern.hpp"
#include "core/arena.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <iostream>
#include <unistd.h>

// ============================================================================
// Session-persistent storage (static so large buffers avoid stack overflow)
// ============================================================================

// intern_arena: long-lived. Holds interned symbols, RelEnv entries, and
// the parsed goal trees for defrel bodies.  Never reset.
alignas(64) static std::uint8_t intern_buf[64 * 1024];

// query_arena: per-query.  Reset at the start of each dispatch() call.
// Holds Evaluator working storage (Work, Kont, Binding, etc.).
alignas(64) static std::uint8_t query_buf[256 * 1024];


// ============================================================================
// merge_rel_env
//
// Deep-copies RelNode bodies from a per-parse RelEnv into intern_arena and
// registers them in the session rel_env.  The deep copy ensures that the
// stable (intern_arena-resident) RelNode does not alias parse temporaries.
// ============================================================================
static void merge_rel_env(Arena& intern_arena, Intern& /*sess_intern*/,
                          RelEnv& sess_rel_env,
                          const RelEnv& src) {
    for (const RelEnvEntry* e = src.head; e; e = e->next) {
        const Goal* body_copy =
            deep_copy_goal(intern_arena, e->rel_term.rel->body);

        RelNode* node = intern_arena.make<RelNode>();
        if (!node) {
            std::printf("error: out of memory — repl stopping\n");
            std::exit(1);
        }
        node->param_count = e->rel_term.rel->param_count;
        node->body        = body_copy;

        Term stable_rel = Term::relation(node);
        sess_rel_env.define(intern_arena, e->name, stable_rel);
    }
}


// ============================================================================
// dispatch — process one complete accumulated form
// ============================================================================
static void dispatch(const std::string& src,
                     Arena& intern_arena, Arena& query_arena,
                     Intern& sess_intern, OutcomeSyms& sess_syms,
                     RelEnv& sess_rel_env, Evaluator& eval,
                     bool verbose, bool timing) {
    query_arena.reset();

    // Parse into intern_arena so all newly-created SymEntries are stable
    // across subsequent query_arena resets.
    ParsedQuery pq = parse_query(intern_arena, src.c_str(),
                                 sess_intern, sess_rel_env);

    // Distinguish outcomes:
    //   goal==nullptr && rel_env.head==nullptr  →  parse error (already printed)
    //   goal==nullptr && rel_env.head!=nullptr  →  defrel-only, success
    //   goal!=nullptr                           →  has a run form, execute it
    if (!pq.goal && !pq.rel_env.head) {
        return;  // parse error
    }

    if (verbose && pq.goal) {
        // print_query needs pq to have outcome_syms set; it's populated
        // by the 4-arg parse_query using sess_intern, so it should be fine.
        print_query(pq);
    }

    // Defrel-only path (pq.n == 0)
    if (pq.n == 0) {
        merge_rel_env(intern_arena, sess_intern, sess_rel_env, pq.rel_env);
        // Print confirmation for each newly defined relation.
        for (const RelEnvEntry* e = pq.rel_env.head; e; e = e->next)
            std::printf("defined: %s\n", e->name->str);
        return;
    }

    // If the input also had defrels before the run form, merge them first.
    if (pq.rel_env.head) {
        merge_rel_env(intern_arena, sess_intern, sess_rel_env, pq.rel_env);
    }

    // Run the query using query_arena for evaluator working storage.
    // The eval was constructed with &query_arena; runN uses it internally.
    auto t0 = std::chrono::high_resolution_clock::now();
    int  count = 0;

    eval.runN(pq.n, pq.goal, pq.qvar, pq.vars_used,
              sess_rel_env,
              [&](Term ans, State) {
                  std::printf("q = ");
                  print_term(ans);
                  std::printf("\n");
                  ++count;
              });

    auto t1 = std::chrono::high_resolution_clock::now();

    if (count == 0) std::printf("(no solutions)\n");

    if (timing) {
        double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        std::printf("; %.1f \xc2\xb5s\n", us);  // µ is UTF-8 0xC2 0xB5
    }

    (void)sess_syms;  // sess_syms is used by eval (passed at construction)
}


// ============================================================================
// main
// ============================================================================
int main(int argc, char** argv) {
    bool verbose     = false;
    bool timing      = false;
    bool interactive = isatty(STDIN_FILENO) != 0;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--verbose") == 0) verbose = true;
        if (std::strcmp(argv[i], "--timing")  == 0) timing  = true;
    }

    Arena intern_arena(intern_buf, sizeof(intern_buf));
    Arena query_arena(query_buf, sizeof(query_buf));

    Intern intern{0, nullptr};
    if (!intern_init(intern_arena, intern, 256)) {
        std::fprintf(stderr, "error: intern_init failed (OOM?)\n");
        return 1;
    }

    OutcomeSyms syms{};
    syms.s_true         = intern_cstr(intern_arena, intern, "true");
    syms.s_false        = intern_cstr(intern_arena, intern, "false");
    syms.s_insufficient = intern_cstr(intern_arena, intern, "insufficient");
    syms.s_bounded      = intern_cstr(intern_arena, intern, "bounded");
    if (!syms.s_true || !syms.s_false ||
        !syms.s_insufficient || !syms.s_bounded) {
        std::fprintf(stderr, "error: failed to intern outcome symbols\n");
        return 1;
    }

    RelEnv  rel_env{};
    // Evaluator uses query_arena for working storage; syms for probe outcomes.
    Evaluator eval(&query_arena, &intern_arena, &intern, &syms);

    if (interactive) {
        std::printf("rap \xe2\x80\x94 Relational Agenda Programming\n");
        std::printf("Type (run N (q) ...) to query, (defrel ...) to define.\n");
        std::printf("EOF (Ctrl-D) to exit.\n\n");
        std::printf("rap> ");
        std::fflush(stdout);
    }

    std::string accumulator;
    int         depth      = 0;
    bool        seen_open  = false;  // true once we've seen at least one '('
    std::string line;

    while (std::getline(std::cin, line)) {
        // Count paren depth; stop counting at ';' (comment character).
        for (char c : line) {
            if (c == ';') break;
            if (c == '(') { ++depth; seen_open = true; }
            if (c == ')') { --depth; }
        }

        if (depth < 0) {
            std::printf("error: unmatched ')'\n");
            accumulator.clear();
            depth     = 0;
            seen_open = false;
            if (interactive) { std::printf("rap> "); std::fflush(stdout); }
            continue;
        }

        accumulator += line;
        accumulator += ' ';

        // Dispatch only when a complete form has been seen: depth returned to
        // 0 after having been > 0 (i.e. at least one '(' was encountered).
        // This correctly ignores comment-only lines and blank lines.
        if (depth == 0 && seen_open) {
            dispatch(accumulator,
                     intern_arena, query_arena,
                     intern, syms, rel_env, eval,
                     verbose, timing);
            accumulator.clear();
            seen_open = false;
            if (interactive) { std::printf("rap> "); std::fflush(stdout); }
        }
    }

    // EOF
    if (interactive) std::printf("\n");
    return 0;
}
