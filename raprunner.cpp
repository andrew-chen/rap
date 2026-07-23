// raprunner.cpp — Reactive RAP program runner
//
// Usage:
//   raprunner program.rkt                 # stdin only
//   raprunner program.rkt arg1 arg2       # CLI args passed to main
//   raprunner program.rkt --fd 4 --fd 7   # watch additional fds
//
// Compile (from the rap/ root):
//   clang++ -std=c++20 -O2 -Wall -Wextra -pedantic -Werror -o raprunner raprunner.cpp

#include "rap/loop.hpp"
#include "rap/agenda.hpp"
#include "rap/spine.hpp"
#include "rap/changeset.hpp"
#include "rap/rap.hpp"
#include "core/sexp_parser.hpp"
#include "core/core.hpp"
#include "core/intern.hpp"
#include "core/arena.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <string>
#include <vector>
#include <unistd.h>
#include <poll.h>
#include <sys/types.h>

// ============================================================================
// Arena layout
// ============================================================================

// prog_arena: long-lived. Holds all defrel Goal trees loaded from program file.
// Never reset.
alignas(64) static std::uint8_t prog_buf[256 * 1024];

// intern_arena: long-lived. Holds interned symbols across the session.
// Never reset.
alignas(64) static std::uint8_t intern_buf[32 * 1024];

// eval_arena: per-query. Reset after each query execution.
alignas(64) static std::uint8_t eval_buf[EVAL_ARENA_SIZE];

// rap_arena: permanent. Holds RapEvaluator's SymEntries and client region.
// Never reset.
alignas(64) static std::uint8_t rap_buf[MAX_CHANGESET_ARENA + 32 * 1024];

// wrap_arena: holds handle_input wrapper RelNodes and char-list PairNodes.
// Reset when no Rel items remain in the agenda.
alignas(64) static std::uint8_t wrap_buf[64 * 1024];


// ============================================================================
// Recursive Var scanner: prints any Var nodes found in a term tree.
// depth: bitmask encoding path (bit k = cdr at level k; else car).
// Returns true if any Var was found.
// ============================================================================
static bool scan_term_vars(Term t, std::uint32_t entry_id,
                           const char* ctx, bool printed_header,
                           int depth, std::uint64_t path) {
    switch (t.tag) {
        case TermTag::Var:
            if (!printed_header) {
                std::fprintf(stderr, "[VARFOUND] entry=%u ctx=%s\n", entry_id, ctx);
                printed_header = true;
            }
            std::fprintf(stderr, "  Var(%u) at depth=%d path=0x%llx addr=%p\n",
                         t.id, depth, (unsigned long long)path, (void*)&t);
            return true;
        case TermTag::Pair:
            if (t.pair) {
                bool a = scan_term_vars(t.pair->car, entry_id, ctx, printed_header,
                                        depth+1, path << 1);
                bool b = scan_term_vars(t.pair->cdr, entry_id, ctx, printed_header || a,
                                        depth+1, (path << 1) | 1);
                return a || b;
            }
            return false;
        default:
            return false;
    }
}

static void scan_agenda_vars(const Agenda& agenda, const char* ctx) {
    std::uint32_t pos = agenda.tail;
    for (std::uint32_t i = 0; i < agenda.count; ++i) {
        const QueryEntry* e = reinterpret_cast<const QueryEntry*>(agenda.buf + pos);
        scan_term_vars(e->args, e->id, ctx, false, 0, 0);
        pos += e->byte_size;
    }
}

// ============================================================================
// apply_changeset: apply the ops from a ChangeSet to the agenda / stdout
// ============================================================================
static void apply_changeset(const ChangeSet& cs,
                            Agenda& agenda,
                            Arena&  intern_arena) {
    for (std::uint32_t i = 0; i < cs.op_count; ++i) {
        const Op& op = cs.ops[i];
        switch (op.tag) {
            case OpTag::Add: {
                std::uint32_t new_id = agenda.enqueue(op.add.rel_term, op.add.args);
                std::uint32_t nparams = (op.add.rel_term.tag == TermTag::Rel && op.add.rel_term.rel)
                    ? op.add.rel_term.rel->param_count : 0;
                std::fprintf(stderr, "[apply] added id=%u nparams=%u\n", new_id, nparams);
                scan_agenda_vars(agenda, "after-add");
                std::fflush(stderr);
                break;
            }
            case OpTag::Remove: {
                std::fprintf(stderr, "[apply] remove qid=%u\n", op.query_id);
                agenda.remove(op.query_id);
                scan_agenda_vars(agenda, "after-remove");
                std::fflush(stderr);
                break;
            }
            case OpTag::Output: {
                // Deep-copy into intern_arena so the term survives eval_arena reset.
                Term stable = deep_copy_term(intern_arena, op.output_term);
                print_term(stable);
                std::printf("\n");
                std::fflush(stdout);
                break;
            }
        }
    }
}


