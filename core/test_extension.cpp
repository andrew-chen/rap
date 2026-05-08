// Stage 0C: Test suite for the base Evaluator extension mechanism.
// Tests the virtual handleUnknownRelation, ClientRegion save/restore,
// and that the default implementation returns failure without crashing.
//
// Build from project root:
//   clang++ -std=c++20 -O2 -Wall -Wextra -pedantic -Werror \
//           -o core_test_extension core/test_extension.cpp

#include "sexp_parser.hpp"
#include <cstdio>
#include <cassert>

// ============================================================================
// Minimal test subclass: counts handleUnknownRelation calls and optionally
// succeeds or fails based on a flag.
// ============================================================================
class TestEvaluator : public Evaluator {
public:
  bool should_succeed = false;
  int  call_count     = 0;

  TestEvaluator(Arena* a, const OutcomeSyms* s)
    : Evaluator(a, s) {}

protected:
  StepResult handleUnknownRelation(
      const SymEntry* name, const Term* args,
      std::uint32_t arg_count, State& st) override
  {
    (void)name; (void)args; (void)arg_count;
    ++call_count;
    if (should_succeed) {
      // Commit any client region state before signalling success.
      st.client_offset = client_region_.offset;
      return StepResult::Yield;
    }
    return StepResult::NoYield;
  }
};

// ============================================================================
// Counters and EXPECT macro
// ============================================================================
static int passed = 0;
static int failed = 0;

#define EXPECT(cond, msg) \
  do { if (cond) { ++passed; std::printf("PASS: %s\n", msg); } \
       else { ++failed; std::printf("FAIL: %s\n", msg); } } while (0)

// ============================================================================
// Tests
// ============================================================================
int main() {

  // --------------------------------------------------------------------------
  // Test 1: Unknown relation through base Evaluator → failure, not crash
  // --------------------------------------------------------------------------
  {
    alignas(64) unsigned char mem[1 << 16];
    Arena a(mem, sizeof(mem));
    ParsedQuery pq = parse_query(a, "(run 1 (q) (call unknown-relation q))");
    EXPECT(pq.goal != nullptr, "T1: query parses");

    int count = 0;
    Evaluator eval(&a, &pq.outcome_syms);
    eval.runN(pq.n, pq.goal, pq.qvar, pq.vars_used, pq.rel_env,
        [&](Term, State) { ++count; });
    EXPECT(count == 0, "T1: unknown relation produces no solutions");
    a.reset();
  }

  // --------------------------------------------------------------------------
  // Test 2: Default handleUnknownRelation returns failure
  // --------------------------------------------------------------------------
  {
    alignas(64) unsigned char mem[1 << 16];
    Arena a(mem, sizeof(mem));
    ParsedQuery pq = parse_query(a, "(run 1 (q) (call no-ops q))");
    EXPECT(pq.goal != nullptr, "T2: query parses");

    int count = 0;
    TestEvaluator eval(&a, &pq.outcome_syms);
    eval.should_succeed = false;
    eval.runN(pq.n, pq.goal, pq.qvar, pq.vars_used, pq.rel_env,
        [&](Term, State) { ++count; });
    EXPECT(count == 0, "T2: default handleUnknownRelation returns failure");
    EXPECT(eval.call_count == 1, "T2: handleUnknownRelation was called once");
    a.reset();
  }

  // --------------------------------------------------------------------------
  // Test 3: ClientRegion offset saved and restored on backtrack
  // Direct unit test of the ClientRegion machinery — no relational query.
  // --------------------------------------------------------------------------
  {
    alignas(64) unsigned char mem[1 << 16];
    Arena a(mem, sizeof(mem));
    OutcomeSyms syms{};  // empty syms OK — no relational queries run here

    struct RegionTestEval : public Evaluator {
      RegionTestEval(Arena* a2, const OutcomeSyms* s) : Evaluator(a2, s) {
        // Manually set up a small client region for testing.
        std::uint8_t* buf =
            static_cast<std::uint8_t*>(a2->alloc(256, 1));
        client_region_.id       = ClientId::None;
        client_region_.base     = buf;
        client_region_.capacity = 256;
        client_region_.offset   = 0;
      }

      void run_test() {
        std::uint32_t saved = client_region_.offset;
        EXPECT(saved == 0, "T3: initial offset is 0");

        client_region_.alloc(10);
        EXPECT(client_region_.offset == 10, "T3: offset advances after alloc");

        client_region_.restore(saved);
        EXPECT(client_region_.offset == 0, "T3: offset restored after backtrack");
      }

    protected:
      StepResult handleUnknownRelation(
          const SymEntry*, const Term*,
          std::uint32_t, State&) override {
        return StepResult::NoYield;
      }
    };

    RegionTestEval eval(&a, &syms);
    eval.run_test();
    a.reset();
  }

  // --------------------------------------------------------------------------
  // Test 4: TestEvaluator succeeds when should_succeed = true
  // --------------------------------------------------------------------------
  {
    alignas(64) unsigned char mem[1 << 16];
    Arena a(mem, sizeof(mem));
    ParsedQuery pq = parse_query(a, "(run 1 (q) (call extension-rel q))");
    EXPECT(pq.goal != nullptr, "T4: query parses");

    int count = 0;
    TestEvaluator eval(&a, &pq.outcome_syms);
    eval.should_succeed = true;
    eval.runN(pq.n, pq.goal, pq.qvar, pq.vars_used, pq.rel_env,
        [&](Term, State) { ++count; });
    EXPECT(count == 1, "T4: extension relation can succeed");
    EXPECT(eval.call_count == 1, "T4: called exactly once");
    a.reset();
  }

  // --------------------------------------------------------------------------
  // Summary
  // --------------------------------------------------------------------------
  std::printf("\n%d passed, %d failed\n", passed, failed);
  return failed == 0 ? 0 : 1;
}
