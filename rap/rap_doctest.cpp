// rap/rap_doctest.cpp — .rap doctest runner
//
// Reads one or more .rap files, each of which may embed expected output using
// triple-semicolon markers:
//
//   ;;; ARGS: token1 token2 ...   (optional)
//   ;;; EXPECT
//   ;;; (expected output term)
//   ;;; END EXPECT
//
// For each file, the runner loads stdlib (if available), then loads the file's
// defrel forms, calls main with the declared args, drains the agenda, and
// compares the actual output terms against the expected terms structurally.
//
// Stdlib loading (three-step fallback):
//   1. --stdlib PATH flag (explicit override)
//   2. stdlib/core.rap relative to cwd (default)
//   3. Warning to stderr + continue without stdlib
//
// Build from project root:
//   clang++ -std=c++20 -O2 -Wall -Wextra -pedantic -Werror \
//           -o rap_doctest rap/rap_doctest.cpp
//
// Usage:
//   ./rap_doctest [--stdlib PATH] tests/test_hello.rap tests/test_multi.rap ...
//
// Exit code: 0 if all tests passed, 1 if any failed.

#include "loop.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// ============================================================================
// File I/O
// ============================================================================

static bool read_file(const char* path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return f.good() || f.eof();
}

// ============================================================================
// Marker extraction
// ============================================================================

struct Markers {
    std::vector<std::string> args_tokens;   // from ;;; ARGS: ...
    std::vector<std::string> expect_lines;  // stripped lines from ;;; EXPECT block
};

// Strip ";;; " prefix from a line (exactly four chars: three semicolons + space).
// Returns true and sets out if the line starts with ";;; " (four chars).
static bool strip_marker_prefix(const std::string& line, std::string& out) {
    if (line.size() >= 4 &&
        line[0] == ';' && line[1] == ';' && line[2] == ';' && line[3] == ' ') {
        out = line.substr(4);
        return true;
    }
    return false;
}

// Split a string by ASCII whitespace.
static std::vector<std::string> split_whitespace(const std::string& s) {
    std::vector<std::string> tokens;
    std::istringstream ss(s);
    std::string tok;
    while (ss >> tok) tokens.push_back(tok);
    return tokens;
}

static Markers extract_markers(const std::string& src) {
    Markers m;
    std::istringstream ss(src);
    std::string line;
    bool in_expect = false;

    while (std::getline(ss, line)) {
        // Trim trailing '\r' for Windows-style line endings.
        if (!line.empty() && line.back() == '\r') line.pop_back();

        std::string body;
        if (!strip_marker_prefix(line, body)) continue;

        // "END EXPECT" closes the block (check before EXPECT to handle "END EXPECT").
        if (body == "END EXPECT") {
            in_expect = false;
            continue;
        }

        if (!in_expect) {
            if (body == "EXPECT") {
                in_expect = true;
                continue;
            }
            if (body.rfind("ARGS:", 0) == 0) {
                std::string args_str = body.substr(5);
                m.args_tokens = split_whitespace(args_str);
                continue;
            }
        } else {
            // Inside EXPECT block: each non-empty body line is one expected term.
            if (!body.empty()) m.expect_lines.push_back(body);
        }
    }
    return m;
}

// ============================================================================
// Term comparison (structural; Sym compared by string content, not pointer)
// ============================================================================

static bool terms_equal(Term a, Term b) {
    if (a.tag != b.tag) return false;
    switch (a.tag) {
        case TermTag::Nil:  return true;
        case TermTag::Int:  return a.value == b.value;
        case TermTag::Sym:
            if (!a.sym || !b.sym) return a.sym == b.sym;
            if (a.sym->len != b.sym->len) return false;
            return bytes_eq(a.sym->str, b.sym->str, a.sym->len);
        case TermTag::Pair:
            if (!a.pair || !b.pair) return a.pair == b.pair;
            return terms_equal(a.pair->car, b.pair->car) &&
                   terms_equal(a.pair->cdr, b.pair->cdr);
        case TermTag::Rel:
            // Rel terms cannot be meaningfully compared — treated as mismatch.
            return false;
        default:
            // Var, BVar: should not appear in ground output; treat as mismatch.
            return false;
    }
}

static bool term_is_rel(Term t) {
    return t.tag == TermTag::Rel;
}

// ============================================================================
// Stdlib loading into a RapLoop
// ============================================================================

// Try to load a single file into loop via load_defs.  Returns true on success.
static bool load_file_into_loop(RapLoop& loop, const char* path) {
    std::string src;
    if (!read_file(path, src)) return false;
    return loop.load_defs(src.c_str());
}

