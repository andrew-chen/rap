// rap/test_loop_additions.cpp
// Regression tests for the loop.hpp additions:
//   - call_main()        — bootstraps the agenda from a 'main' relation
//   - build_args_term()  — builds the args list term from string values
//   - quiet flag         — suppresses "[output] ..." stdout while still
//                          collecting terms in output.terms[]
//
// Build from project root:
//   clang++ -std=c++20 -O2 -Wall -Wextra -pedantic -Werror \
//           -o test_loop_additions rap/test_loop_additions.cpp
//
// Expected: "N passed, 0 failed"

#include "loop.hpp"
#include <cstdio>
#include <cstring>

static int passed = 0;
static int failed = 0;

#define EXPECT(cond, msg) \
    do { if (cond) { ++passed; std::printf("PASS: %s\n", msg); } \
         else { ++failed; std::printf("FAIL: %s\n", msg); } } while(0)

// ============================================================================
// Test 1: call_main() seeds the agenda from a 'main' relation.
// main produces one Output op; after call_main() output.count should be 1.
// quiet=true so the "[output]" line does not interleave with PASS/FAIL.
// ============================================================================
static void test_call_main_basic() {
    RapLoop loop;
    loop.quiet = true;
    EXPECT(loop.init(), "T1: RapLoop initializes");

    const char* defs =
        "(defrel (main args ops)"
        "  (cons-ops (output (main-ran)) (no-ops) ops))"
        "(defrel (handle_input agenda fd input ops)"
        "  (call no-ops ops))";
    EXPECT(loop.load_defs(defs), "T1: defs load");

    bool ok = loop.call_main();
    EXPECT(ok, "T1: call_main succeeds");
    EXPECT(loop.output.count == 1u, "T1: one output term produced");

    if (loop.output.count >= 1) {
        Term t = loop.output.terms[0];
        EXPECT(t.tag == TermTag::Pair && t.pair != nullptr, "T1: output is a pair");
        if (t.tag == TermTag::Pair && t.pair) {
            Term head = t.pair->car;
            EXPECT(head.tag == TermTag::Sym &&
                   sym_lit_eq(head.sym, "main-ran"),
                   "T1: head is 'main-ran'");
            EXPECT(t.pair->cdr.tag == TermTag::Nil,
                   "T1: tail is nil — (main-ran) is a single-element list");
        }
    }
}

// ============================================================================
// Test 2: call_main() with args via build_args_term().
// Program destructures its single arg and echoes it back in the output.
// ============================================================================
static void test_call_main_with_args() {
    RapLoop loop;
    loop.quiet = true;
    EXPECT(loop.init(), "T2: RapLoop initializes");

    const char* defs =
        "(defrel (main args ops)"
        "  (fresh (a)"
        "    (conj"
        "      (== args (a))"
        "      (cons-ops (output (got-arg a)) (no-ops) ops))))"
        "(defrel (handle_input agenda fd input ops)"
        "  (call no-ops ops))";
    EXPECT(loop.load_defs(defs), "T2: defs load");

    Term args = loop.build_args_term({"hello"});
    EXPECT(args.tag == TermTag::Pair, "T2: build_args_term produces a pair");

    bool ok = loop.call_main(args);
    EXPECT(ok, "T2: call_main with args succeeds");
    EXPECT(loop.output.count == 1u, "T2: one output term");

    if (loop.output.count >= 1) {
        Term t = loop.output.terms[0];
        // Expected: (got-arg hello)
        EXPECT(t.tag == TermTag::Pair && t.pair, "T2: output is a pair");
        if (t.tag == TermTag::Pair && t.pair) {
            Term head = t.pair->car;
            EXPECT(head.tag == TermTag::Sym &&
                   sym_lit_eq(head.sym, "got-arg"),
                   "T2: head is 'got-arg'");
            Term rest = t.pair->cdr;
            EXPECT(rest.tag == TermTag::Pair && rest.pair, "T2: rest is a pair");
            if (rest.tag == TermTag::Pair && rest.pair) {
                Term arg = rest.pair->car;
                EXPECT(arg.tag == TermTag::Sym &&
                       sym_lit_eq(arg.sym, "hello"),
                       "T2: arg value is 'hello'");
                EXPECT(rest.pair->cdr.tag == TermTag::Nil,
                       "T2: list ends after the arg");
            }
        }
    }
}

// ============================================================================
// Test 3: build_args_term() produces (foo bar) from {"foo", "bar"}.
// ============================================================================
static void test_build_args_term_multi() {
    RapLoop loop;
    EXPECT(loop.init(), "T3: RapLoop initializes");

    Term args = loop.build_args_term({"foo", "bar"});
    EXPECT(args.tag == TermTag::Pair, "T3: result is a pair");

    if (args.tag == TermTag::Pair && args.pair) {
        Term first = args.pair->car;
        EXPECT(first.tag == TermTag::Sym && sym_lit_eq(first.sym, "foo"),
               "T3: first element is 'foo'");
        Term rest = args.pair->cdr;
        EXPECT(rest.tag == TermTag::Pair && rest.pair, "T3: rest is a pair");
        if (rest.tag == TermTag::Pair && rest.pair) {
            Term second = rest.pair->car;
            EXPECT(second.tag == TermTag::Sym && sym_lit_eq(second.sym, "bar"),
                   "T3: second element is 'bar'");
            EXPECT(rest.pair->cdr.tag == TermTag::Nil,
                   "T3: list ends after second element");
        }
    }
}

