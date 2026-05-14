// Stage 0C: Test suite for RapEvaluator backtrack behavior.
// Tests no-ops/cons-ops stubs, client-region rewind across choice points,
// ClientId safety check, and nested choice points.
//
// Build from project root:
//   clang++ -std=c++20 -O2 -Wall -Wextra -pedantic -Werror \
//           -o rap_test_extension rap/test_rap_extension.cpp

#include "rap.hpp"
#include <cstdio>
#include <vector>

// ============================================================================
// Counters and EXPECT macro
// ============================================================================
static int passed = 0;
static int failed = 0;

#define EXPECT(cond, msg) \
  do { if (cond) { ++passed; std::printf("PASS: %s\n", msg); } \
       else { ++failed; std::printf("FAIL: %s\n", msg); } } while (0)

// ============================================================================
// RapEvalAccessor — thin subclass to expose protected client_region_ fields.
// ============================================================================
struct RapEvalAccessor : public RapEvaluator {
  RapEvalAccessor(Arena* a, Intern* intern, const OutcomeSyms* s)
    : RapEvaluator(a, intern, s) {}
  std::uint32_t get_offset() const { return client_region_.offset; }
  void set_id(ClientId id) { client_region_.id = id; }
};

// ============================================================================
// Tests
// ============================================================================
int main() {

  // --------------------------------------------------------------------------
  // Test 5: no-ops succeeds and advances client region offset
  // --------------------------------------------------------------------------
  {
    alignas(64) unsigned char mem[1 << 16];
    Arena a(mem, sizeof(mem));
    ParsedQuery pq = parse_query(a, "(run 1 (q) (call no-ops q))");
    EXPECT(pq.goal != nullptr, "T5: query parses");

    int count = 0;
    RapEvalAccessor eval(&a, &pq.intern, &pq.outcome_syms);
    std::uint32_t offset_before = eval.get_offset();
    eval.runN(pq.n, pq.goal, pq.qvar, pq.vars_used, pq.rel_env,
        [&](Term, State) { ++count; });
    EXPECT(count == 1, "T5: no-ops produces one solution");
    EXPECT(eval.get_offset() > offset_before, "T5: client region offset advanced");
    a.reset();
  }

  // --------------------------------------------------------------------------
  // Test 6: Backtrack rewinds client region between branches
  // --------------------------------------------------------------------------
  {
    alignas(64) unsigned char mem[1 << 16];
    Arena a(mem, sizeof(mem));
    ParsedQuery pq = parse_query(a,
        "(run 5 (q) (disj (conj (call no-ops q) (== q first))"
        "                 (conj (call no-ops q) (== q second))))");
    EXPECT(pq.goal != nullptr, "T6: query parses");

    struct TrackingEval : public RapEvaluator {
      std::vector<std::uint32_t> offsets_at_call;
      TrackingEval(Arena* a2, Intern* i, const OutcomeSyms* s)
        : RapEvaluator(a2, i, s) {}
    protected:
      StepResult handleUnknownRelation(
          const SymEntry* name, const Term* args,
          std::uint32_t arg_count, State& st) override
      {
        offsets_at_call.push_back(client_region_.offset);
        return RapEvaluator::handleUnknownRelation(name, args, arg_count, st);
      }
    };

    int count = 0;
    TrackingEval eval(&a, &pq.intern, &pq.outcome_syms);
    eval.runN(pq.n, pq.goal, pq.qvar, pq.vars_used, pq.rel_env,
        [&](Term, State) { ++count; });
    EXPECT(count == 2, "T6: two solutions produced");
    EXPECT(eval.offsets_at_call.size() == 2u, "T6: handleUnknownRelation called twice");
    if (eval.offsets_at_call.size() == 2u) {
      EXPECT(eval.offsets_at_call[0] == eval.offsets_at_call[1],
             "T6: client region offset rewound between branches");
    }
    a.reset();
  }

  // --------------------------------------------------------------------------
  // Test 7: cons-ops succeeds with 3 arguments
  // --------------------------------------------------------------------------
  {
    alignas(64) unsigned char mem[1 << 16];
    Arena a(mem, sizeof(mem));
    ParsedQuery pq = parse_query(a,
        "(run 1 (q) (call cons-ops some-op empty-ops q))");
    EXPECT(pq.goal != nullptr, "T7: query parses");

    int count = 0;
    RapEvalAccessor eval(&a, &pq.intern, &pq.outcome_syms);
    eval.runN(pq.n, pq.goal, pq.qvar, pq.vars_used, pq.rel_env,
        [&](Term, State) { ++count; });
    EXPECT(count == 1, "T7: cons-ops/3 succeeds");
    a.reset();
  }

  // --------------------------------------------------------------------------
  // Test 8: cons-ops fails with wrong argument count
  // --------------------------------------------------------------------------
  {
    alignas(64) unsigned char mem[1 << 16];
    Arena a(mem, sizeof(mem));
    ParsedQuery pq = parse_query(a, "(run 1 (q) (call cons-ops q))");
    EXPECT(pq.goal != nullptr, "T8: query parses");

    int count = 0;
    RapEvalAccessor eval(&a, &pq.intern, &pq.outcome_syms);
    eval.runN(pq.n, pq.goal, pq.qvar, pq.vars_used, pq.rel_env,
        [&](Term, State) { ++count; });
    EXPECT(count == 0, "T8: cons-ops/1 fails (wrong arity)");
    a.reset();
  }

  // --------------------------------------------------------------------------
  // Test 9: Wrong ClientId → failure, not crash
  // --------------------------------------------------------------------------
  {
    alignas(64) unsigned char mem[1 << 16];
    Arena a(mem, sizeof(mem));
    ParsedQuery pq = parse_query(a, "(run 1 (q) (call no-ops q))");
    EXPECT(pq.goal != nullptr, "T9: query parses");

    int count = 0;
    RapEvalAccessor eval(&a, &pq.intern, &pq.outcome_syms);
    eval.set_id(ClientId::None);  // corrupt the client id
    eval.runN(pq.n, pq.goal, pq.qvar, pq.vars_used, pq.rel_env,
        [&](Term, State) { ++count; });
    EXPECT(count == 0, "T9: wrong ClientId returns failure, not crash");
    a.reset();
  }

  // --------------------------------------------------------------------------
  // Test 10: Fully unknown relation → failure, not crash
  // --------------------------------------------------------------------------
  {
    alignas(64) unsigned char mem[1 << 16];
    Arena a(mem, sizeof(mem));
    ParsedQuery pq = parse_query(a,
        "(run 1 (q) (call completely-unknown-thing q))");
    EXPECT(pq.goal != nullptr, "T10: query parses");

    int count = 0;
    RapEvalAccessor eval(&a, &pq.intern, &pq.outcome_syms);
    eval.runN(pq.n, pq.goal, pq.qvar, pq.vars_used, pq.rel_env,
        [&](Term, State) { ++count; });
    EXPECT(count == 0, "T10: unknown relation fails cleanly");
    a.reset();
  }

  // --------------------------------------------------------------------------
  // Test 11: Nested choice points with client allocations at multiple levels
  // --------------------------------------------------------------------------
  {
    alignas(64) unsigned char mem[1 << 16];
    Arena a(mem, sizeof(mem));
    ParsedQuery pq = parse_query(a,
        "(run 5 (q) (disj (conj (call no-ops q) (call no-ops q))"
        "                 (conj (call no-ops q) (== q other))))");
    EXPECT(pq.goal != nullptr, "T11: query parses");

    int count = 0;
    RapEvalAccessor eval(&a, &pq.intern, &pq.outcome_syms);
    eval.runN(pq.n, pq.goal, pq.qvar, pq.vars_used, pq.rel_env,
        [&](Term, State) { ++count; });
    EXPECT(count == 2, "T11: nested choice points produce two solutions");
    a.reset();
  }

  // --------------------------------------------------------------------------
  // Summary
  // --------------------------------------------------------------------------
  std::printf("\n%d passed, %d failed\n", passed, failed);
  return failed == 0 ? 0 : 1;
}
