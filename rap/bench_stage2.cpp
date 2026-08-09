// rap/bench_stage2.cpp — performance benchmark for subsume-and-pruneo
// Times only the run_one() call across 100 iterations after 10 warmup runs.
// Compile:
//   clang++ -std=c++20 -O2 -Wall -Wextra -pedantic -Werror \
//           -o bench_stage2 rap/bench_stage2.cpp
#include "loop.hpp"
#include <chrono>
#include <algorithm>
#include <vector>
#include <cstdio>

using Clock = std::chrono::high_resolution_clock;
using NS    = std::chrono::nanoseconds;

static const char* SUBSUME_DEFS =
    "(defrel (subsumeso strong weak)"
    "  (fresh (qidS relS qidW relW H dS dW)"
    "    (== strong (qidS relS (explore H dS)))"
    "    (== weak (qidW relW (explore H dW)))"
    "    (leqo dW dS)))"
    "(defrel (is-explore-entryo entry)"
    "  (fresh (qid rel-term H D)"
    "    (== entry (qid rel-term (explore H D)))))"
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
    "(defrel (is-subsumedo agenda candidate result)"
    "  (disj"
    "    (conj"
    "      (probe (has-subsuming-entryo agenda candidate) true 1000 true false)"
    "      (== result yes))"
    "    (conj"
    "      (probe (has-subsuming-entryo agenda candidate) false 1000 true false)"
    "      (== result no))))"
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

static Term parse_term_local(RapLoop& loop, Arena& tmp, const char* str) {
    Lexer lx{str};
    Token tok = lx.next();
    const Sexp* sx = parse_sexp(tmp, loop.intern_arena, loop.intern, lx, tok);
    if (!sx) return Term::nil();
    return compile_term(tmp, nullptr, nullptr, sx);
}

int main() {
    RapLoop loop;
    loop.quiet = true;
    if (!loop.init()) { std::printf("RapLoop init failed\n"); return 1; }
    if (!loop.load_defs(SUBSUME_DEFS)) { std::printf("load_defs failed\n"); return 1; }

    // Save intern_arena cursor after all defs are loaded.  Each run_one()
    // deep-copies the output term into intern_arena; restoring this cursor at
    // the start of every iteration reclaims those PairNodes.
    std::byte* const intern_safe = loop.intern_arena.cur;

    alignas(64) std::uint8_t term_buf[8 * 1024];
    Arena tmp(term_buf, sizeof(term_buf));

    // content terms for the 4 explore entries (parsed once)
    Term content10 = parse_term_local(loop, tmp, "(explore hypA 1)");
    Term content11 = parse_term_local(loop, tmp, "(explore hypA 2)");
    Term content12 = parse_term_local(loop, tmp, "(explore hypA 3)");
    Term content13 = parse_term_local(loop, tmp, "(explore hypB 5)");

    if (content10.tag != TermTag::Pair || content11.tag != TermTag::Pair ||
        content12.tag != TermTag::Pair || content13.tag != TermTag::Pair) {
        std::printf("term parse failed\n"); return 1;
    }

    Term minimal_rel = make_minimal_rel(loop.intern_arena);

    constexpr int WARMUP = 10;
    constexpr int ITERS  = 100;

    std::vector<double> times(ITERS);

    // Reset the agenda and output; re-enqueue all items for one iteration.
    auto setup_iteration = [&]() {
        loop.agenda.head    = 0;
        loop.agenda.tail    = 0;
        loop.agenda.count   = 0;
        loop.agenda.next_id = 1;
        loop.output.reset();
        loop.intern_arena.cur = intern_safe;  // reclaim previous output PairNodes

        loop.enqueue_query("subsume-and-pruneo");  // id=1
        loop.agenda.next_id = 10;
        loop.agenda.enqueue(minimal_rel, content10);  // id=10
        loop.agenda.enqueue(minimal_rel, content11);  // id=11
        loop.agenda.enqueue(minimal_rel, content12);  // id=12
        loop.agenda.enqueue(minimal_rel, content13);  // id=13
    };

    // Warmup
    for (int i = 0; i < WARMUP; ++i) {
        setup_iteration();
        loop.run_one();
    }

    // Measured iterations
    for (int i = 0; i < ITERS; ++i) {
        setup_iteration();
        auto t0 = Clock::now();
        loop.run_one();
        auto t1 = Clock::now();
        times[i] = std::chrono::duration_cast<NS>(t1 - t0).count() / 1000.0;
    }

    std::sort(times.begin(), times.end());
    double med = (times[ITERS/2 - 1] + times[ITERS/2]) / 2.0;

    std::printf("BENCH_STAGE2_MED=%.2f MIN=%.2f MAX=%.2f\n",
                med, times.front(), times.back());

    // Correctness check on a final run
    setup_iteration();
    loop.run_one();
    bool correct = (loop.output.count == 1 && loop.agenda.count == 2);
    std::printf("Correctness check: %s\n", correct ? "PASS" : "FAIL");

    return correct ? 0 : 1;
}