// ============================================================================
// run_one: dequeue and execute one agenda entry (plain FIFO — every entry
// is runnable; there is no state-holder distinction).
// ============================================================================
static void run_one(RapEvaluator& evaluator,
                    Agenda&       agenda,
                    SpineArena&   spine,
                    RelEnv&       rel_env,
                    Arena&        eval_arena,
                    Arena&        intern_arena,
                    bool          verbose = false) {
    // Print queue state before dequeue (include byte_size and args for 3-param entries).
    {
        std::uint32_t pos = agenda.tail;
        std::fprintf(stderr, "[queue] count=%u tail=%u head=%u\n",
                     agenda.count, agenda.tail, agenda.head);
        for (std::uint32_t i = 0; i < agenda.count; ++i) {
            const QueryEntry* e = reinterpret_cast<const QueryEntry*>(agenda.buf + pos);
            std::uint32_t np = (e->query_term.tag == TermTag::Rel && e->query_term.rel)
                               ? e->query_term.rel->param_count : 0;
            std::fprintf(stderr, "  id=%u np=%u bsz=%u pos=%u\n",
                         e->id, np, e->byte_size, pos);
            pos += e->byte_size;
        }
        std::fflush(stderr);
    }

    QueryEntry entry;
    if (!agenda.dequeue(entry)) return;

    if (verbose) {
        std::fprintf(stderr, "[run_one] start id=%u nparams=%u\n",
                     entry.id, entry.query_term.rel ? entry.query_term.rel->param_count : 0);
        std::fflush(stderr);
    }

    // Build agenda list term from remaining entries.
    spine.reset();
    Term agenda_term = agenda.as_term(spine.get());

    evaluator.init_changeset();

    if (entry.query_term.tag == TermTag::Rel) {
        const RelNode* rel     = entry.query_term.rel;
        std::uint32_t  nparams = rel->param_count;

        Term* call_args = static_cast<Term*>(
            eval_arena.alloc(nparams * sizeof(Term), alignof(Term)));
        if (call_args) {
            // Calling convention (N = nparams, always 2 or 3):
            //   call_args[0]     = agenda snapshot
            //   call_args[N-2]   = entry.args (only when N==3; nil if not provided)
            //   call_args[N-1]   = Term::var(1), the ops output variable (always last)
            // vars_used=2 reserves Var(0) (result) and Var(1) (ops).
            call_args[0] = agenda_term;
            if (nparams == 3)
                call_args[1] = entry.args;
            call_args[nparams - 1] = Term::var(1);

            Goal* call_goal = eval_arena.make<Goal>();
            if (call_goal) {
                call_goal->tag  = GoalTag::Call;
                call_goal->call = GoalCall{entry.query_term, call_args, nparams};

                // vars_used = 2: Var(0) = result, Var(1) = ops output.
                bool oom = false;
                evaluator.runN(1, call_goal, Term::var(0), 2, rel_env,
                               [](Term, State) {}, &oom, dump_oom_work_queue);
                if (oom)
                    std::fprintf(stderr,
                        "WARNING: query execution ran out of memory (eval_arena);"
                        " results may be incomplete or wrong.\n");

                evaluator.sync_changeset_op_count();
                ChangeSet* cs2 = evaluator.get_changeset();
                std::uint32_t ops_count = cs2 ? cs2->op_count : 0;
                if (ops_count == 0 && nparams == 3) {
                    std::fprintf(stderr, "[fail] id=%u oom=%d args=",
                                 entry.id, oom ? 1 : 0);
                    print_term(entry.args);
                    std::fprintf(stderr, "\n");
                    std::fflush(stderr);
                }
            }
        }
    }

    evaluator.sync_changeset_op_count();
    ChangeSet* cs = evaluator.get_changeset();
    if (verbose) {
        std::fprintf(stderr, "[run_one] done id=%u cs_ops=%u\n",
                     entry.id, cs ? cs->op_count : 999);
        std::fflush(stderr);
    }
    if (cs) apply_changeset(*cs, agenda, intern_arena);

    if (verbose) {
        print_arena_usage("eval_arena (run)",  eval_arena);
        print_arena_usage("intern_arena",      intern_arena);
    }

    eval_arena.reset();
}


