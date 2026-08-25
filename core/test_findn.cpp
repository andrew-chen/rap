// core/test_findn.cpp — Tests for GoalTag::FindN (findn primitive).
//
// Tests: basic collection, fewer-than-N solutions, N=0, multi-var,
//        outer binding visibility, result unification, re-execution on branches.
//
// Build from project root:
//   clang++ -std=c++20 -O2 -Wall -Wextra -pedantic -Werror \
//           -o test_findn core/test_findn.cpp

#include "sexp_parser.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ============================================================================
// Test infrastructure
// ============================================================================
static int passed = 0;
static int failed = 0;

#define EXPECT(cond, msg) \
  do { if (cond) { ++passed; std::printf("PASS: %s\n", msg); } \
       else { ++failed; std::printf("FAIL: %s\n", msg); } } while (0)

// term_str: render a Term to a string via fprint_term to a buffer.
static std::string term_str(Term t) {
    char buf[4096];
    std::FILE* f = ::fmemopen(buf, sizeof(buf) - 1, "w");
    if (!f) return "<fmemopen-fail>";
    fprint_term(f, t);
    ::fflush(f);
    long pos = ::ftell(f);
    ::fclose(f);
    if (pos < 0) pos = 0;
    buf[pos] = '\0';
    return std::string(buf, (size_t)pos);
}

// run_query_terms: run src, return all answer Terms.
static std::vector<Term> run_query_terms(const char* src, int n_limit = 20) {
    alignas(64) static std::uint8_t mem[1024 * 1024];
    Arena a(mem, sizeof(mem));
    ParsedQuery pq = parse_query(a, src);
    if (!pq.goal) return {};

    Evaluator eval(&a, &a, &pq.intern, &pq.outcome_syms);
    int run_n = pq.n < n_limit ? pq.n : n_limit;

    std::vector<Term> results;
    eval.runN(run_n, pq.goal, pq.qvar, pq.vars_used, pq.rel_env,
              [&](Term ans, State) { results.push_back(ans); });
    return results;
}

// Pair-list helpers.
static bool is_nil(Term t) { return t.tag == TermTag::Nil; }
static bool is_pair(Term t) { return t.tag == TermTag::Pair && t.pair; }
static Term car(Term t) { return t.pair->car; }
static Term cdr(Term t) { return t.pair->cdr; }

// Count elements in a Pair list.
static int list_len(Term t) {
    int n = 0;
    while (is_pair(t)) { t = cdr(t); ++n; }
    return n;
}

// Get the i-th element (0-indexed) of a Pair list.
static Term list_nth(Term t, int i) {
    while (i-- > 0 && is_pair(t)) t = cdr(t);
    return is_pair(t) ? car(t) : Term::nil();
}

// ============================================================================
// Tests
// ============================================================================

// T1: Basic — collect 3 answers from (membero q '(a b c d e)).
// Expected: q = (a b c), i.e. the first 3 members in order.
static void test_basic_collect() {
    std::printf("\n--- T1: basic collect ---\n");
    // findn 3 (q) (membero q (a b c d e)) result
    // result should be (a b c)
    auto terms = run_query_terms(
        "(defrel (membero x ls)"
        "  (disj"
        "    (fresh (t) (== ls (x . t)))"
        "    (fresh (h t) (== ls (h . t)) (membero x t))))"
        "(run 1 (result)"
        "  (findn 3 (q) (membero q (a b c d e)) result))");

    EXPECT(terms.size() == 1, "T1: one outer solution");
    if (terms.empty()) return;

    Term lst = terms[0];
    EXPECT(is_pair(lst), "T1: result is a pair");
    EXPECT(list_len(lst) == 3, "T1: list has 3 elements");
    EXPECT(term_str(list_nth(lst, 0)) == "a", "T1: first element is a");
    EXPECT(term_str(list_nth(lst, 1)) == "b", "T1: second element is b");
    EXPECT(term_str(list_nth(lst, 2)) == "c", "T1: third element is c");
}

// T2: Fewer solutions than N — ask for 10, only 3 exist.
// Expected: result has exactly 3 elements.
static void test_fewer_than_n() {
    std::printf("\n--- T2: fewer solutions than N ---\n");
    auto terms = run_query_terms(
        "(defrel (membero x ls)"
        "  (disj"
        "    (fresh (t) (== ls (x . t)))"
        "    (fresh (h t) (== ls (h . t)) (membero x t))))"
        "(run 1 (result)"
        "  (findn 10 (q) (membero q (a b c)) result))");

    EXPECT(terms.size() == 1, "T2: one outer solution");
    if (terms.empty()) return;

    Term lst = terms[0];
    EXPECT(list_len(lst) == 3, "T2: list has exactly 3 elements (all solutions)");
    EXPECT(term_str(list_nth(lst, 0)) == "a", "T2: first element is a");
    EXPECT(term_str(list_nth(lst, 1)) == "b", "T2: second element is b");
    EXPECT(term_str(list_nth(lst, 2)) == "c", "T2: third element is c");
}

