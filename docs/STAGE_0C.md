# Stage 0C Specification: Automated Test Suite

**Version:** 1.0  
**Depends on:** Stage 0B complete (docs/STAGE_0B.md)  
**Required before:** Stage 2 (work queue + ChangeSet)  
**Status:** Specification — not yet implemented  
**Date:** May 7, 2026

---

## Purpose

Stage 0C adds an automated test suite that locks down all existing behavior
before Stage 2 makes significant changes to `rap/`. If Stage 2 breaks
anything, the test suite will catch it immediately.

When Stage 0C is complete:

- `core/test_extension.cpp` tests the base `Evaluator` extension mechanism
- `rap/test_rap_extension.cpp` tests `RapEvaluator` backtrack behavior
- Both test files are built by `make` and run as part of the standard
  verification workflow
- All tests pass cleanly

This stage adds tests only. No implementation changes.

---

## Test File 1: core/test_extension.cpp

Tests the base `Evaluator` class extension mechanism directly, without
involving `RapEvaluator`. Uses a minimal test subclass.

### Setup

```cpp
#include "core/sexp_parser.hpp"
#include <cstdio>
#include <cassert>

// Minimal test subclass that counts handleUnknownRelation calls
// and optionally succeeds or fails based on a flag.
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
        ++call_count;
        if (should_succeed) {
            st.client_offset = client_region_.offset;
            return StepResult::Yield;
        }
        return StepResult::NoYield;
    }
};

static int passed = 0;
static int failed = 0;

#define EXPECT(cond, msg) \
    do { if (cond) { ++passed; std::printf("PASS: %s\n", msg); } \
         else { ++failed; std::printf("FAIL: %s\n", msg); } } while(0)
```

### Test 1: Unknown relation with no client → failure, not crash

```
Run: (run 1 (q) (unknown-relation q)) through base Evaluator (no subclass)
Expected: no solutions, no crash
```

```cpp
// Test 1
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
```

### Test 2: Default handleUnknownRelation returns failure

```
Subclass Evaluator but do not override handleUnknownRelation.
Run: (run 1 (q) (call no-ops q))
Expected: no solutions (default returns NoYield)
```

```cpp
// Test 2 — uses TestEvaluator with should_succeed = false (default)
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
```

### Test 3: client_region offset saved and restored on backtrack

```
Unit test of the machinery — not a relational query.
Manually verify that State copy preserves client_offset and
that client_region_.restore() rewinds the offset.
```

```cpp
// Test 3 — direct unit test of ClientRegion save/restore
{
    alignas(64) unsigned char mem[1 << 16];
    Arena a(mem, sizeof(mem));
    OutcomeSyms syms;  // empty syms ok for this test

    // Access client_region_ through a TestEvaluator
    // (client_region_ is protected, accessible from subclass)
    struct RegionTestEval : public Evaluator {
        RegionTestEval(Arena* a, const OutcomeSyms* s) : Evaluator(a, s) {
            // Set up a small client region manually
            uint8_t* buf = static_cast<uint8_t*>(a->alloc(256, 1));
            client_region_.id       = ClientId::None;
            client_region_.base     = buf;
            client_region_.capacity = 256;
            client_region_.offset   = 0;
        }
        void run_test() {
            uint32_t saved = client_region_.offset;
            EXPECT(saved == 0, "T3: initial offset is 0");

            // Allocate 10 bytes
            client_region_.alloc(10);
            EXPECT(client_region_.offset == 10, "T3: offset advances after alloc");

            // Simulate backtrack: restore to saved offset
            client_region_.restore(saved);
            EXPECT(client_region_.offset == 0, "T3: offset restored after backtrack");
        }
    protected:
        StepResult handleUnknownRelation(const SymEntry*, const Term*,
            std::uint32_t, State&) override { return StepResult::NoYield; }
    };

    RegionTestEval eval(&a, &syms);
    eval.run_test();
    a.reset();
}
```

### Test 4: TestEvaluator succeeds when should_succeed = true

```
Verify that a subclass can succeed from handleUnknownRelation
and that the solution is delivered.
```

