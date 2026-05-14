// rap/bench_stage2.cpp — performance benchmark for strengthen-agendao
// Times only the run_one() call across 100 iterations.
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

// Replicate parse_term helper from test_stage2.cpp
static Term parse_term_local(RapLoop& loop, Arena& tmp, const char* str) {
    Lexer lx{str};
    Token tok = lx.next();
    const Sexp* sx = parse_sexp(tmp, loop.intern, lx, tok);
    if (!sx) return Term::nil();
    return compile_term(tmp, nullptr, nullptr, sx);
}

int main() {
    // One-time setup: RapLoop init + load defs
    RapLoop loop;
    if (!loop.init()) { std::printf("RapLoop init failed\n"); return 1; }

    const char* defs =
        "(defrel (membero x lst)"
        "  (disj"
        "    (fresh (rest) (== lst (x . rest)))"
        "    (fresh (head rest)"
        "      (conj (== lst (head . rest))"
        "            (call membero x rest)))))"

        "(defrel (weak-check-qido item H T qid)"
        "  (== item (q qid (check H T))))"

        "(defrel (not-weak-check-qido item H T)"
        "  (fresh (tag id chk)"
        "    (== item (tag id chk))"
        "    (=/= chk (check H T))))"

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
        "          (conj"
        "            (call not-weak-check-qido item H T)"
        "            (call collect-weak-qidso rest H T qids)))))))"

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

    if (!loop.load_defs(defs)) { std::printf("load_defs failed\n"); return 1; }

    // Save intern_arena cursor after all defs are loaded and all symbols are
    // interned.  Each run_one() deep-copies the output term into intern_arena;
    // restoring this cursor at the start of every iteration reclaims those
    // PairNodes (output.reset() clears the references first, so no dangling use).
    std::byte* const intern_safe = loop.intern_arena.cur;

    // Parse data terms once (symbols interned into loop.intern, deep-copied on enqueue)
    alignas(64) std::uint8_t term_buf[8 * 1024];
    Arena tmp(term_buf, sizeof(term_buf));

    Term item10 = parse_term_local(loop, tmp, "(q 10 (check  hypA test1))");
    Term item11 = parse_term_local(loop, tmp, "(q 11 (check+ hypA test1 refineX))");
    Term item12 = parse_term_local(loop, tmp, "(q 12 (check  hypA test1))");
    Term item13 = parse_term_local(loop, tmp, "(q 13 (explore hypB 2))");

    if (item10.tag != TermTag::Pair || item11.tag != TermTag::Pair ||
        item12.tag != TermTag::Pair || item13.tag != TermTag::Pair) {
        std::printf("term parse failed\n"); return 1;
    }

    constexpr int WARMUP = 10;
    constexpr int ITERS  = 100;

    std::vector<double> times(ITERS);

    // Helper: reset agenda and output, re-enqueue all items for one iteration.
    // Also restores the intern_arena cursor to reclaim the PairNodes deep-copied
    // for the previous iteration's output term (safe: output.reset() clears refs).
    auto setup_iteration = [&]() {
        loop.agenda.head    = 0;
        loop.agenda.tail    = 0;
        loop.agenda.count   = 0;
        loop.agenda.next_id = 1;
        loop.output.reset();
        loop.intern_arena.cur = intern_safe;  // reclaim output PairNodes

        loop.enqueue_query("strengthen-agendao");  // id=1
        loop.agenda.next_id = 10;
        loop.agenda.enqueue(item10);  // id=10
        loop.agenda.enqueue(item11);  // id=11
        loop.agenda.enqueue(item12);  // id=12
        loop.agenda.enqueue(item13);  // id=13
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

    // Verify correctness on a final run
    setup_iteration();
    loop.run_one();
    bool correct = (loop.output.count == 1 && loop.agenda.count == 2);
    std::printf("Correctness check: %s\n", correct ? "PASS" : "FAIL");

    return correct ? 0 : 1;
}
