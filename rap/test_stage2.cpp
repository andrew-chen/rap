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
// All symbols come from loop.intern so they match symbols in loaded defs.
// The resulting PairNodes are allocated in tmp_arena; callers must enqueue
// (which deep-copies into the agenda buffer) before resetting tmp_arena.
// ============================================================================
static Term parse_term(RapLoop& loop, Arena& tmp, const char* str) {
    Lexer lx{str};
    Token tok = lx.next();
    const Sexp* sx = parse_sexp(tmp, loop.intern, lx, tok);
    if (!sx) return Term::nil();
    return compile_term(tmp, nullptr, nullptr, sx);
}

int main() {

    RapLoop loop;
    EXPECT(loop.init(), "RapLoop initializes");
    if (failed) { std::printf("\nAborting: init failed.\n"); return 1; }

    // -------------------------------------------------------------------------
    // Load the strengthen-agendao relation definitions.
    // -------------------------------------------------------------------------
    const char* defs =
        "(defrel (membero x lst)"
        "  (disj"
        "    (fresh (rest) (== lst (x . rest)))"
        "    (fresh (head rest)"
        "      (conj (== lst (head . rest))"
        "            (call membero x rest)))))"

        "(defrel (weak-check-qido item H T qid)"
        "  (== item (q qid (check H T))))"

        "(defrel (collect-weak-qidso agenda H T qids)"
        "  (disj"
        "    (conj (== agenda ()) (== qids ()))"
        "    (fresh (item rest qid tail)"
        "      (conj"
        "        (== agenda (item . rest))"
        "        (disj"
        "          (conj"
        "            (call weak-check-qido item H T qid)"
        "            (call collect-weak-qidso rest H T tail)"
        "            (== qids (qid . tail)))"
        "          (call collect-weak-qidso rest H T qids))))))"

        "(defrel (qids->remove-opso qids ops)"
        "  (disj"
        "    (conj (== qids ()) (call no-ops ops))"
        "    (fresh (qid rest ops-tail)"
        "      (conj"
        "        (== qids (qid . rest))"
        "        (call qids->remove-opso rest ops-tail)"
        "        (call cons-ops (remove qid) ops-tail ops)))))"

        "(defrel (strengthen-agendao agenda ops)"
        "  (fresh (H T R strong-qid weak-qids ops0 strong-item)"
        "    (conj"
        "      (call membero strong-item agenda)"
        "      (== strong-item (q strong-qid (check+ H T R)))"
        "      (call collect-weak-qidso agenda H T weak-qids)"
        "      (call qids->remove-opso weak-qids ops0)"
        "      (call cons-ops (output (pruned H T)) ops0 ops))))";

    EXPECT(loop.load_defs(defs), "Relation definitions load");
    if (failed) { std::printf("\nAborting: load_defs failed.\n"); return 1; }

    // -------------------------------------------------------------------------
    // Set up the test agenda.
    //
    // Enqueue strengthen-agendao FIRST (it will be at the front of the queue),
    // then set next_id=10 and enqueue the 4 data entries.  When run_one()
    // dequeues strengthen-agendao, the remaining agenda is:
    //   [id=10: (q 10 (check  hypA test1)),
    //    id=11: (q 11 (check+ hypA test1 refineX)),
    //    id=12: (q 12 (check  hypA test1)),
    //    id=13: (q 13 (explore hypB 2))]
    // strengthen-agendao will find id=11 as the "strong" check and emit
    // Remove(10), Remove(12), Output((pruned hypA test1)).
    // -------------------------------------------------------------------------

    // Enqueue strengthen-agendao first.
    std::uint32_t sa_id = loop.enqueue_query("strengthen-agendao");
    EXPECT(sa_id != 0u, "strengthen-agendao enqueued");
    if (failed) { std::printf("\nAborting: enqueue_query failed.\n"); return 1; }

    // Force the 4 data entries to get ids 10-13 so that Remove(10) and
    // Remove(12) match the QueryEntry.id values in the agenda.
    loop.agenda.next_id = 10;

    // Build the data terms using our intern table so symbol pointers match
    // the symbols compiled into the loaded relations.
    alignas(64) std::uint8_t term_buf[4 * 1024];
    Arena tmp(term_buf, sizeof(term_buf));

    Term item10 = parse_term(loop, tmp, "(q 10 (check  hypA test1))");
    Term item11 = parse_term(loop, tmp, "(q 11 (check+ hypA test1 refineX))");
    Term item12 = parse_term(loop, tmp, "(q 12 (check  hypA test1))");
    Term item13 = parse_term(loop, tmp, "(q 13 (explore hypB 2))");

    EXPECT(item10.tag == TermTag::Pair, "item10 parsed");
    EXPECT(item11.tag == TermTag::Pair, "item11 parsed");
    EXPECT(item12.tag == TermTag::Pair, "item12 parsed");
    EXPECT(item13.tag == TermTag::Pair, "item13 parsed");

    // Enqueue data items (deep-copied into agenda buffer, tmp_arena may reset).
    std::uint32_t id10 = loop.agenda.enqueue(item10);
    std::uint32_t id11 = loop.agenda.enqueue(item11);
    std::uint32_t id12 = loop.agenda.enqueue(item12);
    std::uint32_t id13 = loop.agenda.enqueue(item13);

    EXPECT(id10 == 10u, "item10 gets id=10");
    EXPECT(id11 == 11u, "item11 gets id=11");
    EXPECT(id12 == 12u, "item12 gets id=12");
    EXPECT(id13 == 13u, "item13 gets id=13");

    // Agenda now: [sa, item10, item11, item12, item13]
    EXPECT(loop.agenda.count == 5u, "Agenda has 5 entries before run");

    // -------------------------------------------------------------------------
    // Run strengthen-agendao (dequeued from front).
    // After execution, expect:
    //   - ChangeSet: Remove(10), Remove(12), Output((pruned hypA test1))
    //   - Agenda after apply: item11 and item13 remain (count=2)
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

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
