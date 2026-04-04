#include "rap.hpp"
#include <cstdio>

// Minimal smoke test: verify the rap layer can instantiate a core engine
// and run a simple query end-to-end.
int main() {
  alignas(64) unsigned char mem[1 << 16];
  Arena a(mem, sizeof(mem));

  RapEngine eng;
  if (!RapEngine::init(a, eng)) {
    std::printf("FAIL: RapEngine::init\n");
    return 1;
  }

  // Parse a simple query using the core sexp parser.
  ParsedQuery pq = parse_query(a, "(run 2 (q) (disj (== q ok) (== q fail)))");
  if (!pq.goal) {
    std::printf("FAIL: parse_query\n");
    return 1;
  }

  int count = 0;
  eng.run(pq.n, pq.goal, pq.qvar, pq.vars_used,
          [&](Term ans, State /*st*/) {
            std::printf("answer: ");
            print_term(ans);
            std::printf("\n");
            ++count;
          });

  std::printf("%s: got %d answer(s)\n", count == 2 ? "PASS" : "FAIL", count);
  return count == 2 ? 0 : 1;
}
