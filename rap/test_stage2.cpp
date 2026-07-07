// rap/test_stage2.cpp
// Validates the strengthen-agendao case study (paper Section 5.2).
// Expected output: PASS: N tests, 0 failures
//
// Build from project root:
//   clang++ -std=c++20 -O2 -Wall -Wextra -pedantic -Werror \
//           -o test_stage2 rap/test_stage2.cpp

#include "loop.hpp"
#include <cstdio>

static int passed = 0;
static int failed = 0;

#define EXPECT(cond, msg) \
    do { if (cond) { ++passed; std::printf("PASS: %s\n", msg); } \
         else { ++failed; std::printf("FAIL: %s\n", msg); } } while(0)

// ============================================================================
// Build a term from an s-expression string, using the RapLoop's intern table.
// ============================================================================
static Term parse_term(RapLoop& loop, Arena& tmp, const char* str) {
    Lexer lx{str};
    Token tok = lx.next();
    const Sexp* sx = parse_sexp(tmp, loop.intern_arena, loop.intern, lx, tok);
    if (!sx) return Term::nil();
    return compile_term(tmp, nullptr, nullptr, sx);
}

// ============================================================================
// Build a minimal 1-param RelNode with trivial body (== BVar(0) BVar(0)).
// Used as the rel_term for state-holding agenda entries; content is passed
// separately as the published args to agenda.enqueue().
// ============================================================================
static Term make_minimal_rel(Arena& stable) {
    Term bv0; bv0.tag = TermTag::BVar; bv0.id = 0;
    Goal* body = stable.make<Goal>();
    if (!body) return Term::nil();
    body->tag  = GoalTag::Eq;
    body->eq.u = bv0;
    body->eq.v = bv0;

    RelNode* rn = stable.make<RelNode>();
    if (!rn) return Term::nil();
    rn->param_count = 1;
    rn->body        = body;

    return Term::relation(rn);
}

