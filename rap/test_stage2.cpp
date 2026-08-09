// rap/test_stage2.cpp
// Validates subsumeso and subsume-and-pruneo (paper Section 5.2):
// semantic subsumption and the self-directed agenda pruning query that
// uses it to remove redundant explore entries from the live agenda.
// Expected output: PASS: N tests, 0 failures
//
// Build from project root:
//   clang++ -std=c++20 -O2 -Wall -Wextra -pedantic -Werror \
//           -o test_stage2 rap/test_stage2.cpp

#include "loop.hpp"
#include <cstdio>
#include <functional>

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
// Build a minimal inert 3-param RelNode to carry data args in the agenda.
// Body: (== BVar(0) BVar(0)) — trivially succeeds, produces no ops.
// param_count=3 satisfies the (agenda args ops) calling convention.
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
    rn->param_count = 3;
    rn->body        = body;

    return Term::relation(rn);
}

// ============================================================================
// subsumeso / subsume-and-pruneo definitions.
// Ported verbatim from examples/memory/component_tests/test_subsume_agenda.rap (verified correct).
//
// subsumeso: strong subsumes weak iff both explore the same hypothesis H
// and strong's depth dS >= weak's depth dW  (equivalently, leqo dW dS).
//
// subsume-and-pruneo: self-directed 3-param agenda query. Receives the live
// agenda at dequeue time, discovers all subsumed explore entries via
// relational scan, emits Remove ops for each, and outputs which qids were
// pruned.  The middle "args" parameter is accepted but unused.
// ============================================================================
static const char* SUBSUME_DEFS =

    "(defrel (subsumeso strong weak)"
    "  (fresh (qidS relS qidW relW H dS dW)"
    "    (== strong (qidS relS (explore H dS)))"
    "    (== weak (qidW relW (explore H dW)))"
    "    (leqo dW dS)))"

    "(defrel (is-explore-entryo entry)"
    "  (fresh (qid rel-term H D)"
    "    (== entry (qid rel-term (explore H D)))))"

    // has-subsuming-entryo: succeeds iff some OTHER entry in agenda subsumes candidate.
    // (=/= qidE qidC) ensures an entry is not counted as subsuming itself.
    "(defrel (has-subsuming-entryo agenda candidate)"
    "  (fresh (entry rest qidC relC argsC qidE)"
    "    (== candidate (qidC relC argsC))"
    "    (== agenda (entry . rest))"
    "    (disj"
    "      (fresh (relE argsE)"
    "        (conj"
    "          (== entry (qidE relE argsE))"
    "          (=/= qidE qidC)"
    "          (subsumeso entry candidate)))"
    "      (has-subsuming-entryo rest candidate))))"

    // is-subsumedo: converts has-subsuming-entryo into explicit yes/no via Probe
    // (avoids non-determinism — existence check, not structural match).
    "(defrel (is-subsumedo agenda candidate result)"
    "  (disj"
    "    (conj"
    "      (probe (has-subsuming-entryo agenda candidate) true 1000 true false)"
    "      (== result yes))"
    "    (conj"
    "      (probe (has-subsuming-entryo agenda candidate) false 1000 true false)"
    "      (== result no))))"

    // is-explore-entryo-reporto: yes/no wrapper around is-explore-entryo via Probe.
    // Avoids the groundo bug: testing shape via (=/= entry (qid relE argsE)) with
    // fresh vars would trivially succeed via deferred constraints regardless of shape.
    "(defrel (is-explore-entryo-reporto entry result)"
    "  (disj"
    "    (conj (probe (is-explore-entryo entry) true 1000 true false)"
    "          (== result yes))"
    "    (conj (probe (is-explore-entryo entry) false 1000 true false)"
    "          (== result no))))"

    "(defrel (find-all-subsumed-qidso agenda full-agenda qids)"
    "  (disj"
    "    (conj (== agenda ()) (== qids ()))"
    "    (fresh (entry rest qid relE argsE tail result is-explore)"
    "      (conj"
    "        (== agenda (entry . rest))"
    "        (is-explore-entryo-reporto entry is-explore)"
    "        (disj"
    "          (conj"
    "            (== is-explore yes)"
    "            (== entry (qid relE argsE))"
    "            (is-subsumedo full-agenda entry result)"
    "            (disj"
    "              (conj"
    "                (== result yes)"
    "                (find-all-subsumed-qidso rest full-agenda tail)"
    "                (== qids (qid . tail)))"
    "              (conj"
    "                (== result no)"
    "                (find-all-subsumed-qidso rest full-agenda qids))))"
    "          (conj"
    "            (== is-explore no)"
    "            (find-all-subsumed-qidso rest full-agenda qids)))))))"

    "(defrel (qids->remove-opso qids ops)"
    "  (disj"
    "    (conj (== qids ()) (no-ops ops))"
    "    (fresh (qid rest ops-tail)"
    "      (conj"
    "        (== qids (qid . rest))"
    "        (qids->remove-opso rest ops-tail)"
    "        (cons-ops (remove qid) ops-tail ops)))))"

    "(defrel (subsume-and-pruneo agenda args ops)"
    "  (fresh (subsumed-qids ops0)"
    "    (conj"
    "      (find-all-subsumed-qidso agenda agenda subsumed-qids)"
    "      (qids->remove-opso subsumed-qids ops0)"
    "      (cons-ops (output (subsume-and-pruneo-ran subsumed-qids)) ops0 ops))))";

