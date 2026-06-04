// core/test_arith.cpp — Test suite for STAGE_ARITH arithmetic and constraint built-ins.
//
// Tests: leqo, lto, geqo, gto, eqo, neqo, addsubo, multaddiso, charo,
//        constraint interactions, and =/= regression.
//
// Build from project root:
//   clang++ -std=c++20 -O2 -Wall -Wextra -pedantic -Werror \
//           -o test_arith core/test_arith.cpp

#include "sexp_parser.hpp"
#include <cstdio>
#include <cstring>
#include <string>

// ============================================================================
// Test infrastructure
// ============================================================================
static int passed = 0;
static int failed = 0;

#define EXPECT(cond, msg) \
  do { if (cond) { ++passed; std::printf("PASS: %s\n", msg); } \
       else { ++failed; std::printf("FAIL: %s\n", msg); } } while (0)

// run_query: parse and run src, return (count, first_answer_as_string).
// Uses a fresh arena and fresh intern per call — no state shared between calls.
// n_limit: cap on answers collected (used when src has a large run N).
static std::pair<int, std::string> run_query(const char* src, int n_limit = 10) {
    // Use a large static buffer; safe because intern is fresh each call.
    alignas(64) static std::uint8_t mem[512 * 1024];

    // Fresh arena view starting from the beginning each call.
    Arena a(mem, sizeof(mem));

    // 2-arg parse_query creates a fresh intern inside 'a' each call.
    ParsedQuery pq = parse_query(a, src);
    if (!pq.goal) return {-1, "parse_error"};

    // Evaluator uses 'a' for both working storage and symbol interning.
    // pq.intern lives in 'a' and is valid for this call's duration.
    Evaluator eval(&a, &a, &pq.intern, &pq.outcome_syms);

    int run_n = pq.n;
    if (n_limit < run_n) run_n = n_limit;

    int count = 0;
    std::string first_ans;

    eval.runN(run_n, pq.goal, pq.qvar, pq.vars_used, pq.rel_env,
              [&](Term ans, State) {
                  if (count == 0) {
                      char buf[64];
                      if (ans.tag == TermTag::Int) {
                          std::snprintf(buf, sizeof(buf), "%d", ans.value);
                          first_ans = buf;
                      } else if (ans.tag == TermTag::Sym && ans.sym) {
                          first_ans = ans.sym->str;
                      } else if (ans.tag == TermTag::Var) {
                          std::snprintf(buf, sizeof(buf), "_.%u", ans.id);
                          first_ans = buf;
                      } else {
                          first_ans = "<other>";
                      }
                  }
                  ++count;
              });
    return {count, first_ans};
}

// run_check: wrap body_expr in (run 1 (q) ...) and return solution count.
static int run_check(const char* body_expr) {
    char src[512];
    std::snprintf(src, sizeof(src), "(run 1 (q) %s)", body_expr);
    return run_query(src, 1).first;
}

