#include "sexp_parser.hpp"
#include <cstdio>

int main() {
  alignas(64) unsigned char mem[1 << 16]; // 64 KiB arena
  Arena a(mem, sizeof(mem));

  const char* program1 = "(run 10 (q) (disj (== q foo) (== q bar)))";
  const char* program2 = "(conj (== q (1 2 3)) (== q (1 2 3)))";
  const char* program3 = "(run 10 (q) (== q (a b . c)))";

  // NEW: surface-language fresh; expects q unified with a fresh logic var
  const char* program4 = "(run 5 (q) (fresh (x) (== q x)))";

  const char* programs[] = {program1, program2, program3, program4};

  for (const char* program : programs) {
    std::printf("Program: %s\n", program);

    ParsedQuery pq = parse_query(a, program);
    if (!pq.goal) {
      std::printf("Parse/compile failed.\n\n");
      a.reset();
      continue;
    }

    runN(a, pq.n, pq.goal, pq.qvar, pq.vars_used,
     [&] (Term ans, State /*st*/) {
       print_term(ans);
       std::printf("\n");
     });

    std::printf("\n");
    a.reset();
  }

  return 0;
}