// ============================================================================
// Test 4: build_args_term() on empty vector produces nil.
// ============================================================================
static void test_build_args_term_empty() {
    RapLoop loop;
    EXPECT(loop.init(), "T4: RapLoop initializes");

    Term args = loop.build_args_term({});
    EXPECT(args.tag == TermTag::Nil, "T4: empty args list is nil");
}

// ============================================================================
// Test 5: quiet=true — terms still collected in output.terms[], quiet=false
// (default) prints to stdout.  We verify the functional side: the output
// queue is populated regardless of the quiet setting.
// ============================================================================
static void test_quiet_flag_still_collects_terms() {
    RapLoop loop;
    loop.quiet = true;
    EXPECT(loop.init(), "T5: RapLoop initializes with quiet=true");

    const char* defs =
        "(defrel (main args ops)"
        "  (cons-ops (output (quiet-test)) (no-ops) ops))"
        "(defrel (handle_input agenda fd input ops)"
        "  (call no-ops ops))";
    EXPECT(loop.load_defs(defs), "T5: defs load");

    bool ok = loop.call_main();
    EXPECT(ok, "T5: call_main succeeds");
    EXPECT(loop.output.count == 1u, "T5: output term captured despite quiet=true");

    if (loop.output.count >= 1) {
        Term t = loop.output.terms[0];
        EXPECT(t.tag == TermTag::Pair && t.pair, "T5: captured term is a pair");
        if (t.tag == TermTag::Pair && t.pair) {
            EXPECT(t.pair->car.tag == TermTag::Sym &&
                   sym_lit_eq(t.pair->car.sym, "quiet-test"),
                   "T5: captured head is 'quiet-test'");
        }
    }
}

// ============================================================================
// Test 6: call_main() returns false when 'main' is not defined.
// ============================================================================
static void test_call_main_no_main() {
    RapLoop loop;
    EXPECT(loop.init(), "T6: RapLoop initializes");
    bool ok = loop.call_main();
    EXPECT(!ok, "T6: call_main returns false when 'main' is undefined");
}

// ============================================================================
// Test 7: call_main() seeds the agenda; run_one() drains it and produces
// output.  main adds a worker entry; the actual output comes from run_one().
// ============================================================================
static void test_call_main_seeds_agenda() {
    RapLoop loop;
    loop.quiet = true;
    EXPECT(loop.init(), "T7: RapLoop initializes");

    const char* defs =
        "(defrel (worker agenda args ops)"
        "  (cons-ops (output (worker-ran)) (no-ops) ops))"
        "(defrel (main args ops)"
        "  (cons-ops (add (rel (a b r) (call worker a b r)) (worker-tag))"
        "            (no-ops) ops))"
        "(defrel (handle_input agenda fd input ops)"
        "  (call no-ops ops))";
    EXPECT(loop.load_defs(defs), "T7: defs load");

    bool ok = loop.call_main();
    EXPECT(ok, "T7: call_main succeeds");
    EXPECT(loop.output.count == 0u, "T7: no output yet — main only seeded the agenda");
    EXPECT(loop.agenda.count == 1u, "T7: one entry in agenda after call_main");

    loop.run_one();
    EXPECT(loop.output.count == 1u, "T7: one output term after run_one()");
    EXPECT(loop.agenda.count == 0u, "T7: agenda empty after run_one()");

    if (loop.output.count >= 1) {
        Term t = loop.output.terms[0];
        EXPECT(t.tag == TermTag::Pair && t.pair &&
               t.pair->car.tag == TermTag::Sym &&
               sym_lit_eq(t.pair->car.sym, "worker-ran"),
               "T7: output is (worker-ran)");
    }
}

// ============================================================================
// Test 8: multiple output terms from a single main call.
// ============================================================================
static void test_call_main_multiple_outputs() {
    RapLoop loop;
    loop.quiet = true;
    EXPECT(loop.init(), "T8: RapLoop initializes");

    const char* defs =
        "(defrel (main args ops)"
        "  (fresh (ops0 ops1)"
        "    (conj"
        "      (cons-ops (output (first))  (no-ops) ops0)"
        "      (cons-ops (output (second)) ops0     ops1)"
        "      (cons-ops (output (third))  ops1     ops))))"
        "(defrel (handle_input agenda fd input ops)"
        "  (call no-ops ops))";
    EXPECT(loop.load_defs(defs), "T8: defs load");

    bool ok = loop.call_main();
    EXPECT(ok, "T8: call_main succeeds");
    EXPECT(loop.output.count == 3u, "T8: three output terms produced");

    auto head_str = [](Term t, const char* s) -> bool {
        return t.tag == TermTag::Pair && t.pair &&
               t.pair->car.tag == TermTag::Sym &&
               sym_lit_eq(t.pair->car.sym, s);
    };

    if (loop.output.count == 3u) {
        EXPECT(head_str(loop.output.terms[0], "first"),  "T8: terms[0] is (first)");
        EXPECT(head_str(loop.output.terms[1], "second"), "T8: terms[1] is (second)");
        EXPECT(head_str(loop.output.terms[2], "third"),  "T8: terms[2] is (third)");
    }
}

// ============================================================================
int main() {
    test_call_main_basic();
    test_call_main_with_args();
    test_build_args_term_multi();
    test_build_args_term_empty();
    test_quiet_flag_still_collects_terms();
    test_call_main_no_main();
    test_call_main_seeds_agenda();
    test_call_main_multiple_outputs();

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
