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
    const Sexp* sx = parse_sexp(tmp, loop.intern, lx, tok);
    if (!sx) return Term::nil();
    return compile_term(tmp, nullptr, nullptr, sx);
}

// ============================================================================
// Build a carrier RelNode in the permanent arena with one captured value.
// The body is a trivial (== BVar(0) BVar(0)) that always succeeds; the
// carrier is never actually called — only introspected via rel-argso.
// ============================================================================
static Term make_carrier(Arena& stable, Term content) {
    // Build trivial body: (== BVar(0) BVar(0))
    Term bv0; bv0.tag = TermTag::BVar; bv0.id = 0;
    Goal* body = stable.make<Goal>();
    if (!body) return Term::nil();
    body->tag  = GoalTag::Eq;
    body->eq.u = bv0;
    body->eq.v = bv0;

    // Build captured_values array with one deep-copied element.
    Term* caps = static_cast<Term*>(stable.alloc(sizeof(Term), alignof(Term)));
    if (!caps) return Term::nil();
    caps[0] = deep_copy_term(stable, content);

    // Build RelNode.
    RelNode* rn = stable.make<RelNode>();
    if (!rn) return Term::nil();
    rn->param_count     = 1;
    rn->body            = body;
    rn->captured_count  = 1;
    rn->captured_values = caps;

    return Term::relation(rn);
}

int main() {

    RapLoop loop;
    EXPECT(loop.init(), "RapLoop initializes");
    if (failed) { std::printf("\nAborting: init failed.\n"); return 1; }

    // -------------------------------------------------------------------------
    // Load the strengthen-agendao relation definitions.
    // Now using rel-argso + find-by-contento for introspection.
    // Each agenda entry is (id . rel-term) where rel-term is a carrier Rel
    // with captured_values = [(check H T)] or [(check+ H T R)] etc.
    // -------------------------------------------------------------------------
    const char* defs =
        // weak-check-qido: entry is (qid . rel-term), rel-argso gives [(check H T)]
        "(defrel (weak-check-qido entry H T qid)"
        "  (fresh (rel-term args)"
        "    (== entry (qid . rel-term))"
        "    (rel-argso rel-term args)"
        "    (== args ((check H T)))))"

        // not-weak-check-qido: same shape but content != (check H T)
        "(defrel (not-weak-check-qido entry H T)"
        "  (fresh (qid rel-term args chk)"
        "    (== entry (qid . rel-term))"
        "    (rel-argso rel-term args)"
        "    (== args (chk))"
        "    (=/= chk (check H T))))"

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

        // find-by-contento: find entry whose rel-argso matches pattern-list
        "(defrel (find-by-contento agenda pattern-list id)"
        "  (fresh (entry rest entry-id entry-rel entry-args)"
        "    (== agenda (entry . rest))"
        "    (disj"
        "      (conj"
        "        (== entry (entry-id . entry-rel))"
        "        (rel-argso entry-rel entry-args)"
        "        (== entry-args pattern-list)"
        "        (== id entry-id))"
        "      (find-by-contento rest pattern-list id))))"

        // strengthen-agendao: find the strong check+ entry via find-by-contento
        "(defrel (strengthen-agendao agenda ops)"
        "  (fresh (H T R strong-qid weak-qids ops0)"
        "    (conj"
        "      (find-by-contento agenda ((check+ H T R)) strong-qid)"
        "      (collect-weak-qidso agenda H T weak-qids)"
        "      (call qids->remove-opso weak-qids ops0)"
        "      (call cons-ops (output (pruned H T)) ops0 ops))))";

    EXPECT(loop.load_defs(defs), "Relation definitions load");
    if (failed) { std::printf("\nAborting: load_defs failed.\n"); return 1; }

    // -------------------------------------------------------------------------
    // Set up the test agenda.
    //
    // Enqueue strengthen-agendao FIRST (it will be at the front of the queue),
    // then set next_id=10 and enqueue the 4 carrier entries. When run_one()
    // dequeues strengthen-agendao, the remaining agenda is:
    //   [id=10: carrier((check  hypA test1)),
    //    id=11: carrier((check+ hypA test1 refineX)),
    //    id=12: carrier((check  hypA test1)),
    //    id=13: carrier((explore hypB 2))]
    // strengthen-agendao finds id=11 as the strong check+ and emits
    // Remove(10), Remove(12), Output((pruned hypA test1)).
    // -------------------------------------------------------------------------

    // Enqueue strengthen-agendao first.
    std::uint32_t sa_id = loop.enqueue_query("strengthen-agendao");
    EXPECT(sa_id != 0u, "strengthen-agendao enqueued");
    if (failed) { std::printf("\nAborting: enqueue_query failed.\n"); return 1; }

    // Force the 4 carrier entries to get ids 10-13.
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

    // Build carrier Rel terms in the permanent intern_arena.
    Term carrier10 = make_carrier(loop.intern_arena, content10);
    Term carrier11 = make_carrier(loop.intern_arena, content11);
    Term carrier12 = make_carrier(loop.intern_arena, content12);
    Term carrier13 = make_carrier(loop.intern_arena, content13);

    // Enqueue carriers (deep-copied into agenda buffer).
    std::uint32_t id10 = loop.agenda.enqueue(carrier10);
    std::uint32_t id11 = loop.agenda.enqueue(carrier11);
    std::uint32_t id12 = loop.agenda.enqueue(carrier12);
    std::uint32_t id13 = loop.agenda.enqueue(carrier13);

    EXPECT(id10 == 10u, "item10 gets id=10");
    EXPECT(id11 == 11u, "item11 gets id=11");
    EXPECT(id12 == 12u, "item12 gets id=12");
    EXPECT(id13 == 13u, "item13 gets id=13");

    // Agenda now: [sa, carrier10, carrier11, carrier12, carrier13]
    EXPECT(loop.agenda.count == 5u, "Agenda has 5 entries before run");

    // -------------------------------------------------------------------------
    // Run strengthen-agendao (dequeued from front).
    // After execution, expect:
    //   - ChangeSet: Remove(10), Remove(12), Output((pruned hypA test1))
    //   - Agenda after apply: carrier11 and carrier13 remain (count=2)
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