```cpp
// Test 4
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
```

---

## Test File 2: rap/test_rap_extension.cpp

Tests `RapEvaluator` specifically: backtrack rewind of client region,
no-ops and cons-ops stub behavior, ClientId safety.

### Setup

```cpp
#include "core/sexp_parser.hpp"
#include "rap/rap.hpp"
#include <cstdio>
#include <cassert>

static int passed = 0;
static int failed = 0;

#define EXPECT(cond, msg) \
    do { if (cond) { ++passed; std::printf("PASS: %s\n", msg); } \
         else { ++failed; std::printf("FAIL: %s\n", msg); } } while(0)

// Helper: get client_region offset from RapEvaluator.
// Since client_region_ is protected, use a thin accessor subclass.
struct RapEvalAccessor : public RapEvaluator {
    RapEvalAccessor(Arena* a, Intern* intern, const OutcomeSyms* s)
        : RapEvaluator(a, intern, s) {}
    uint32_t get_offset() const { return client_region_.offset; }
    void set_id(ClientId id) { client_region_.id = id; }
};
```

### Test 5: no-ops succeeds and advances client region offset

```
Run: (run 1 (q) (call no-ops q))
Expected: one solution; client_region offset > 0 after query
```

```cpp
// Test 5
{
    alignas(64) unsigned char mem[1 << 16];
    Arena a(mem, sizeof(mem));
    ParsedQuery pq = parse_query(a, "(run 1 (q) (call no-ops q))");
    EXPECT(pq.goal != nullptr, "T5: query parses");

    int count = 0;
    RapEvalAccessor eval(&a, &pq.intern, &pq.outcome_syms);
    uint32_t offset_before = eval.get_offset();
    eval.runN(pq.n, pq.goal, pq.qvar, pq.vars_used, pq.rel_env,
        [&](Term, State) { ++count; });
    EXPECT(count == 1, "T5: no-ops produces one solution");
    EXPECT(eval.get_offset() > offset_before, "T5: client region offset advanced");
    a.reset();
}
```

### Test 6: Backtrack rewinds client region

```
Run: (run* (q) (disj (conj (call no-ops q) (== q first))
                     (conj (call no-ops q) (== q second))))
Expected: two solutions (first, second).
The client region offset after the first branch should be rewound
before the second branch executes — verified by checking offset
at the start of each handleUnknownRelation call.
```

For this test, instrument `RapEvaluator` by subclassing and recording
offset values at each `handleUnknownRelation` call:

```cpp
// Test 6
{
    alignas(64) unsigned char mem[1 << 16];
    Arena a(mem, sizeof(mem));
    ParsedQuery pq = parse_query(a,
        "(run 5 (q) (disj (conj (call no-ops q) (== q first))"
        "                 (conj (call no-ops q) (== q second))))");
    EXPECT(pq.goal != nullptr, "T6: query parses");

    struct TrackingEval : public RapEvaluator {
        std::vector<uint32_t> offsets_at_call;
        TrackingEval(Arena* a, Intern* i, const OutcomeSyms* s)
            : RapEvaluator(a, i, s) {}
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
    EXPECT(eval.offsets_at_call.size() == 2, "T6: handleUnknownRelation called twice");
    // Both calls should see the same starting offset (backtrack rewound between them)
    EXPECT(eval.offsets_at_call[0] == eval.offsets_at_call[1],
           "T6: client region offset rewound between branches");
    a.reset();
}
```

### Test 7: cons-ops succeeds with 3 arguments

```
Run: (run 1 (q) (call cons-ops some-op empty-ops q))
Expected: one solution
```

```cpp
// Test 7
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
```

### Test 8: cons-ops fails with wrong argument count

```
Run: (run 1 (q) (call cons-ops q))
Expected: no solutions
```

```cpp
// Test 8
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
```

### Test 9: Wrong ClientId → failure, not crash

```
Manually set client_region_.id to ClientId::None on a RapEvaluator.
Run: (run 1 (q) (call no-ops q))
Expected: no solutions (failure), not crash, not assert
```