// T3: N=0 — collect zero answers; result must be nil.
static void test_n_zero() {
    std::printf("\n--- T3: N=0 ---\n");
    auto terms = run_query_terms(
        "(defrel (membero x ls)"
        "  (disj"
        "    (fresh (t) (== ls (x . t)))"
        "    (fresh (h t) (== ls (h . t)) (membero x t))))"
        "(run 1 (result)"
        "  (findn 0 (q) (membero q (a b c)) result))");

    EXPECT(terms.size() == 1, "T3: one outer solution");
    if (terms.empty()) return;
    EXPECT(is_nil(terms[0]), "T3: result is nil for N=0");
}

// T4: Inner goal has no solutions — result is nil regardless of N.
static void test_no_solutions() {
    std::printf("\n--- T4: inner goal unsatisfiable ---\n");
    // (== 1 2) always fails
    auto terms = run_query_terms(
        "(run 1 (result)"
        "  (findn 5 (q) (== 1 2) result))");

    EXPECT(terms.size() == 1, "T4: one outer solution (findn itself succeeds)");
    if (terms.empty()) return;
    EXPECT(is_nil(terms[0]), "T4: result is nil when inner goal has no solutions");
}

// T5: Multi-var — (findn 2 (x y) ...) — each element should be a pair.
// Use (disj (conj (== x 1) (== y a)) (conj (== x 2) (== y b))).
static void test_multi_var() {
    std::printf("\n--- T5: multi-var (x y) ---\n");
    auto terms = run_query_terms(
        "(run 1 (result)"
        "  (findn 2 (x y)"
        "    (disj"
        "      (conj (== x 1) (== y a))"
        "      (conj (== x 2) (== y b)))"
        "    result))");

    EXPECT(terms.size() == 1, "T5: one outer solution");
    if (terms.empty()) return;

    Term lst = terms[0];
    EXPECT(list_len(lst) == 2, "T5: two answer tuples collected");

    Term t0 = list_nth(lst, 0);  // should be (1 a)
    Term t1 = list_nth(lst, 1);  // should be (2 b)

    EXPECT(is_pair(t0), "T5: first tuple is a pair");
    EXPECT(is_pair(t1), "T5: second tuple is a pair");

    EXPECT(term_str(list_nth(t0, 0)) == "1", "T5: tuple0 x=1");
    EXPECT(term_str(list_nth(t0, 1)) == "a", "T5: tuple0 y=a");
    EXPECT(term_str(list_nth(t1, 0)) == "2", "T5: tuple1 x=2");
    EXPECT(term_str(list_nth(t1, 1)) == "b", "T5: tuple1 y=b");
}

// T6: Outer binding is visible inside findn.
// Outer (== outer-var foo) fires before findn; inner goal uses outer-var.
static void test_outer_binding_visible() {
    std::printf("\n--- T6: outer binding visible inside ---\n");
    auto terms = run_query_terms(
        "(run 1 (result)"
        "  (fresh (val)"
        "    (== val foo)"
        "    (findn 1 (q) (== q val) result)))");

    EXPECT(terms.size() == 1, "T6: one outer solution");
    if (terms.empty()) return;

    Term lst = terms[0];
    EXPECT(list_len(lst) == 1, "T6: one answer collected");
    EXPECT(term_str(list_nth(lst, 0)) == "foo", "T6: inner q sees outer binding val=foo");
}

// T7: findn inside disj — two outer branches, each re-runs inner query.
// Left branch: outer x=1, collects (a b).
// Right branch: outer x=2, collects (c d).
// Outer q should be (1 . (a b)) on left and (2 . (c d)) on right.
static void test_rerun_on_branches() {
    std::printf("\n--- T7: re-execution on outer branches ---\n");
    auto terms = run_query_terms(
        "(defrel (membero x ls)"
        "  (disj"
        "    (fresh (t) (== ls (x . t)))"
        "    (fresh (h t) (== ls (h . t)) (membero x t))))"
        "(run 2 (q)"
        "  (fresh (x items)"
        "    (disj"
        "      (conj (== x 1) (membero items ((a b) (c d)))"
        "            (findn 2 (m) (membero m items) q))"
        "      (conj (== x 2) (membero items ((e f) (g h)))"
        "            (findn 2 (m) (membero m items) q)))))");

    EXPECT(terms.size() >= 2, "T7: at least two outer solutions");
    if (terms.size() < 2) return;

    // Each answer should be a pair list of 2 elements.
    EXPECT(list_len(terms[0]) == 2, "T7: first outer branch gives 2-element list");
    EXPECT(list_len(terms[1]) == 2, "T7: second outer branch gives 2-element list");
}