// ============================================================================
// call_main: invoke main(args, ops) and apply the resulting ChangeSet
// ============================================================================
static bool call_main(RapEvaluator& evaluator,
                      Term          main_rel,
                      Term          args_term,
                      Agenda&       agenda,
                      RelEnv&       rel_env,
                      Arena&        eval_arena,
                      Arena&        intern_arena,
                      bool          verbose = false) {
    evaluator.init_changeset();

    // Build: (call main-rel args-term var(0))
    // var(0) is the ops variable that cons-ops/no-ops will unify with.
    Term call_args[2];
    call_args[0] = args_term;
    call_args[1] = Term::var(0);

    Goal* call_goal = eval_arena.make<Goal>();
    if (!call_goal) {
        std::fprintf(stderr, "raprunner: OOM building main call goal\n");
        return false;
    }
    call_goal->tag  = GoalTag::Call;
    call_goal->call = GoalCall{main_rel, call_args, 2};

    bool succeeded = false;
    bool oom       = false;
    evaluator.runN(1, call_goal, Term::var(0), 1, rel_env,
                   [&](Term, State) { succeeded = true; }, &oom,
                   dump_oom_work_queue);

    if (oom)
        std::fprintf(stderr,
            "WARNING: query execution ran out of memory (eval_arena);"
            " results may be incomplete or wrong.\n");

    if (!succeeded) {
        std::fprintf(stderr, "raprunner: 'main' produced no solution\n");
        eval_arena.reset();
        return false;
    }

    ChangeSet* cs = evaluator.get_changeset();
    if (cs) apply_changeset(*cs, agenda, intern_arena);

    if (verbose) {
        print_arena_usage("eval_arena (main)", eval_arena);
        print_arena_usage("intern_arena",      intern_arena);
    }

    eval_arena.reset();
    return true;
}


// ============================================================================
// build_args_term: build (arg1 arg2 ...) as a Pair-list of Sym terms
// ============================================================================
static Term build_args_term(Arena& arena, Intern& intern,
                            const std::vector<const char*>& user_args) {
    Term result = Term::nil();
    for (int i = static_cast<int>(user_args.size()) - 1; i >= 0; --i) {
        const SymEntry* s = intern_cstr(arena, intern, user_args[static_cast<std::size_t>(i)]);
        if (!s) return Term::nil();
        PairNode* p = arena.make<PairNode>();
        if (!p) return Term::nil();
        p->car = Term::symbol(s);
        p->cdr = result;
        result = Term::make_pair(p);
    }
    return result;
}


// ============================================================================
// build_char_list: convert raw bytes into a char-list term
//
// Each byte becomes a single-character symbol (e.g., 'a', '\n').
// PairNodes are allocated in pair_arena; SymEntries in sym_arena.
// Built back-to-front so the final result is in input order.
// ============================================================================
static Term build_char_list(Arena& pair_arena, Arena& sym_arena, Intern& intern,
                             const char* buf, ssize_t len) {
    Term result = Term::nil();
    for (ssize_t i = len - 1; i >= 0; --i) {
        char cbuf[2] = { buf[i], '\0' };
        const SymEntry* s = intern_cstr(sym_arena, intern, cbuf);
        if (!s) return result;
        PairNode* p = pair_arena.make<PairNode>();
        if (!p) return result;
        p->car = Term::symbol(s);
        p->cdr = result;
        result = Term::make_pair(p);
    }
    return result;
}


