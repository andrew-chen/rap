#include "core/sexp_parser.hpp"
#include <cstdio>

int main() {
  alignas(64) unsigned char mem[1 << 16]; // 64 KiB arena
  Arena a(mem, sizeof(mem));

  const char* program1 = "(run 10 (q) (disj (== q foo) (== q bar)))";
  const char* program2 = "(conj (== q (1 2 3)) (== q (1 2 3)))";
  const char* program3 = "(run 10 (q) (== q (a b . c)))";
  const char* program4 = "(run 5 (q) (fresh (x) (== q x)))";
  const char* program5 = "(run 5 (q) (fresh (x y) (disj (== q x) (== q y))))";
  const char* program6 = "(run 1 (q) (probe (== q foo) insufficient 50 true true))";

  const char* programs[] = {
    program1, program2, program3, program4, program5, program6
  };

  for (const char* src : programs) {
    std::printf("----------------------------------------\n");
    std::printf("Program: %s\n\n", src);

    ParsedQuery pq = parse_query(a, src);

    // Always print diagnostic info — this shows parse success/failure
    // and the compiled goal tree so we can verify structure.
    print_query(pq);

    if (!pq.goal) {
      std::printf("(skipping run — parse failed)\n\n");
      a.reset();
      continue;
    }

    std::printf("Results:\n");
    runN(a, pq.n, pq.goal, pq.qvar, pq.vars_used, pq.outcome_syms,
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