// T8: Result unification failure — findn still produces exactly one solution
// when result unifies, zero when it cannot.
static void test_result_unification() {
    std::printf("\n--- T8: result unification ---\n");

    // Constrain result to a specific list that matches what findn produces.
    auto match_terms = run_query_terms(
        "(defrel (membero x ls)"
        "  (disj"
        "    (fresh (t) (== ls (x . t)))"
        "    (fresh (h t) (== ls (h . t)) (membero x t))))"
        "(run 1 (ok)"
        "  (findn 2 (q) (membero q (a b c)) (a b))"
        "  (== ok yes))");
    EXPECT(match_terms.size() == 1, "T8: succeeds when result matches collected list");

    auto nomatch_terms = run_query_terms(
        "(defrel (membero x ls)"
        "  (disj"
        "    (fresh (t) (== ls (x . t)))"
        "    (fresh (h t) (== ls (h . t)) (membero x t))))"
        "(run 1 (ok)"
        "  (findn 2 (q) (membero q (a b c)) (x y z))"
        "  (== ok yes))");
    EXPECT(nomatch_terms.size() == 0,
           "T8: fails when result does not match collected list");
}

// T9: Single-element list; N=1 returns a one-element list (not a scalar).
static void test_n_one() {
    std::printf("\n--- T9: N=1 returns a one-element Pair list ---\n");
    auto terms = run_query_terms(
        "(run 1 (result)"
        "  (findn 1 (q) (== q hello) result))");

    EXPECT(terms.size() == 1, "T9: one outer solution");
    if (terms.empty()) return;

    Term lst = terms[0];
    EXPECT(is_pair(lst), "T9: result is a pair");
    EXPECT(list_len(lst) == 1, "T9: result has exactly one element");
    EXPECT(term_str(list_nth(lst, 0)) == "hello", "T9: the element is hello");
    EXPECT(is_nil(cdr(lst)), "T9: list is properly nil-terminated");
}

// T10: Explicit budget — budget of 1 terminates early; only partial list returned.
// membero over (a b c d e) with budget=1 may collect 0 or 1 items, never all 5.
static void test_budget_truncates() {
    std::printf("\n--- T10: explicit budget truncates ---\n");
    // Ask for 5 but set budget=1. Inner BFS fires at most 1 step before stopping.
    // The list length must be < 5 (budget prevents full enumeration).
    auto terms = run_query_terms(
        "(defrel (membero x ls)"
        "  (disj"
        "    (fresh (t) (== ls (x . t)))"
        "    (fresh (h t) (== ls (h . t)) (membero x t))))"
        "(run 1 (result)"
        "  (findn 5 (q) (membero q (a b c d e)) result 1))");

    EXPECT(terms.size() == 1, "T10: outer solution still returned on budget hit");
    if (terms.empty()) return;
    EXPECT(list_len(terms[0]) < 5, "T10: partial list when budget is tiny");
}

// T11: Default budget (no 5th arg) behaves same as explicit 100000 budget
// for small finite queries — all solutions collected when inner BFS finishes fast.
static void test_default_budget_finite() {
    std::printf("\n--- T11: default budget works for finite inner goals ---\n");
    auto terms = run_query_terms(
        "(defrel (membero x ls)"
        "  (disj"
        "    (fresh (t) (== ls (x . t)))"
        "    (fresh (h t) (== ls (h . t)) (membero x t))))"
        "(run 1 (result)"
        "  (findn 3 (q) (membero q (a b c)) result))");

    EXPECT(terms.size() == 1, "T11: one outer solution");
    if (terms.empty()) return;
    EXPECT(list_len(terms[0]) == 3, "T11: all 3 answers collected with default budget");
}

// ============================================================================
// main
// ============================================================================
int main() {
    std::printf("=== findn tests ===\n");

    test_basic_collect();
    test_fewer_than_n();
    test_n_zero();
    test_no_solutions();
    test_multi_var();
    test_outer_binding_visible();
    test_rerun_on_branches();
    test_result_unification();
    test_n_one();
    test_budget_truncates();
    test_default_budget_finite();

    std::printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return (failed > 0) ? 1 : 0;
}