```cpp
// Test 9
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
```

### Test 10: Fully unknown relation → failure, not crash

```
Run: (run 1 (q) (call completely-unknown-thing q))
Expected: no solutions
```

```cpp
// Test 10
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
```

### Test 11: Nested choice points with client allocations at multiple levels

```
Run: (run* (q) (disj (conj (call no-ops q) (call no-ops q))
                     (conj (call no-ops q) (== q other))))
Expected: two solutions.
Client region offset correctly restored at each backtrack level.
```

```cpp
// Test 11
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
```

---

## main() for Both Test Files

Each test file needs a `main()` that reports results:

```cpp
int main() {
    // ... all test bodies above ...

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
```

Return code 1 on any failure — this allows `make test` to detect failures.

---

## Makefile Additions

Add two new targets. They should be built by `make` (add to `all`) and
run by a new `make test` target:

```makefile
# Test binaries
TESTS = core/test_extension rap/test_rap_extension

all: parse_run test_rap security/security_test \
     core/test_extension rap/test_rap_extension

core/test_extension: core/test_extension.cpp \
    core/sexp_parser.hpp core/core.hpp core/intern.hpp \
    core/arena.hpp core/mktypes.hpp
	$(CXX) $(CXXFLAGS) -o $@ $<

rap/test_rap_extension: rap/test_rap_extension.cpp \
    rap/rap.hpp core/sexp_parser.hpp core/core.hpp \
    core/intern.hpp core/arena.hpp core/mktypes.hpp
	$(CXX) $(CXXFLAGS) -o $@ $<

test: all
	./parse_run
	./security/security_test
	./rap/test_rap
	./core/test_extension
	./rap/test_rap_extension
	@echo "All tests complete."
```

---

## Implementation Notes for Claude Code

**On `std::vector` in `TrackingEval` (Test 6):**
The test uses `std::vector<uint32_t>` to record offsets. This is acceptable
in test code — the `no dynamic allocation` rule applies to the engine, not
to test harnesses. If `-Werror` flags this for any reason, replace with a
fixed-size array.

**On `RapEvalAccessor`:**
`client_region_` is `protected` in `Evaluator`. The accessor subclass
approach is the correct way to observe it from test code. Do not change
`client_region_` to `public`.

**On Test 6 query syntax:**
The query uses multi-line string concatenation. Verify the resulting string
is valid before running — the s-expression parser is whitespace-tolerant so
splitting across strings is fine.

**On the `call` surface syntax:**
All tests use `(call relation-name args...)` which is the Stage 0A surface
syntax. Verify this matches what the parser actually expects after Stage 0A.
If the surface syntax differs (e.g., if it is just `(relation-name args...)`
rather than `(call relation-name args...)`), adjust the query strings
accordingly.

---

## Acceptance Criteria for Stage 0C

Stage 0C is complete when:

- [ ] `core/test_extension.cpp` exists with Tests 1–4
- [ ] `rap/test_rap_extension.cpp` exists with Tests 5–11
- [ ] Both files added to `all` target in Makefile
- [ ] `make test` target added and runs all five test binaries
- [ ] `make` builds cleanly with `-Werror`
- [ ] All 11 tests pass (output: `11 passed, 0 failed` across both files)
- [ ] `parse_run` all 12 programs unchanged
- [ ] `security/security_test` all 10 cases pass
- [ ] `rap/test_rap` all 5 tests pass
- [ ] No implementation changes made — test files only
- [ ] This document updated if any test cases required adjustment

---

## What Stage 2 Builds on Top of This

Stage 2 replaces the `// STAGE 0B STUB` implementations with real
ChangeSet construction. At that point, Tests 5, 7, and 11 will need
to be extended to verify actual ChangeSet content, not just that the
stubs succeed. The test infrastructure from Stage 0C makes this easy —
add assertions, don't rewrite tests.

---

*This document is the authoritative specification for Stage 0C.*  
*If implementation diverges from this document, update it and note why.*

*v1.0 May 7, 2026 — initial specification*