int main() {

    RapLoop loop;
    loop.quiet = true;  // suppress "[output] ..." to keep PASS/FAIL lines readable
    EXPECT(loop.init(), "RapLoop initializes");
    if (failed) { std::printf("\nAborting: init failed.\n"); return 1; }

    // -------------------------------------------------------------------------
    // Load subsumeso, subsume-and-pruneo, and all helper relation definitions.
    // -------------------------------------------------------------------------
    EXPECT(loop.load_defs(SUBSUME_DEFS), "Relation definitions load");
    if (failed) { std::printf("\nAborting: load_defs failed.\n"); return 1; }

    // -------------------------------------------------------------------------
    // Set up the test agenda.
    //
    // Enqueue subsume-and-pruneo FIRST (it will be at the front of the FIFO
    // queue), then set next_id=10 and enqueue 4 data entries.
    //
    // When run_one() dequeues subsume-and-pruneo (FIFO front), the remaining
    // agenda it receives is:
    //   [id=10: minimal-rel args=(explore hypA 1),   ← weak: subsumed by 12
    //    id=11: minimal-rel args=(explore hypA 2),   ← weak: subsumed by 12
    //    id=12: minimal-rel args=(explore hypA 3),   ← strong: survives
    //    id=13: minimal-rel args=(explore hypB 5)]   ← different hyp: survives
    //
    // subsume-and-pruneo discovers entries 10 and 11 are each subsumed by
    // entry 12 (same hypothesis, greater depth) and emits:
    //   Remove(10), Remove(11), Output((subsume-and-pruneo-ran (10 11)))
    // Entries 12 and 13 survive (count=2 after apply_changeset).
    // -------------------------------------------------------------------------

    std::uint32_t sa_id = loop.enqueue_query("subsume-and-pruneo");
    EXPECT(sa_id != 0u, "subsume-and-pruneo enqueued");
    if (failed) { std::printf("\nAborting: enqueue_query failed.\n"); return 1; }

    loop.agenda.next_id = 10;

    alignas(64) std::uint8_t term_buf[4 * 1024];
    Arena tmp(term_buf, sizeof(term_buf));

    Term content10 = parse_term(loop, tmp, "(explore hypA 1)");
    Term content11 = parse_term(loop, tmp, "(explore hypA 2)");
    Term content12 = parse_term(loop, tmp, "(explore hypA 3)");
    Term content13 = parse_term(loop, tmp, "(explore hypB 5)");

    EXPECT(content10.tag == TermTag::Pair, "content10 parsed");
    EXPECT(content11.tag == TermTag::Pair, "content11 parsed");
    EXPECT(content12.tag == TermTag::Pair, "content12 parsed");
    EXPECT(content13.tag == TermTag::Pair, "content13 parsed");

    // Build an inert 3-param Rel to carry each explore content term.
    Term minimal_rel = make_minimal_rel(loop.intern_arena);

    std::uint32_t id10 = loop.agenda.enqueue(minimal_rel, content10);
    std::uint32_t id11 = loop.agenda.enqueue(minimal_rel, content11);
    std::uint32_t id12 = loop.agenda.enqueue(minimal_rel, content12);
    std::uint32_t id13 = loop.agenda.enqueue(minimal_rel, content13);

    EXPECT(id10 == 10u, "item10 gets id=10");
    EXPECT(id11 == 11u, "item11 gets id=11");
    EXPECT(id12 == 12u, "item12 gets id=12");
    EXPECT(id13 == 13u, "item13 gets id=13");

    // Agenda: [sa, entry10, entry11, entry12, entry13] — all 5 entries.
    EXPECT(loop.agenda.count == 5u, "Agenda has 5 entries before run");

    // -------------------------------------------------------------------------
    // Run subsume-and-pruneo (FIFO dequeue — at the front of the queue).
    // After execution, expect:
    //   - ChangeSet: Remove(10), Remove(11), Output((subsume-and-pruneo-ran (10 11)))
    //   - Agenda after apply: entry12 and entry13 remain (count=2)
    //   - OutputQueue: 1 term
    // -------------------------------------------------------------------------
    loop.run_one();

    EXPECT(loop.output.count == 1u, "One output term produced");
    EXPECT(loop.agenda.count == 2u, "Two entries remain in agenda");

    // Verify the two remaining entries are id=12 and id=13.
    if (loop.agenda.count == 2) {
        const auto* e0 = reinterpret_cast<const QueryEntry*>(
            loop.agenda.buf + loop.agenda.tail);
        const auto* e1 = reinterpret_cast<const QueryEntry*>(
            loop.agenda.buf + loop.agenda.tail + e0->byte_size);
        EXPECT(e0->id == 12u, "Remaining entry 0 is id=12");
        EXPECT(e1->id == 13u, "Remaining entry 1 is id=13");
    }

    // Verify the output term is (subsume-and-pruneo-ran (10 11)).
    if (loop.output.count >= 1) {
        Term out = loop.output.terms[0];
        std::printf("Output term: ");
        print_term(out);
        std::printf("\n");
        EXPECT(out.tag == TermTag::Pair, "Output is a pair");
        if (out.tag == TermTag::Pair && out.pair) {
            Term head = out.pair->car;
            EXPECT(head.tag == TermTag::Sym &&
                   sym_lit_eq(head.sym, "subsume-and-pruneo-ran"),
                   "Output head is 'subsume-and-pruneo-ran'");
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

        loop2.quiet = true;
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

        loop3.quiet = true;
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

    // =========================================================================
    // Test C: agenda Var-scan regression — after remove() compaction, no
    // live entry's args should contain a bare Var node.
    //
    // This directly tests the deep_copy_term overlap fix: before the fix,
    // a Var(2) appeared in an entry's args after remove() compacted it into
    // a smaller gap.  The test runs subsume-and-pruneo (which calls remove
    // twice: Remove(10) and Remove(11)) and verifies the two surviving entries
    // (id=12, id=13) have no Var nodes in their args.
    //
    // The lambda mimics what --trace's scan_agenda_for_vars does.
    // =========================================================================
    {
        // Re-use the loop from the main test above: after loop.run_one(),
        // loop.agenda holds entries id=12 and id=13.
        bool found_var = false;

        std::function<bool(Term)> has_var = [&](Term t) -> bool {
            if (t.tag == TermTag::Var) return true;
            if (t.tag == TermTag::Pair && t.pair)
                return has_var(t.pair->car) || has_var(t.pair->cdr);
            return false;
        };

        std::uint32_t pos = loop.agenda.tail;
        for (std::uint32_t i = 0; i < loop.agenda.count; ++i) {
            const QueryEntry* e =
                reinterpret_cast<const QueryEntry*>(loop.agenda.buf + pos);
            if (has_var(e->args)) {
                found_var = true;
                std::printf("FAIL (Test C): Var found in entry id=%u args\n", e->id);
            }
            pos += e->byte_size;
        }

        EXPECT(!found_var,
               "Test C: no stray Var nodes in agenda entries after remove compaction");
    }

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
