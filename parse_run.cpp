#include "core/sexp_parser.hpp"
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

  const char* programs[] = {
    program1, program2, program3, program4, program5, program6,
    program7, program8, program9, program10, program11, program12
  };

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
    Evaluator eval(&a, &pq.outcome_syms);
    eval.runN(pq.n, pq.goal, pq.qvar, pq.vars_used, pq.rel_env,
      [&](Term ans, State /*st*/) {
        std::printf("  ");
        print_term(ans);
        std::printf("\n");
      });

    std::printf("\n");
    a.reset();
  }

  return 0;
}