// ============================================================================
// enqueue_handle_input: build and enqueue a handle_input wrapper query
//
// The wrapper is a 2-param RelNode: (rel (agenda ops) body).
// run_one calls it as: call_args[0]=agenda_term, call_args[1]=Var(1).
//
// BVar resolution in a 2-param wrapper uses DeBruijn indexing from the
// innermost (last) param outward:
//   BVar(0) → depth 0 → last param → Var(base+1) → call_args[1] = Var(1) (ops)
//   BVar(1) → depth 1 → first param → Var(base+0) → call_args[0] = agenda_term
// ============================================================================
static void enqueue_handle_input(Arena&      wrap_arena,
                                 Agenda&     agenda,
                                 SpineArena& spine,
                                 RelEnv&     rel_env,
                                 Arena&      intern_arena,
                                 Intern&     sess_intern,
                                 Term        handle_input_rel,
                                 int         fd,
                                 Term        input_term) {
    // Allocate the args array for the inner call in wrap_arena.
    Term* inner_args = static_cast<Term*>(
        wrap_arena.alloc(4 * sizeof(Term), alignof(Term)));
    if (!inner_args) {
        std::fprintf(stderr, "raprunner: OOM building handle_input args\n");
        return;
    }
    // BVar(1) is the agenda (first/outermost param); BVar(0) is ops (last param).
    // Using BVar(1) for agenda (not a captured literal) ensures handle_input sees
    // the live agenda at dequeue time, not a stale snapshot from enqueue time.
    inner_args[0] = Term::bvar(1);      // agenda: BVar(1) → first param → agenda_term
    inner_args[1] = Term::integer(fd);  // fd identifier (captured literal)
    inner_args[2] = input_term;         // char-list or nil / EOF (captured literal)
    inner_args[3] = Term::bvar(0);      // ops: BVar(0) → last param → Var(1)

    // Build the inner call goal: (call handle_input agenda-snap fd input BVar(1))
    Goal* body = wrap_arena.make<Goal>();
    if (!body) {
        std::fprintf(stderr, "raprunner: OOM building wrapper body\n");
        return;
    }
    body->tag  = GoalTag::Call;
    body->call = GoalCall{handle_input_rel, inner_args, 4};

    // Build the 2-param wrapper RelNode: (rel (agenda ops) body).
    // param_count=2 satisfies enqueue's {2,3} check.
    // Enqueued with nil args; run_one passes [agenda_term, Var(1)] per the
    // arity-2 calling convention, making BVar(0) resolve to Var(1) (ops) and
    // BVar(1) resolve to agenda_term.
    RelNode* wrapper = wrap_arena.make<RelNode>();
    if (!wrapper) {
        std::fprintf(stderr, "raprunner: OOM building wrapper RelNode\n");
        return;
    }
    wrapper->param_count = 2;
    wrapper->body        = body;

    Term wrapper_term = Term::relation(wrapper);
    if (!agenda.enqueue(wrapper_term, Term::nil())) {
        std::fprintf(stderr, "raprunner: agenda OOM enqueuing handle_input\n");
    }

    (void)rel_env;
    (void)intern_arena;
    (void)sess_intern;
    (void)spine;
}


// ============================================================================
// load_program: parse defrel forms from a file into prog_arena / rel_env
// ============================================================================
static bool load_program(const char*  path,
                         Arena&       prog_arena,
                         Arena&       intern_arena,
                         Intern&      sess_intern,
                         RelEnv&      rel_env,
                         bool         verbose = false) {
    FILE* f = std::fopen(path, "r");
    if (!f) {
        std::fprintf(stderr, "raprunner: cannot open '%s': %s\n",
                     path, std::strerror(errno));
        return false;
    }

    // Read entire file into a temporary heap buffer.
    std::fseek(f, 0, SEEK_END);
    long fsize = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (fsize <= 0) {
        std::fprintf(stderr, "raprunner: program file '%s' is empty\n", path);
        std::fclose(f);
        return false;
    }

    std::vector<char> src(static_cast<std::size_t>(fsize) + 1, '\0');
    if (std::fread(src.data(), 1, static_cast<std::size_t>(fsize), f)
            != static_cast<std::size_t>(fsize)) {
        std::fprintf(stderr, "raprunner: error reading '%s'\n", path);
        std::fclose(f);
        return false;
    }
    std::fclose(f);

    // Parse into prog_arena (temporaries) and intern_arena (SymEntries).
    ParsedQuery pq = parse_query(prog_arena, intern_arena, src.data(), sess_intern, rel_env);

    if (verbose) {
        print_arena_usage("prog_arena (parse)",  prog_arena);
        print_arena_usage("intern_arena",         intern_arena);
    }

    if (pq.goal != nullptr) {
        std::fprintf(stderr, "raprunner: program file must contain only defrel forms\n");
        return false;
    }
    if (!pq.rel_env.head) {
        std::fprintf(stderr, "raprunner: no relations defined in '%s'\n", path);
        return false;
    }

    // Merge new defrels into the session rel_env.
    for (const RelEnvEntry* e = pq.rel_env.head; e; e = e->next) {
        const Goal* body_copy = deep_copy_goal(prog_arena, e->rel_term.rel->body);
        RelNode* rn = prog_arena.make<RelNode>();
        if (!rn) {
            std::fprintf(stderr, "raprunner: OOM in prog_arena\n");
            return false;
        }
        rn->param_count = e->rel_term.rel->param_count;
        rn->body        = body_copy;
        rel_env.define(prog_arena, e->name, Term::relation(rn));
        if (verbose)
            print_rel_body(e->name->str, rn->param_count, rn->body);
    }

    return true;
}


