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
    // Overflow: true result does not fit in int32 → fail, not silent wrap.
    {
        auto [cnt, _1] = run_query("(run 1 (q) (addsubo 2000000000 2000000000 q))");
        EXPECT(cnt == 0, "addsubo overflow: 2e9+2e9 > INT32_MAX → 0 solutions");
        (void)_1;
    }
    {
        auto [cnt, ans] = run_query("(run 1 (q) (addsubo 2000000000 100000000 q))");
        EXPECT(cnt == 1, "addsubo 2e9+1e8=2.1e9: fits in int32 → 1 solution");
        EXPECT(ans == "2100000000", "addsubo 2e9+1e8: answer is 2100000000");
    }
    // Boundary: INT32_MAX itself must succeed.
    {
        auto [cnt, ans] = run_query("(run 1 (q) (addsubo 2147483646 1 q))");
        EXPECT(cnt == 1, "addsubo INT32_MAX-1 + 1 = INT32_MAX: 1 solution");
        EXPECT(ans == "2147483647", "addsubo boundary: answer is INT32_MAX");
    }
    // INT32_MAX + 1 must fail.
    {
        auto [cnt, _1] = run_query("(run 1 (q) (addsubo 2147483647 1 q))");
        EXPECT(cnt == 0, "addsubo INT32_MAX + 1 overflows → 0 solutions");
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
    // Overflow: true result does not fit in int32 → fail.
    // 1103515245 * 42 = 46347640290 — the original discovered bug.
    {
        auto [cnt, _1] = run_query("(run 1 (q) (multaddiso 1103515245 42 0 q))");
        EXPECT(cnt == 0, "multaddiso overflow: 1103515245*42 > INT32_MAX → 0 solutions");
        (void)_1;
    }
    // Boundary: 46340^2 = 2147396900 < INT32_MAX → should succeed.
    // 46341^2 = 2147488281 > INT32_MAX → should fail.
    {
        auto [cnt, ans] = run_query("(run 1 (q) (multaddiso 46340 46340 0 q))");
        EXPECT(cnt == 1, "multaddiso 46340^2=2147395600 fits → 1 solution");
        EXPECT(ans == "2147395600", "multaddiso 46340^2: answer is 2147395600");
    }
    {
        auto [cnt, _1] = run_query("(run 1 (q) (multaddiso 46341 46341 0 q))");
        EXPECT(cnt == 0, "multaddiso 46341^2=2147488281 > INT32_MAX → 0 solutions");
        (void)_1;
    }
    // INT32_MAX itself as a product must succeed.
    {
        auto [cnt, ans] = run_query("(run 1 (q) (multaddiso 1 2147483647 0 q))");
        EXPECT(cnt == 1, "multaddiso 1*INT32_MAX: 1 solution");
        EXPECT(ans == "2147483647", "multaddiso 1*INT32_MAX: answer is INT32_MAX");
    }

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
    // divmodo
    // =========================================================================
    std::printf("--- divmodo ---\n");

    // All-bound verify: 3*5+2=17, 0<=2<5.
    EXPECT(run_check("(divmodo 17 5 3 2)") == 1, "divmodo 17 5 3 2: 1 solution (verify)");
    EXPECT(run_check("(divmodo 17 5 4 2)") == 0, "divmodo 17 5 4 2: 0 solutions (wrong q)");
    EXPECT(run_check("(divmodo 17 5 3 3)") == 0, "divmodo 17 5 3 3: 0 solutions (wrong r)");

    // b <= 0 rejected.
    EXPECT(run_check("(divmodo 17 0 3 2)") == 0, "divmodo 17 0 3 2: 0 solutions (b=0)");
    EXPECT(run_check("(divmodo 17 -1 3 2)") == 0, "divmodo 17 -1 3 2: 0 solutions (b<0)");

    // q unknown (a, b, r known).
    {
        auto [cnt, ans] = run_query("(run 1 (q) (divmodo 17 5 q 2))");
        EXPECT(cnt == 1, "divmodo 17 5 q 2: 1 solution");
        EXPECT(ans == "3", "divmodo 17 5 q 2: q=3");
    }
    {
        auto [cnt, _1] = run_query("(run 1 (q) (divmodo 17 5 q 3))");
        EXPECT(cnt == 0, "divmodo 17 5 q 3: 0 solutions (r=3 not consistent)");
        (void)_1;
    }

    // r unknown (a, b, q known).
    {
        auto [cnt, ans] = run_query("(run 1 (q) (divmodo 17 5 3 q))");
        EXPECT(cnt == 1, "divmodo 17 5 3 q: 1 solution");
        EXPECT(ans == "2", "divmodo 17 5 3 q: q=2");
    }
    {
        auto [cnt, _1] = run_query("(run 1 (q) (divmodo 17 5 4 q))");
        EXPECT(cnt == 0, "divmodo 17 5 4 q: 0 solutions (q=4 => r=-3, invalid)");
        (void)_1;
    }

    // a unknown (b, q, r known): a = b*q + r.
    {
        auto [cnt, ans] = run_query("(run 1 (q) (divmodo q 5 3 2))");
        EXPECT(cnt == 1, "divmodo q 5 3 2: 1 solution");
        EXPECT(ans == "17", "divmodo q 5 3 2: q=17");
    }

    // b unknown (a, q, r known): b = (a-r)/q.
    {
        auto [cnt, ans] = run_query("(run 1 (q) (divmodo 17 q 3 2))");
        EXPECT(cnt == 1, "divmodo 17 q 3 2: 1 solution");
        EXPECT(ans == "5", "divmodo 17 q 3 2: q=5");
    }
    {
        auto [cnt, _1] = run_query("(run 1 (q) (divmodo 17 q 0 2))");
        EXPECT(cnt == 0, "divmodo 17 q 0 2: 0 solutions (q=0, b unconstrained)");
        (void)_1;
    }

    // Both q and r unknown (a, b known): floor division.
    {
        auto [cnt, ans] = run_query("(run 1 (q) (fresh (r) (divmodo 17 5 q r)))");
        EXPECT(cnt == 1, "divmodo 17 5 q r (both unknown): 1 solution");
        EXPECT(ans == "3", "divmodo 17 5 q r: q=3");
    }
    {
        auto [cnt, ans] = run_query("(run 1 (q) (fresh (dv) (divmodo 17 5 dv q)))");
        EXPECT(cnt == 1, "divmodo 17 5 (quotient) q (remainder): 1 solution");
        EXPECT(ans == "2", "divmodo 17 5 _ q: r=2");
    }
    {
        // 100 / 7 = 14 remainder 2
        auto [cnt, ans] = run_query("(run 1 (q) (fresh (r) (divmodo 100 7 q r)))");
        EXPECT(cnt == 1, "divmodo 100 7 q r: 1 solution");
        EXPECT(ans == "14", "divmodo 100 7 q r: q=14");
    }
    {
        // 0 / 5 = 0 remainder 0
        auto [cnt, ans] = run_query("(run 1 (q) (fresh (r) (divmodo 0 5 q r)))");
        EXPECT(cnt == 1, "divmodo 0 5 q r: 1 solution");
        EXPECT(ans == "0", "divmodo 0 5 q r: q=0");
    }
    {
        // Floor division: -7 / 5 = -2 remainder 3 (floor, not truncated -1 r -2)
        auto [cnt, ans] = run_query("(run 1 (q) (fresh (r) (divmodo -7 5 q r)))");
        EXPECT(cnt == 1, "divmodo -7 5 q r: 1 solution (floor division)");
        EXPECT(ans == "-2", "divmodo -7 5 q r: q=-2 (floor)");
    }
    {
        auto [cnt, ans] = run_query("(run 1 (q) (fresh (dv) (divmodo -7 5 dv q)))");
        EXPECT(cnt == 1, "divmodo -7 5 _ r: remainder=3");
        EXPECT(ans == "3", "divmodo -7 5 _ q: r=3");
    }
    {
        // Large value: 2147483647 / 3 = 715827882 r 1  (715827882*3=2147483646)
        auto [cnt, ans] = run_query("(run 1 (q) (fresh (r) (divmodo 2147483647 3 q r)))");
        EXPECT(cnt == 1, "divmodo 2147483647 3 q r: 1 solution");
        EXPECT(ans == "715827882", "divmodo 2147483647 3 q r: q=715827882");
    }

    // =========================================================================
    // divo and modo via inline defrels (using divmodo under the hood)
    // =========================================================================
    std::printf("--- divo/modo (inline defrel) ---\n");

    static const char divo_prefix[] =
        "(defrel (divo a b q r) (divmodo a b q r))"
        "(defrel (modo a b r) (fresh (q) (divo a b q r)))";

    auto run_divo = [&](const char* body) -> std::pair<int,std::string> {
        std::string src = std::string(divo_prefix) + "(run 1 (q) " + body + ")";
        return run_query(src.c_str(), 1);
    };
    auto check_divo = [&](const char* body) -> int {
        std::string src = std::string(divo_prefix) + "(run 1 (q) " + body + ")";
        return run_query(src.c_str(), 1).first;
    };

    // divo: basic cases.
    EXPECT(check_divo("(divo 17 5 3 2)") == 1, "divo 17 5 3 2: 1 solution (verify)");
    EXPECT(check_divo("(divo 17 5 4 2)") == 0, "divo 17 5 4 2: 0 solutions");
    {
        auto [cnt, ans] = run_divo("(divo 17 5 q 2)");
        EXPECT(cnt == 1, "divo 17 5 q 2: 1 solution");
        EXPECT(ans == "3", "divo 17 5 q 2: q=3");
    }
    {
        auto [cnt, ans] = run_divo("(divo 17 5 3 q)");
        EXPECT(cnt == 1, "divo 17 5 3 q: 1 solution");
        EXPECT(ans == "2", "divo 17 5 3 q: q=2");
    }
    {
        auto [cnt, ans] = run_divo("(divo q 5 3 2)");
        EXPECT(cnt == 1, "divo q 5 3 2: 1 solution");
        EXPECT(ans == "17", "divo q 5 3 2: q=17");
    }
    {
        auto [cnt, ans] = run_divo("(divo 17 q 3 2)");
        EXPECT(cnt == 1, "divo 17 q 3 2: 1 solution");
        EXPECT(ans == "5", "divo 17 q 3 2: q=5");
    }
    {
        // Both q and r unknown via divo.
        auto [cnt, ans] = run_divo("(fresh (r) (divo 17 5 q r))");
        EXPECT(cnt == 1, "divo 17 5 q r (both unknown): 1 solution");
        EXPECT(ans == "3", "divo 17 5 q r: q=3");
    }

    // modo: basic cases.
    {
        auto [cnt, ans] = run_divo("(modo 17 5 q)");
        EXPECT(cnt == 1, "modo 17 5 q: 1 solution");
        EXPECT(ans == "2", "modo 17 5 q: q=2");
    }
    {
        auto [cnt, ans] = run_divo("(modo 100 7 q)");
        EXPECT(cnt == 1, "modo 100 7 q: 1 solution");
        EXPECT(ans == "2", "modo 100 7 q: q=2");
    }
    {
        auto [cnt, ans] = run_divo("(modo 0 5 q)");
        EXPECT(cnt == 1, "modo 0 5 q: 1 solution");
        EXPECT(ans == "0", "modo 0 5 q: q=0");
    }
    {
        // Floor division: -7 mod 5 = 3 (not -2 from truncated division)
        auto [cnt, ans] = run_divo("(modo -7 5 q)");
        EXPECT(cnt == 1, "modo -7 5 q: 1 solution (floor)");
        EXPECT(ans == "3", "modo -7 5 q: q=3 (floor mod)");
    }
    {
        // Large INT32-safe value: 2147483647 mod 3 = 1
        auto [cnt, ans] = run_divo("(modo 2147483647 3 q)");
        EXPECT(cnt == 1, "modo 2147483647 3 q: 1 solution");
        EXPECT(ans == "1", "modo 2147483647 3 q: q=1");
    }

    // =========================================================================
    // Overflow: mulo (via inline defrel, same multaddiso path)
    // =========================================================================
    std::printf("--- mulo overflow ---\n");
    {
        // Original discovered bug: 1103515245 * 42 = 46347640290, doesn't fit.
        // Previously produced -896999966 silently; now must fail.
        std::string src = std::string(divo_prefix) +
            "(run 1 (q) (fresh (r) (mulo 1103515245 42 r) (== q r)))";
        // Note: mulo uses multaddiso under the hood; divo_prefix defines divo/modo only.
        // Use multaddiso directly for mulo test.
    }
    {
        // Test mulo via multaddiso (mulo a b c ≡ multaddiso a b 0 c).
        auto [cnt, _1] = run_query("(run 1 (q) (multaddiso 1103515245 42 0 q))");
        EXPECT(cnt == 0, "mulo overflow (via multaddiso): 1103515245*42 → 0 solutions");
        (void)_1;
    }

    // =========================================================================
    // Overflow: divmodo a_var case (b*q+r can overflow int32)
    // =========================================================================
    std::printf("--- divmodo a_var overflow ---\n");
    {
        // b=1000000, q=3000, r=0 → a = 3e9 > INT32_MAX → must fail.
        auto [cnt, _1] = run_query("(run 1 (q) (divmodo q 1000000 3000 0))");
        EXPECT(cnt == 0, "divmodo a_var: b*q+r=3e9 > INT32_MAX → 0 solutions");
        (void)_1;
    }
    {
        // b=1, q=INT32_MAX, r=0 → a = INT32_MAX → must succeed.
        auto [cnt, ans] = run_query("(run 1 (q) (divmodo q 1 2147483647 0))");
        EXPECT(cnt == 1, "divmodo a_var: b*q+r=INT32_MAX → 1 solution");
        EXPECT(ans == "2147483647", "divmodo a_var: answer is INT32_MAX");
    }

    // =========================================================================
    // Summary
    // =========================================================================
    std::printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return (failed == 0) ? 0 : 1;
}