int main() {

    RapLoop loop;
    EXPECT(loop.init(), "RapLoop initializes");
    if (failed) { std::printf("\nAborting: init failed.\n"); return 1; }

    // -------------------------------------------------------------------------
    // Load the strengthen-agendao relation definitions.
    // Agenda entries are now 3-element lists: (id rel-term args).
    // find-by-contento matches on entry-args directly.
    // weak-check-qido / not-weak-check-qido destructure (qid rel-term entry-args).
    // -------------------------------------------------------------------------
    const char* defs =
        // weak-check-qido: entry is (qid rel-term entry-args), args = (check H T)
        "(defrel (weak-check-qido entry H T qid)"
        "  (fresh (rel-term entry-args)"
        "    (== entry (qid rel-term entry-args))"
        "    (== entry-args (check H T))))"

        // not-weak-check-qido: entry whose args != (check H T)
        "(defrel (not-weak-check-qido entry H T)"
        "  (fresh (qid rel-term entry-args)"
        "    (== entry (qid rel-term entry-args))"
        "    (=/= entry-args (check H T))))"

        "(defrel (collect-weak-qidso agenda H T qids)"
        "  (disj"
        "    (conj (== agenda ()) (== qids ()))"
        "    (fresh (entry rest qid tail)"
        "      (conj"
        "        (== agenda (entry . rest))"
        "        (disj"
        "          (conj"
        "            (weak-check-qido entry H T qid)"
        "            (collect-weak-qidso rest H T tail)"
        "            (== qids (qid . tail)))"
        "          (conj"
        "            (not-weak-check-qido entry H T)"
        "            (collect-weak-qidso rest H T qids)))))))"

        "(defrel (qids->remove-opso qids ops)"
        "  (disj"
        "    (conj (== qids ()) (call no-ops ops))"
        "    (fresh (qid rest ops-tail)"
        "      (conj"
        "        (== qids (qid . rest))"
        "        (call qids->remove-opso rest ops-tail)"
        "        (call cons-ops (remove qid) ops-tail ops)))))"

        // find-by-contento: matches entry (id rel-term args) where args = pattern
        "(defrel (find-by-contento agenda pattern id)"
        "  (fresh (entry rest entry-id entry-rel entry-args)"
        "    (== agenda (entry . rest))"
        "    (disj"
        "      (conj"
        "        (== entry (entry-id entry-rel entry-args))"
        "        (== entry-args pattern)"
        "        (== id entry-id))"
        "      (find-by-contento rest pattern id))))"

        // strengthen-agendao: find the strong check+ entry via find-by-contento
        "(defrel (strengthen-agendao agenda ops)"
        "  (fresh (H T R strong-qid weak-qids ops0)"
        "    (conj"
        "      (find-by-contento agenda (check+ H T R) strong-qid)"
        "      (collect-weak-qidso agenda H T weak-qids)"
        "      (call qids->remove-opso weak-qids ops0)"
        "      (call cons-ops (output (pruned H T)) ops0 ops))))";

    EXPECT(loop.load_defs(defs), "Relation definitions load");
    if (failed) { std::printf("\nAborting: load_defs failed.\n"); return 1; }

    // -------------------------------------------------------------------------
    // Set up the test agenda.
    //
    // Enqueue strengthen-agendao FIRST (it will be at the front of the queue),
    // then set next_id=10 and enqueue the 4 state-holder entries. When run_one()
    // dequeues strengthen-agendao, the remaining agenda is:
    //   [id=10: (10 minimal-rel (check  hypA test1)),
    //    id=11: (11 minimal-rel (check+ hypA test1 refineX)),
    //    id=12: (12 minimal-rel (check  hypA test1)),
    //    id=13: (13 minimal-rel (explore hypB 2))]
    // strengthen-agendao finds id=11 as the strong check+ and emits
    // Remove(10), Remove(12), Output((pruned hypA test1)).
    // -------------------------------------------------------------------------

    // Enqueue strengthen-agendao first (2-param, nil args → runnable).
    std::uint32_t sa_id = loop.enqueue_query("strengthen-agendao");
    EXPECT(sa_id != 0u, "strengthen-agendao enqueued");
    if (failed) { std::printf("\nAborting: enqueue_query failed.\n"); return 1; }

    // Force the 4 state-holder entries to get ids 10-13.
    loop.agenda.next_id = 10;

    // Build content terms using our intern table so symbol pointers match.
    alignas(64) std::uint8_t term_buf[4 * 1024];
    Arena tmp(term_buf, sizeof(term_buf));

    Term content10 = parse_term(loop, tmp, "(check  hypA test1)");
    Term content11 = parse_term(loop, tmp, "(check+ hypA test1 refineX)");
    Term content12 = parse_term(loop, tmp, "(check  hypA test1)");
    Term content13 = parse_term(loop, tmp, "(explore hypB 2)");

    EXPECT(content10.tag == TermTag::Pair, "content10 parsed");
    EXPECT(content11.tag == TermTag::Pair, "content11 parsed");
    EXPECT(content12.tag == TermTag::Pair, "content12 parsed");
    EXPECT(content13.tag == TermTag::Pair, "content13 parsed");

    // Build a minimal Rel (1-param, trivial body) to use as the rel_term for
    // all state-holder entries. Content is published separately as args.
    Term minimal_rel = make_minimal_rel(loop.intern_arena);

    // Enqueue state-holders: args = content, non-nil → never dequeued as runnable.
    std::uint32_t id10 = loop.agenda.enqueue(minimal_rel, content10);
    std::uint32_t id11 = loop.agenda.enqueue(minimal_rel, content11);
    std::uint32_t id12 = loop.agenda.enqueue(minimal_rel, content12);
    std::uint32_t id13 = loop.agenda.enqueue(minimal_rel, content13);

    EXPECT(id10 == 10u, "item10 gets id=10");
    EXPECT(id11 == 11u, "item11 gets id=11");
    EXPECT(id12 == 12u, "item12 gets id=12");
    EXPECT(id13 == 13u, "item13 gets id=13");

    // Agenda now: [sa (runnable), entry10-13 (state-holders)]
    EXPECT(loop.agenda.count == 5u, "Agenda has 5 entries before run");

    // -------------------------------------------------------------------------
    // Run strengthen-agendao (dequeued from front as the only runnable entry).
    // After execution, expect:
    //   - ChangeSet: Remove(10), Remove(12), Output((pruned hypA test1))
    //   - Agenda after apply: entry11 and entry13 remain (count=2)
    //   - OutputQueue: 1 term
    // -------------------------------------------------------------------------
    loop.run_one();

    EXPECT(loop.output.count == 1u, "One output term produced");
    EXPECT(loop.agenda.count == 2u, "Two entries remain in agenda");

    // Verify the two remaining entries are id=11 and id=13.
    if (loop.agenda.count == 2) {
        const auto* e0 = reinterpret_cast<const QueryEntry*>(
            loop.agenda.buf + loop.agenda.tail);
        const auto* e1 = reinterpret_cast<const QueryEntry*>(
            loop.agenda.buf + loop.agenda.tail + e0->byte_size);
        EXPECT(e0->id == 11u, "Remaining entry 0 is id=11");
        EXPECT(e1->id == 13u, "Remaining entry 1 is id=13");
    }

    // Verify the output term is (pruned hypA test1).
    if (loop.output.count >= 1) {
        Term out = loop.output.terms[0];
        std::printf("Output term: ");
        print_term(out);
        std::printf("\n");
        // Check structure: car = sym(pruned)
        EXPECT(out.tag == TermTag::Pair, "Output is a pair");
        if (out.tag == TermTag::Pair && out.pair) {
            Term head = out.pair->car;
            EXPECT(head.tag == TermTag::Sym && sym_lit_eq(head.sym, "pruned"),
                   "Output head is 'pruned'");
        }
    }

    // =========================================================================
    // Test A: stale ops from a failed branch must not appear in the ChangeSet.
    //
    // The first branch pushes Output(stale) then fails (== 0 1 is false).
    // The second branch pushes Output(correct) and succeeds.
    // Without the fix: op_count=2, apply_changeset produces 2 output terms.
    // With the fix:    op_count=1, apply_changeset produces 1 output term.
    // =========================================================================
    {
        RapLoop loop2;
        EXPECT(loop2.init(), "Test A: RapLoop initializes");

        const char* defs_a =
            "(defrel (backtrack-test agenda ops)"
            "  (disj"
            "    (conj"
            "      (== 0 1)"
            "      (fresh (c) (conj (call no-ops c) (call cons-ops (output stale) c ops))))"
            "    (fresh (c) (conj (call no-ops c) (call cons-ops (output correct) c ops)))))";

        EXPECT(loop2.load_defs(defs_a), "Test A: definitions load");

        std::uint32_t qid = loop2.enqueue_query("backtrack-test");
        EXPECT(qid != 0u, "Test A: backtrack-test enqueued");

        loop2.run_one();

        EXPECT(loop2.output.count == 1u,
               "Test A: exactly one output (stale op from failed branch excluded)");

        if (loop2.output.count >= 1) {
            Term out = loop2.output.terms[0];
            bool is_correct = (out.tag == TermTag::Sym &&
                               sym_lit_eq(out.sym, "correct"));
            EXPECT(is_correct, "Test A: output term is 'correct', not 'stale'");
        }
    }

    // =========================================================================
    // Test B: ops pushed inside a sandboxed Probe must not appear in the outer
    //         ChangeSet.
    //
    // The probe sub-goal calls cons-ops (pushing Output(probe-output)).
    // sandbox=true: the probe's substitution is discarded.
    // Without the fix: both probe-output and outer-output are applied (count=2).
    // With the fix:    only outer-output is applied (count=1).
    // =========================================================================
    {
        RapLoop loop3;
        EXPECT(loop3.init(), "Test B: RapLoop initializes");

        const char* defs_b =
            "(defrel (probe-test agenda outer-ops)"
            "  (conj"
            "    (probe"
            "      (fresh (io c2)"
            "        (conj (call no-ops c2) (call cons-ops (output probe-output) c2 io)))"
            "      true 100 true false)"
            "    (fresh (c) (conj (call no-ops c) (call cons-ops (output outer-output) c outer-ops)))))";

        EXPECT(loop3.load_defs(defs_b), "Test B: definitions load");

        std::uint32_t qid3 = loop3.enqueue_query("probe-test");
        EXPECT(qid3 != 0u, "Test B: probe-test enqueued");

        loop3.run_one();

        EXPECT(loop3.output.count == 1u,
               "Test B: exactly one output (sandboxed probe op excluded)");

        if (loop3.output.count >= 1) {
            Term out = loop3.output.terms[0];
            bool is_outer = (out.tag == TermTag::Sym &&
                             sym_lit_eq(out.sym, "outer-output"));
            EXPECT(is_outer, "Test B: output term is 'outer-output', not 'probe-output'");
        }
    }

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