// ============================================================================
// load_stdlib: load stdlib/core.rap from $RAP_STDLIB or relative to cwd
// ============================================================================
static void load_stdlib(Arena& prog_arena, Arena& intern_arena, Intern& intern,
                        RelEnv& rel_env, bool verbose = false) {
    const char* env_path = std::getenv("RAP_STDLIB");
    std::vector<std::string> candidates;
    if (env_path) candidates.push_back(env_path);
    candidates.push_back("stdlib/core.rap");

    for (const auto& path : candidates) {
        FILE* f = std::fopen(path.c_str(), "r");
        if (!f) continue;
        std::fclose(f);
        if (!load_program(path.c_str(), prog_arena, intern_arena, intern, rel_env, verbose))
            std::fprintf(stderr, "raprunner: warning: failed to load stdlib '%s'\n",
                         path.c_str());
        return;
    }
    std::fprintf(stderr, "raprunner: warning: stdlib/core.rap not found; "
                         "standard relations unavailable\n");
}


// ============================================================================
// main
// ============================================================================
int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: raprunner <program-file> [args...] [--fd N]...\n");
        return 1;
    }

    // ---- Parse command line ------------------------------------------------
    const char* program_file = argv[1];
    bool verbose = false;
    std::vector<const char*> user_args;
    std::vector<int> extra_fds;

    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (std::strcmp(argv[i], "--fd") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "raprunner: --fd requires an argument\n");
                return 1;
            }
            ++i;
            int fd = std::atoi(argv[i]);
            extra_fds.push_back(fd);
        } else {
            user_args.push_back(argv[i]);
        }
    }

    // ---- Initialize arenas -------------------------------------------------
    Arena prog_arena(prog_buf, sizeof(prog_buf));
    Arena intern_arena(intern_buf, sizeof(intern_buf));
    Arena eval_arena(eval_buf, sizeof(eval_buf));
    Arena rap_arena(rap_buf, sizeof(rap_buf));
    Arena wrap_arena(wrap_buf, sizeof(wrap_buf));

    // ---- Initialize intern -------------------------------------------------
    Intern intern{0, nullptr};
    if (!intern_init(intern_arena, intern, 256)) {
        std::fprintf(stderr, "raprunner: intern_init failed\n");
        return 1;
    }

    OutcomeSyms syms{};
    syms.s_true         = intern_cstr(intern_arena, intern, "true");
    syms.s_false        = intern_cstr(intern_arena, intern, "false");
    syms.s_insufficient = intern_cstr(intern_arena, intern, "insufficient");
    syms.s_bounded      = intern_cstr(intern_arena, intern, "bounded");
    if (!syms.s_true || !syms.s_false ||
        !syms.s_insufficient || !syms.s_bounded) {
        std::fprintf(stderr, "raprunner: failed to intern outcome symbols\n");
        return 1;
    }

    // ---- Initialize RAP evaluator ------------------------------------------
    // Use placement new so the evaluator can hold a stable &eval_arena pointer.
    alignas(alignof(RapEvaluator)) std::uint8_t evaluator_buf[sizeof(RapEvaluator)];
    RapEvaluator* evaluator =
        new (evaluator_buf) RapEvaluator(&eval_arena, &rap_arena, &intern, &syms);

    // ---- Load stdlib then program file -------------------------------------
    RelEnv rel_env{};
    load_stdlib(prog_arena, intern_arena, intern, rel_env, verbose);
    if (!load_program(program_file, prog_arena, intern_arena, intern, rel_env, verbose)) return 1;
    if (verbose) {
        print_arena_usage("prog_arena",   prog_arena);
        print_arena_usage("intern_arena", intern_arena);
        print_arena_usage("rap_arena",    rap_arena);
    }

    // ---- Check entry points ------------------------------------------------
    const SymEntry* main_sym =
        intern_cstr(intern_arena, intern, "main");
    const SymEntry* hi_sym =
        intern_cstr(intern_arena, intern, "handle_input");

    Term main_rel = rel_env.lookup(main_sym);
    if (main_rel.tag != TermTag::Rel) {
        std::fprintf(stderr, "raprunner: 'main' not defined in '%s'\n", program_file);
        return 1;
    }
    if (main_rel.rel->param_count != 2) {
        std::fprintf(stderr, "raprunner: 'main' must have arity 2 (args ops)\n");
        return 1;
    }

    Term handle_input_rel = rel_env.lookup(hi_sym);
    if (handle_input_rel.tag != TermTag::Rel) {
        std::fprintf(stderr, "raprunner: 'handle_input' not defined in '%s'\n",
                     program_file);
        return 1;
    }
    if (handle_input_rel.rel->param_count != 4) {
        std::fprintf(stderr,
                     "raprunner: 'handle_input' must have arity 4 "
                     "(agenda fd input ops)\n");
        return 1;
    }

    // ---- Build agenda infrastructure ----------------------------------------
    Agenda    agenda{};
    SpineArena spine{};

    // ---- Build CLI args term ------------------------------------------------
    Term args_term = build_args_term(intern_arena, intern, user_args);

    // ---- Call main ---------------------------------------------------------
    if (!call_main(*evaluator, main_rel, args_term, agenda, rel_env,
                   eval_arena, intern_arena, verbose)) {
        return 1;
    }

    // ---- Reactive loop -----------------------------------------------------
    // Watched fds: start with stdin (0), add extra fds from --fd.
    std::vector<int> watched_fds;
    watched_fds.push_back(0);  // stdin
    for (int fd : extra_fds) watched_fds.push_back(fd);

    while (true) {
        // Run agenda until empty — every entry is runnable.
        if (!agenda.empty()) {
            run_one(*evaluator, agenda, spine, rel_env,
                    eval_arena, intern_arena, verbose);
        } else {
            // Agenda empty. Reset wrap_arena — all wrappers have executed.
            wrap_arena.reset();
            // Exit if no fds remain.
            if (watched_fds.empty()) break;
        }

        // Poll watched fds for input.
        std::vector<struct pollfd> pfds;
        pfds.reserve(watched_fds.size());
        for (int fd : watched_fds) {
            struct pollfd pfd{};
            pfd.fd     = fd;
            pfd.events = POLLIN;
            pfds.push_back(pfd);
        }

        int ret = poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 1);
        if (ret < 0) {
            std::fprintf(stderr, "raprunner: poll error: %s\n",
                         std::strerror(errno));
            return 1;
        }

        // Process poll results.
        std::vector<int> fds_to_remove;
        for (std::size_t i = 0; i < pfds.size(); ++i) {
            int   fd      = pfds[i].fd;
            short revents = pfds[i].revents;

            if (revents & POLLIN) {
                char block[4096];
                ssize_t n = read(fd, block, sizeof(block));
                if (n == 0) {
                    // EOF: send empty list as input, then drop fd.
                    enqueue_handle_input(wrap_arena, agenda, spine, rel_env,
                                        intern_arena, intern,
                                        handle_input_rel, fd, Term::nil());
                    fds_to_remove.push_back(fd);
                } else if (n > 0) {
                    Term input_term = build_char_list(wrap_arena, intern_arena,
                                                     intern, block, n);
                    enqueue_handle_input(wrap_arena, agenda, spine, rel_env,
                                        intern_arena, intern,
                                        handle_input_rel, fd, input_term);
                }
                // n < 0: read error — ignore for now (poll will report POLLERR next)
            } else if (revents & (POLLHUP | POLLERR | POLLNVAL)) {
                // Hung up, errored, or invalid fd (macOS returns POLLNVAL for
                // /dev/null and other non-pollable fds). Treat as EOF.
                enqueue_handle_input(wrap_arena, agenda, spine, rel_env,
                                    intern_arena, intern,
                                    handle_input_rel, fd, Term::nil());
                fds_to_remove.push_back(fd);
            }
        }

        // Remove EOF/error fds from watch set.
        for (int fd : fds_to_remove) {
            auto it = std::find(watched_fds.begin(), watched_fds.end(), fd);
            if (it != watched_fds.end()) watched_fds.erase(it);
        }
    }

    return 0;
}
