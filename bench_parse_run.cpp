// bench_parse_run.cpp — performance benchmark for canonical miniKanren programs
// Times Program 8 (appendo forward) and Program 11 (appendo backward)
// across 100 iterations each. Reports median and range in microseconds.
// Compile:
//   clang++ -std=c++20 -O2 -Wall -Wextra -pedantic -Werror \
//           -o bench_parse_run bench_parse_run.cpp
#include "core/sexp_parser.hpp"
#include <chrono>
#include <algorithm>
#include <vector>
#include <cstdio>

using Clock = std::chrono::high_resolution_clock;
using NS    = std::chrono::nanoseconds;

static void run_program(Arena& a, const char* src) {
    ParsedQuery pq = parse_query(a, src);
    if (!pq.goal) { a.reset(); return; }
    Evaluator eval(&a, &pq.outcome_syms);
    eval.runN(pq.n, pq.goal, pq.qvar, pq.vars_used, pq.rel_env,
        [&](Term, State) {});
    a.reset();
}

int main() {
    // Program 8: appendo forward — (1 2) ++ (3 4) = ?
    const char* program8 =
        "(defrel (appendo l r o)"
        "  (disj"
        "    (conj (== l ()) (== o r))"
        "    (fresh (h t res)"
        "      (conj (== l (h . t))"
        "            (conj (== o (h . res))"
        "                  (call appendo t r res))))))"
        "(run 3 (q) (call appendo (1 2) (3 4) q))";

    // Program 11: appendo backward — ? ++ (3 4) = (1 2 3 4)
    const char* program11 =
        "(defrel (appendo l r o)"
        "  (disj"
        "    (conj (== l ()) (== o r))"
        "    (fresh (h t res)"
        "      (conj (== l (h . t))"
        "            (conj (== o (h . res))"
        "                  (call appendo t r res))))))"
        "(run 3 (q) (call appendo q (3 4) (1 2 3 4)))";

    constexpr int WARMUP = 10;
    constexpr int ITERS  = 100;

    alignas(64) unsigned char mem[1 << 17]; // 128 KiB
    Arena a(mem, sizeof(mem));

    std::vector<double> times8(ITERS), times11(ITERS);

    // Warmup
    for (int i = 0; i < WARMUP; ++i) {
        run_program(a, program8);
        run_program(a, program11);
    }

    // Measured iterations — program 8
    for (int i = 0; i < ITERS; ++i) {
        auto t0 = Clock::now();
        run_program(a, program8);
        auto t1 = Clock::now();
        times8[i] = std::chrono::duration_cast<NS>(t1 - t0).count() / 1000.0;
    }

    // Measured iterations — program 11
    for (int i = 0; i < ITERS; ++i) {
        auto t0 = Clock::now();
        run_program(a, program11);
        auto t1 = Clock::now();
        times11[i] = std::chrono::duration_cast<NS>(t1 - t0).count() / 1000.0;
    }

    std::sort(times8.begin(),  times8.end());
    std::sort(times11.begin(), times11.end());

    double med8  = (times8[ITERS/2 - 1]  + times8[ITERS/2])  / 2.0;
    double med11 = (times11[ITERS/2 - 1] + times11[ITERS/2]) / 2.0;

    std::printf("BENCH_P8_MED=%.2f  MIN=%.2f MAX=%.2f\n",
                med8,  times8.front(),  times8.back());
    std::printf("BENCH_P11_MED=%.2f MIN=%.2f MAX=%.2f\n",
                med11, times11.front(), times11.back());

    return 0;
}