// Three-step stdlib loading.  Returns the path that was loaded, or "" if none.
// explicit_stdlib: from --stdlib flag ("" means not given).
// On fallback: prints warning to stderr and returns "".
static std::string load_stdlib(RapLoop& loop, const std::string& explicit_stdlib) {
    // Step 1: explicit --stdlib PATH.
    if (!explicit_stdlib.empty()) {
        if (load_file_into_loop(loop, explicit_stdlib.c_str()))
            return explicit_stdlib;
        std::fprintf(stderr, "rap_doctest: error: --stdlib '%s' could not be loaded\n",
                     explicit_stdlib.c_str());
        return "";
    }

    // Step 2: default path relative to cwd.
    const char* default_path = "stdlib/core.rap";
    if (load_file_into_loop(loop, default_path))
        return default_path;

    // Step 3: visible warning, continue without stdlib.
    std::fprintf(stderr,
        "rap_doctest: warning: could not load stdlib/core.rap (tried '%s'); "
        "proceeding without it — tests using stdlib relations may fail\n",
        default_path);
    return "";
}

// ============================================================================
// Per-file test runner
// ============================================================================

// Parse a term from a string using the loop's own intern table and a temp arena.
static Term parse_expected_term(RapLoop& loop, Arena& tmp, const std::string& s) {
    Lexer lx{s.c_str()};
    Token tok = lx.next();
    const Sexp* sx = parse_sexp(tmp, loop.intern_arena, loop.intern, lx, tok);
    if (!sx) return Term::nil();
    return compile_term(tmp, nullptr, nullptr, sx);
}

// Run one .rap file.  Returns true if the file passed.
static bool run_test_file(const char* path, const std::string& explicit_stdlib) {
    // --- read file ---
    std::string src;
    if (!read_file(path, src)) {
        std::fprintf(stderr, "FAIL %s: could not read file\n", path);
        return false;
    }

    // --- extract markers ---
    Markers m = extract_markers(src);

    // --- set up RapLoop ---
    RapLoop loop;
    loop.quiet = true;
    if (!loop.init()) {
        std::fprintf(stderr, "FAIL %s: RapLoop::init() failed\n", path);
        return false;
    }

    // --- load stdlib (three-step) ---
    load_stdlib(loop, explicit_stdlib);

    // --- load test file's defrels ---
    if (!loop.load_defs(src.c_str())) {
        std::fprintf(stderr, "FAIL %s: load_defs failed (no defrel forms parsed)\n", path);
        return false;
    }

    // --- build args term and call main ---
    Term args = loop.build_args_term(m.args_tokens);
    if (!loop.call_main(args)) {
        std::fprintf(stderr, "FAIL %s: call_main() returned false\n", path);
        return false;
    }

    // drain any agenda entries main added
    loop.run_until_empty();

    // --- parse expected terms using a temp arena ---
    alignas(64) std::uint8_t tmp_buf[64 * 1024];
    Arena tmp(tmp_buf, sizeof(tmp_buf));

    std::vector<Term> expected;
    for (const auto& line : m.expect_lines) {
        Term t = parse_expected_term(loop, tmp, line);
        expected.push_back(t);
    }

    // --- compare ---
    std::uint32_t actual_count = loop.output.count;
    std::uint32_t expect_count = static_cast<std::uint32_t>(expected.size());

    if (actual_count != expect_count) {
        std::fprintf(stderr,
            "FAIL %s: expected %u output term(s), got %u\n",
            path, expect_count, actual_count);
        std::fprintf(stderr, "  actual output:\n");
        for (std::uint32_t i = 0; i < actual_count; ++i) {
            std::fprintf(stderr, "    [%u] ", i);
            fprint_term(stderr, loop.output.terms[i]);
            std::fprintf(stderr, "\n");
        }
        return false;
    }

    for (std::uint32_t i = 0; i < expect_count; ++i) {
        Term act = loop.output.terms[i];
        Term exp = expected[i];

        if (term_is_rel(act) || term_is_rel(exp)) {
            std::fprintf(stderr,
                "FAIL %s: term[%u]: Rel values cannot be compared "
                "(see tests/README.md — future work limitation)\n", path, i);
            return false;
        }

        if (!terms_equal(act, exp)) {
            std::fprintf(stderr, "FAIL %s: term[%u] mismatch\n", path, i);
            std::fprintf(stderr, "  expected: ");
            fprint_term(stderr, exp);
            std::fprintf(stderr, "\n  actual:   ");
            fprint_term(stderr, act);
            std::fprintf(stderr, "\n");
            return false;
        }
    }

    std::fprintf(stderr, "PASS %s\n", path);
    return true;
}

// ============================================================================
int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "Usage: rap_doctest [--stdlib PATH] file.rap [file.rap ...]\n");
        return 1;
    }

    std::string explicit_stdlib;
    std::vector<const char*> files;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--stdlib") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "rap_doctest: --stdlib requires a PATH argument\n");
                return 1;
            }
            explicit_stdlib = argv[++i];
        } else {
            files.push_back(argv[i]);
        }
    }

    if (files.empty()) {
        std::fprintf(stderr, "rap_doctest: no test files given\n");
        return 1;
    }

    int passed = 0, failed = 0;
    for (const char* path : files) {
        if (run_test_file(path, explicit_stdlib)) ++passed;
        else                                       ++failed;
    }

    std::fprintf(stderr, "\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
