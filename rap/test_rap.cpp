#include "rap.hpp"
#include <cstdio>

// ============================================================================
// Stage 0B test suite for RapEvaluator.
//
// Tests:
//   1. RapEvaluator constructs without error and runs a simple query
//   2. 'no-ops' succeeds (1 solution) and allocates into the client region
//   3. 'cons-ops' with 3 args succeeds (1 solution)
//   4. 'cons-ops' with wrong arg count fails (0 solutions)
//   5. A fully unknown relation fails (0 solutions, no crash)
// ============================================================================

int main() {
  alignas(64) unsigned char mem[1 << 16];  // 64 KiB — enough for all tests
  Arena a(mem, sizeof(mem));

  // ---- Test 1: Construction + simple query ----
  // Each test uses parse_query to get its own intern table, then constructs
  // a RapEvaluator using that intern table so pointer-identity comparisons
  // for 'no-ops'/'cons-ops' work correctly at runtime.
  {
    ParsedQuery pq = parse_query(a, "(run 2 (q) (disj (== q ok) (== q fail)))");
    if (!pq.goal) {
      std::printf("FAIL: parse simple query\n");
      return 1;
    }

    RapEvaluator eval(&a, &pq.intern, &pq.outcome_syms);

    int count = 0;
    eval.runN(pq.n, pq.goal, pq.qvar, pq.vars_used, pq.rel_env,
              [&](Term ans, State /*st*/) {
                std::printf("answer: ");
                print_term(ans);
                std::printf("\n");
                ++count;
              });
    std::printf("%s: got %d answer(s)\n", count == 2 ? "PASS" : "FAIL", count);
    if (count != 2) return 1;
    a.reset();
  }

  // ---- Test 2: no-ops succeeds; client region used ----
  {
    ParsedQuery pq = parse_query(a, "(run 1 (q) (call no-ops q))");
    if (!pq.goal) {
      std::printf("FAIL: parse no-ops query\n");
      return 1;
    }

    RapEvaluator eval(&a, &pq.intern, &pq.outcome_syms);

    int count = 0;
    eval.runN(pq.n, pq.goal, pq.qvar, pq.vars_used, pq.rel_env,
              [&](Term, State) { ++count; });

    std::uint32_t off = eval.client_region_offset();
    std::printf("%s: no-ops count=%d\n",        count == 1 ? "PASS" : "FAIL", count);
    std::printf("%s: client_region offset=%u\n", off   >  0 ? "PASS" : "FAIL", off);
    if (count != 1 || off == 0) return 1;
    a.reset();
  }

  // ---- Test 3: cons-ops with 3 args succeeds ----
  {
    ParsedQuery pq = parse_query(a, "(run 1 (q) (call cons-ops a b q))");
    if (!pq.goal) {
      std::printf("FAIL: parse cons-ops query\n");
      return 1;
    }

    RapEvaluator eval(&a, &pq.intern, &pq.outcome_syms);

    int count = 0;
    eval.runN(pq.n, pq.goal, pq.qvar, pq.vars_used, pq.rel_env,
              [&](Term, State) { ++count; });
    std::printf("%s: cons-ops/3 count=%d\n", count == 1 ? "PASS" : "FAIL", count);
    if (count != 1) return 1;
    a.reset();
  }

  // ---- Test 4: cons-ops with wrong arg count fails ----
  {
    ParsedQuery pq = parse_query(a, "(run 1 (q) (call cons-ops a))");
    if (!pq.goal) {
      std::printf("FAIL: parse cons-ops/1 query\n");
      return 1;
    }

    RapEvaluator eval(&a, &pq.intern, &pq.outcome_syms);

    int count = 0;
    eval.runN(pq.n, pq.goal, pq.qvar, pq.vars_used, pq.rel_env,
              [&](Term, State) { ++count; });
    std::printf("%s: cons-ops wrong arity count=%d (expected 0)\n",
                count == 0 ? "PASS" : "FAIL", count);
    if (count != 0) return 1;
    a.reset();
  }

  // ---- Test 5: Fully unknown relation fails (no crash) ----
  {
    ParsedQuery pq = parse_query(a, "(run 1 (q) (call unknown-relation q))");
    if (!pq.goal) {
      std::printf("FAIL: parse unknown-relation query\n");
      return 1;
    }

    RapEvaluator eval(&a, &pq.intern, &pq.outcome_syms);

    int count = 0;
    eval.runN(pq.n, pq.goal, pq.qvar, pq.vars_used, pq.rel_env,
              [&](Term, State) { ++count; });
    std::printf("%s: unknown relation count=%d (expected 0)\n",
                count == 0 ? "PASS" : "FAIL", count);
    if (count != 0) return 1;
    a.reset();
  }

  std::printf("PASS: all tests passed\n");
  return 0;
}
