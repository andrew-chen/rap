#include "core/sexp_parser.hpp"
#include <chrono>
#include <cstdio>

int main() {
  alignas(64) unsigned char mem[1 << 17]; // 128 KiB arena (programs 7-12 need more)
  Arena a(mem, sizeof(mem));

  // ---- Programs 1-6 (unchanged) ----
  const char* program1 = "(run 10 (q) (disj (== q foo) (== q bar)))";
  const char* program2 = "(conj (== q (1 2 3)) (== q (1 2 3)))";
  const char* program3 = "(run 10 (q) (== q (a b . c)))";
  const char* program4 = "(run 5 (q) (fresh (x) (== q x)))";
  const char* program5 = "(run 5 (q) (fresh (x y) (disj (== q x) (== q y))))";
  const char* program6 = "(run 1 (q) (probe (== q foo) insufficient 50 true true))";

  // ---- Programs 7-12 (Stage 0A) ----

  // Program 7: Simple named relation
  const char* program7 =
    "(defrel (same x y) (== x y))"
    "(run 3 (q) (call same q foo))";

  // Program 8: Recursive relation (appendo)
  const char* program8 =
    "(defrel (appendo l r o)"
    "  (disj"
    "    (conj (== l ()) (== o r))"
    "    (fresh (h t res)"
    "      (conj (== l (h . t))"
    "            (conj (== o (h . res))"
    "                  (call appendo t r res))))))"
    "(run 3 (q) (call appendo (1 2) (3 4) q))";

  // Program 9: Relation passed as argument
  const char* program9 =
    "(defrel (apply-rel r x) (call r x))"
    "(defrel (is-foo x) (== x foo))"
    "(run 1 (q) (call apply-rel is-foo q))";

  // Program 10: Mutual recursion via fresh
  const char* program10 =
    "(run 3 (q)"
    "  (fresh (eveno oddo)"
    "    (== eveno (rel (n) (disj (== n 0)"
    "                             (fresh (m) (conj (== n (s m))"
    "                                             (call oddo m))))))"
    "    (== oddo  (rel (n) (fresh (m) (conj (== n (s m))"
    "                                        (call eveno m)))))"
    "    (call eveno q)))";

  // Program 11: Run backward (relational property)
  const char* program11 =
    "(defrel (appendo l r o)"
    "  (disj"
    "    (conj (== l ()) (== o r))"
    "    (fresh (h t res)"
    "      (conj (== l (h . t))"
    "            (conj (== o (h . res))"
    "                  (call appendo t r res))))))"
    "(run 3 (q) (call appendo q (3 4) (1 2 3 4)))";

  // Program 12: Inline cache correctness
  const char* program12 =
    "(defrel (is-bar x) (== x bar))"
    "(run 1 (q) (call is-bar q))";

  // Program 13: Disequality constraint — foo excluded, bar and baz produced
  const char* program13 =
    "(run 5 (q)"
    "  (conj"
    "    (=/= q foo)"
    "    (disj (== q foo) (== q bar) (== q baz))))";

  // ---- Programs 14-16: Guide accuracy tests ----
  // These programs exercise claims in docs/RAP_PROGRAMMING_GUIDE.md.

  // Program 14: geqo (>=) exists as a built-in — the guide omits it from the
  // comparisons list but it is a real relation alongside leqo/lto/gto/eqo.
  // Expected: (5) — geqo 5 3 succeeds, geqo 2 3 fails.
  const char* program14 =
    "(run 1 (q)"
    "  (conj"
    "    (geqo q 3)"
    "    (== q 5)"
    "    (geqo 5 3)))";

  // Program 15: neqo (!=) exists as a built-in — also omitted from the guide's
  // comparisons list. Expected: (4) — neqo 4 3 succeeds, neqo 3 3 fails.
  const char* program15 =
    "(run 1 (q)"
    "  (conj"
    "    (neqo q 3)"
    "    (== q 4)"
    "    (neqo 4 3)))";

  // Program 16: disj requires >= 2 args, same as conj — the guide says only
  // conj has this restriction, but both do. This program should FAIL to parse
  // with '[compile_goal] ERROR: disj requires at least 2 args'. The 'skipping
  // run' line in the output confirms the parse failed as expected.
  const char* program16 =
    "(run 1 (q) (disj (== q foo)))";

  const char* programs[] = {
    program1, program2, program3, program4, program5, program6,
    program7, program8, program9, program10, program11, program12,
    program13,
    program14, program15, program16
  };

  // ---- Diagnostic pass: print goal trees and results (one run each) ----

  for (const char* src : programs) {
    std::printf("----------------------------------------\n");
    std::printf("Program: %s\n\n", src);

    ParsedQuery pq = parse_query(a, src);

    print_query(pq);

    if (!pq.goal) {
      std::printf("(skipping run — parse failed)\n\n");
      a.reset();
      continue;
    }

    std::printf("Results:\n");
    Evaluator eval(&a, &a, &pq.intern, &pq.outcome_syms);
    eval.runN(pq.n, pq.goal, pq.qvar, pq.vars_used, pq.rel_env,
      [&](Term ans, State /*st*/) {
        std::printf("  ");
        print_term(ans);
        std::printf("\n");
      });

    std::printf("\n");
    a.reset();
  }

  // ---- Benchmark pass: 5 warmup + 100 measured iterations per program ----
  // printf is suppressed during the hot loop to avoid measuring I/O.
  // Each iteration calls parse_query then runN on a freshly reset arena.
  // Averages are reported in µs to one decimal place.

  std::printf("========================================\n");
  std::printf("Parse vs. Evaluation Timing (avg over 100 iterations, 5 warmup)\n");
  std::printf("========================================\n");

  int prog_num = 0;
  for (const char* src : programs) {
    ++prog_num;

    // Pre-loop validity check: ensure the program parses before starting
    // the benchmark. A failed parse here would silently produce meaningless
    // timing numbers against a null goal.
    {
      ParsedQuery pq_check = parse_query(a, src);
      bool ok = (pq_check.goal != nullptr);
      a.reset();
      if (!ok) {
        std::printf("Program %2d: [SKIP — parse failed]\n", prog_num);
        continue;
      }
    }

    using clock = std::chrono::high_resolution_clock;

    // Warmup: 5 iterations, timing discarded.
    for (int w = 0; w < 5; ++w) {
      ParsedQuery pq = parse_query(a, src);
      if (pq.goal) {
        Evaluator eval(&a, &a, &pq.intern, &pq.outcome_syms);
        eval.runN(pq.n, pq.goal, pq.qvar, pq.vars_used, pq.rel_env,
                  [](Term, State) {});
      }
      a.reset();
    }

    // Measurement: 100 iterations.
    long long total_parse_ns = 0, total_eval_ns = 0;
    for (int i = 0; i < 100; ++i) {
      auto t0 = clock::now();
      ParsedQuery pq = parse_query(a, src);
      auto t1 = clock::now();
      total_parse_ns +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

      if (pq.goal) {
        Evaluator eval(&a, &a, &pq.intern, &pq.outcome_syms);
        auto t2 = clock::now();
        eval.runN(pq.n, pq.goal, pq.qvar, pq.vars_used, pq.rel_env,
                  [](Term, State) {});
        auto t3 = clock::now();
        total_eval_ns +=
            std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();
      }
      a.reset();
    }

    double parse_us = total_parse_ns / 100.0 / 1000.0;
    double eval_us  = total_eval_ns  / 100.0 / 1000.0;
    double total_us = parse_us + eval_us;
    std::printf("Program %2d: parse: %5.1f \xc2\xb5s  eval: %5.1f \xc2\xb5s  total: %5.1f \xc2\xb5s\n",
                prog_num, parse_us, eval_us, total_us);
  }

  return 0;
}