int main() {
    std::printf("=== STAGE_ARITH tests ===\n\n");

    // =========================================================================
    // Comparison: leqo
    // =========================================================================
    std::printf("--- leqo ---\n");
    EXPECT(run_check("(leqo 3 5)")  == 1, "leqo 3 5: 1 solution");
    EXPECT(run_check("(leqo 5 3)")  == 0, "leqo 5 3: 0 solutions");
    EXPECT(run_check("(leqo 3 3)")  == 1, "leqo 3 3: 1 solution (equal)");

    {
        auto [cnt, ans] = run_query("(run 1 (q) (leqo q 5) (== q 3))");
        EXPECT(cnt == 1, "leqo q 5, q=3: 1 solution");
        EXPECT(ans == "3", "leqo q 5, q=3: answer is 3");
    }
    {
        auto [cnt, _1] = run_query("(run 1 (q) (leqo q 5) (== q 6))");
        EXPECT(cnt == 0, "leqo q 5, q=6: 0 solutions (constraint fires)");
        (void)_1;
    }
    {
        auto [cnt, ans] = run_query("(run 1 (q) (leqo q 5) (== q 5))");
        EXPECT(cnt == 1, "leqo q 5, q=5: 1 solution");
        EXPECT(ans == "5", "leqo q 5, q=5: answer is 5");
    }

    // =========================================================================
    // Comparison: lto
    // =========================================================================
    std::printf("--- lto ---\n");
    EXPECT(run_check("(lto 3 5)") == 1, "lto 3 5: 1 solution");
    EXPECT(run_check("(lto 3 3)") == 0, "lto 3 3: 0 solutions");

    // =========================================================================
    // Comparison: geqo
    // =========================================================================
    std::printf("--- geqo ---\n");
    EXPECT(run_check("(geqo 5 3)") == 1, "geqo 5 3: 1 solution");
    EXPECT(run_check("(geqo 3 5)") == 0, "geqo 3 5: 0 solutions");

    // =========================================================================
    // Comparison: gto
    // =========================================================================
    std::printf("--- gto ---\n");
    EXPECT(run_check("(gto 5 3)") == 1, "gto 5 3: 1 solution");
    EXPECT(run_check("(gto 3 3)") == 0, "gto 3 3: 0 solutions");

    // =========================================================================
    // Comparison: eqo
    // =========================================================================
    std::printf("--- eqo ---\n");
    EXPECT(run_check("(eqo 3 3)") == 1, "eqo 3 3: 1 solution");
    EXPECT(run_check("(eqo 3 4)") == 0, "eqo 3 4: 0 solutions");
    {
        auto [cnt, ans] = run_query("(run 1 (q) (eqo q 7))");
        EXPECT(cnt == 1, "eqo q 7: 1 solution");
        EXPECT(ans == "7", "eqo q 7: answer is 7");
    }

    // =========================================================================
    // Comparison: neqo
    // =========================================================================
    std::printf("--- neqo ---\n");
    EXPECT(run_check("(neqo 3 4)") == 1, "neqo 3 4: 1 solution");
    EXPECT(run_check("(neqo 3 3)") == 0, "neqo 3 3: 0 solutions");
    {
        auto [cnt, _1] = run_query("(run 1 (q) (neqo q 3) (== q 3))");
        EXPECT(cnt == 0, "neqo q 3, q=3: 0 solutions (constraint fires)");
        (void)_1;
    }
    {
        auto [cnt, ans] = run_query("(run 1 (q) (neqo q 3) (== q 4))");
        EXPECT(cnt == 1, "neqo q 3, q=4: 1 solution");
        EXPECT(ans == "4", "neqo q 3, q=4: answer is 4");
    }

    // =========================================================================
    // addsubo
    // =========================================================================
    std::printf("--- addsubo ---\n");
    EXPECT(run_check("(addsubo 3 4 7)") == 1, "addsubo 3 4 7: 1 solution");
    EXPECT(run_check("(addsubo 3 4 8)") == 0, "addsubo 3 4 8: 0 solutions");
    {
        auto [cnt, ans] = run_query("(run 1 (q) (addsubo 3 4 q))");
        EXPECT(cnt == 1, "addsubo 3 4 q: 1 solution");
        EXPECT(ans == "7", "addsubo 3 4 q: answer is 7");
    }
    {
        auto [cnt, ans] = run_query("(run 1 (q) (addsubo 3 q 7))");
        EXPECT(cnt == 1, "addsubo 3 q 7: 1 solution");
        EXPECT(ans == "4", "addsubo 3 q 7: answer is 4");
    }
    {
        auto [cnt, ans] = run_query("(run 1 (q) (addsubo q 4 7))");
        EXPECT(cnt == 1, "addsubo q 4 7: 1 solution");
        EXPECT(ans == "3", "addsubo q 4 7: answer is 3");
    }
    {
        auto [cnt, ans] = run_query("(run 1 (q) (addsubo q 4 3))");
        EXPECT(cnt == 1, "addsubo q 4 3: 1 solution");
        EXPECT(ans == "-1", "addsubo q 4 3: answer is -1");
    }
    {
        auto [cnt, _1] = run_query("(run 1 (q) (addsubo q q q))");
        EXPECT(cnt == 0, "addsubo q q q: 0 solutions (two unbound)");
        (void)_1;
    }

    // =========================================================================
    // multaddiso
    // =========================================================================
    std::printf("--- multaddiso ---\n");
    EXPECT(run_check("(multaddiso 3 4 0 12)") == 1, "multaddiso 3 4 0 12: 1 solution");
    EXPECT(run_check("(multaddiso 3 4 2 14)") == 1, "multaddiso 3 4 2 14: 1 solution");
    EXPECT(run_check("(multaddiso 3 4 2 15)") == 0, "multaddiso 3 4 2 15: 0 solutions");
    {
        auto [cnt, ans] = run_query("(run 1 (q) (multaddiso 3 4 0 q))");
        EXPECT(cnt == 1, "multaddiso 3 4 0 q: 1 solution");
        EXPECT(ans == "12", "multaddiso 3 4 0 q: answer is 12");
    }
    {
        auto [cnt, ans] = run_query("(run 1 (q) (multaddiso 3 4 2 q))");
        EXPECT(cnt == 1, "multaddiso 3 4 2 q: 1 solution");
        EXPECT(ans == "14", "multaddiso 3 4 2 q: answer is 14");
    }
    {
        auto [cnt, ans] = run_query("(run 1 (q) (multaddiso q 4 0 12))");
        EXPECT(cnt == 1, "multaddiso q 4 0 12: 1 solution");
        EXPECT(ans == "3", "multaddiso q 4 0 12: answer is 3");
    }
    {
        auto [cnt, ans] = run_query("(run 1 (q) (multaddiso 3 q 0 12))");
        EXPECT(cnt == 1, "multaddiso 3 q 0 12: 1 solution");
        EXPECT(ans == "4", "multaddiso 3 q 0 12: answer is 4");
    }
    {
        auto [cnt, ans] = run_query("(run 1 (q) (multaddiso 3 4 q 14))");
        EXPECT(cnt == 1, "multaddiso 3 4 q 14: 1 solution");
        EXPECT(ans == "2", "multaddiso 3 4 q 14: answer is 2");
    }
    {
        auto [cnt, _1] = run_query("(run 1 (q) (multaddiso q 4 0 13))");
        EXPECT(cnt == 0, "multaddiso q 4 0 13: 0 solutions (13 not div by 4)");
        (void)_1;
    }
    {
        auto [cnt, _1] = run_query("(run 1 (q) (multaddiso q 0 3 3))");
        EXPECT(cnt == 0, "multaddiso q 0 3 3: 0 solutions (b=0, c==d, a unconstrained)");
        (void)_1;
    }
    {
        auto [cnt, _1] = run_query("(run 1 (q) (multaddiso q 0 3 4))");
        EXPECT(cnt == 0, "multaddiso q 0 3 4: 0 solutions (b=0, c!=d)");
        (void)_1;
    }
    EXPECT(run_check("(multaddiso 5 0 3 3)") == 1,
           "multaddiso 5 0 3 3: 1 solution (5*0+3=3, all bound)");

    // =========================================================================
    // charo
    // =========================================================================
    std::printf("--- charo ---\n");
    EXPECT(run_check("(charo a 97)") == 1, "charo a 97: 1 solution");
    EXPECT(run_check("(charo a 98)") == 0, "charo a 98: 0 solutions");
    {
        auto [cnt, ans] = run_query("(run 1 (q) (charo a q))");
        EXPECT(cnt == 1, "charo a q: 1 solution");
        EXPECT(ans == "97", "charo a q: answer is 97");
    }
    {
        auto [cnt, ans] = run_query("(run 1 (q) (charo q 97))");
        EXPECT(cnt == 1, "charo q 97: 1 solution");
        EXPECT(ans == "a", "charo q 97: answer is symbol 'a'");
    }
    {
        auto [cnt, ans] = run_query("(run 1 (q) (charo q 65))");
        EXPECT(cnt == 1, "charo q 65: 1 solution");
        EXPECT(ans == "A", "charo q 65: answer is symbol 'A'");
    }
    {
        auto [cnt, _1] = run_query("(run 1 (q) (charo q 0))");
        EXPECT(cnt == 0, "charo q 0: 0 solutions (out of range)");
        (void)_1;
    }
    {
        auto [cnt, _1] = run_query("(run 1 (q) (charo q 128))");
        EXPECT(cnt == 0, "charo q 128: 0 solutions (out of range)");
        (void)_1;
    }
    {
        // Both unbound: enumerate 32..126 (95 printable ASCII).
        // (run 3) collects first 3: n=32 (' '), n=33 ('!'), n=34 ('"').
        auto [cnt, first] = run_query("(run 3 (q) (fresh (n) (charo q n)))", 3);
        EXPECT(cnt == 3, "charo both-unbound: run 3 gives 3 solutions");
        // First symbol should be ' ' (space, ASCII 32).
        EXPECT(first.size() == 1 && first[0] == ' ',
               "charo both-unbound: first answer is space (ASCII 32)");
    }

    // =========================================================================
    // Constraint interaction
    // =========================================================================
    std::printf("--- constraint interaction ---\n");
    {
        auto [cnt, _1] = run_query("(run 1 (q) (=/= q foo) (== q foo))");
        EXPECT(cnt == 0, "=/= q foo, q=foo: 0 solutions (existing =/= still works)");
        (void)_1;
    }
    {
        // (leqo q 5) records {q, 5, 0, Gt} (fire if q > 5)
        // (leqo 3 q) records {q, 3, 0, Lt} (fire if q < 3)
        // No further unification: q stays unbound. 1 solution with q = _.N.
        auto [cnt, ans] = run_query("(run 1 (q) (leqo q 5) (leqo 3 q))");
        EXPECT(cnt == 1, "leqo q 5 + leqo 3 q: 1 solution (q unbound, constraints recorded)");
        EXPECT(ans.size() >= 2 && ans[0] == '_' && ans[1] == '.',
               "leqo constraints: q is an unbound variable (_.N)");
        std::printf("  NOTE: both-unbound leqo fails so no enumeration occurs.\n");
        std::printf("        q=%s (unbound with two recorded constraints).\n", ans.c_str());
        std::printf("        Constraints: fail if q>5 OR fail if q<3.\n");
        std::printf("        (run 3) also gives 1 solution — see below.\n");
    }
    {
        auto [cnt, _1] = run_query("(run 3 (q) (leqo q 5) (leqo 3 q))", 3);
        EXPECT(cnt == 1, "run 3 with leqo constraints: 1 solution (no enumeration)");
        (void)_1;
    }

    // =========================================================================
    // Summary
    // =========================================================================
    std::printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return (failed == 0) ? 0 : 1;
}
