// rap/loop.hpp — Stage 2: Reactive execution loop
#pragma once
#include "../core/mktypes.hpp"
#include "../core/core.hpp"
#include "../core/sexp_parser.hpp"
#include "rap.hpp"
#include "agenda.hpp"
#include "spine.hpp"
#include "changeset.hpp"
#include <cstdio>
#include <memory>
#include <new>

constexpr std::uint32_t MAX_OUTPUT_TERMS = 256;
constexpr std::uint32_t EVAL_ARENA_SIZE  = 1024 * 1024 * 1024;  

// ============================================================================
// OutputQueue: collects Output ops during a query run.
// ============================================================================
struct OutputQueue {
    Term          terms[MAX_OUTPUT_TERMS];
    std::uint32_t count = 0;

    bool push(Term t) {
        if (count >= MAX_OUTPUT_TERMS) return false;
        terms[count++] = t;
        return true;
    }
    void reset() { count = 0; }
};
static_assert(std::is_trivially_destructible_v<OutputQueue>);

// ============================================================================
// RapLoop: reactive execution loop over an agenda of pending queries.
//
// The evaluator uses eval_arena for per-query working memory (reset each
// query). All long-lived data (intern table, relation definitions, output
// terms) lives in intern_arena which is never reset.
// ============================================================================
struct RapLoop {
    // Long-lived arenas (persist across queries).
    alignas(64) std::uint8_t intern_buf[32 * 1024];
    Arena       intern_arena;
    Intern      intern;
    OutcomeSyms syms;

    // Per-query arena (reset between queries).
    // Heap-allocated: EVAL_ARENA_SIZE can be very large (currently 1 GiB), so
    // an inline member array would overflow the stack when RapLoop is on the stack.
    std::unique_ptr<std::uint8_t[]> eval_buf;
    Arena       eval_arena;

    // Permanent evaluator arena: holds the RapEvaluator's client region and
    // interned "no-ops"/"cons-ops"/"empty-ops" SymEntries.  These must survive
    // across eval_arena resets, so they live in a separate never-reset buffer.
    alignas(64) std::uint8_t rap_buf[MAX_CHANGESET_ARENA + 32 * 1024];
    Arena       rap_arena;

    // Queues.
    Agenda      agenda;
    SpineArena  spine;
    OutputQueue output;

    // Relation environment (populated by load_defs).
    RelEnv rel_env;

    // The evaluator is constructed in-place using placement new so it can
    // hold a stable pointer to eval_arena (which is a member of this struct).
    alignas(alignof(RapEvaluator)) std::uint8_t evaluator_buf[sizeof(RapEvaluator)];
    RapEvaluator* evaluator = nullptr;

    RapLoop()
        : intern_arena(intern_buf, sizeof(intern_buf))
        , eval_buf(std::make_unique<std::uint8_t[]>(EVAL_ARENA_SIZE))
        , eval_arena(eval_buf.get(), EVAL_ARENA_SIZE)
        , rap_arena(rap_buf, sizeof(rap_buf))
    {}

    // Initialize. Must be called before any other method.
    // Returns false on OOM.
    bool init() {
        if (!intern_init(intern_arena, intern, 256)) return false;

        syms.s_true         = intern_cstr(intern_arena, intern, "true");
        syms.s_false        = intern_cstr(intern_arena, intern, "false");
        syms.s_insufficient = intern_cstr(intern_arena, intern, "insufficient");
        syms.s_bounded      = intern_cstr(intern_arena, intern, "bounded");
        if (!syms.s_true || !syms.s_false ||
            !syms.s_insufficient || !syms.s_bounded) return false;

        // Construct RapEvaluator: eval_arena is the working arena (reset between
        // queries); rap_arena is the permanent arena (never reset) that holds
        // the interned "no-ops"/"cons-ops" SymEntries and the 16 KiB client-
        // region buffer so they survive eval_arena.reset() calls in run_one().
        evaluator = new (evaluator_buf) RapEvaluator(&eval_arena, &rap_arena, &intern, &syms);
        return evaluator != nullptr;
    }

    // Parse and register defrel forms from src using our own intern table.
    // All compiled symbol pointers come from this RapLoop's intern, so
    // agenda terms built with the same intern will match at runtime.
    // Returns true if at least one defrel was parsed successfully.
    bool load_defs(const char* src) {
        Lexer lx{src};
        Token tok = lx.next();
        bool any = false;

        while (tok.tag != TokTag::End && tok.tag != TokTag::Error) {
            const Sexp* top = parse_sexp(intern_arena, intern_arena, intern, lx, tok);
            if (!top) break;

            // Only process defrel forms; skip anything else.
            if (top->tag != SexpTag::List || !top->list.head) continue;
            const Sexp* head_sexp = top->list.head->car;
            if (!head_sexp || head_sexp->tag != SexpTag::Sym) continue;
            if (!sym_lit_eq(head_sexp->sym, "defrel")) continue;

            // (defrel (name param1 ...) body)
            const SexpCell* dc = top->list.head->cdr;
            if (!dc || !dc->car || dc->car->tag != SexpTag::List) continue;

            const SexpCell* name_and_params = dc->car->list.head;
            if (!name_and_params || !name_and_params->car ||
                name_and_params->car->tag != SexpTag::Sym) continue;

            const SymEntry* rel_name   = name_and_params->car->sym;
            const SexpCell* param_list = name_and_params->cdr;

            // Collect parameter names.
            const SymEntry* param_names[64];
            std::uint32_t   param_count = 0;
            bool param_ok = true;
            for (const SexpCell* p = param_list; p; p = p->cdr) {
                if (!p->car || p->car->tag != SexpTag::Sym ||
                    param_count >= 64) { param_ok = false; break; }
                param_names[param_count++] = p->car->sym;
            }
            if (!param_ok) continue;

            // Body sexp.
            const SexpCell* body_cell = dc->cdr;
            if (!body_cell || !body_cell->car) continue;

            // Build closed benv for parameters (bound_push prepends, so the
            // first parameter ends up at the deepest depth; this matches how
            // compile_goal resolves bound variables).
            const BoundBind* rel_benv = nullptr;
            for (std::uint32_t i = 0; i < param_count; ++i) {
                rel_benv = bound_push(intern_arena, rel_benv, param_names[i]);
                if (!rel_benv) { param_ok = false; break; }
            }
            if (!param_ok) continue;

            // Compile body with empty genv (defrel is top-level).
            const Goal* body = compile_goal(intern_arena, nullptr, rel_benv,
                                            body_cell->car);
            if (!body) continue;

            RelNode* rn = intern_arena.make<RelNode>();
            if (!rn) continue;
            rn->param_count = param_count;
            rn->body        = body;

            rel_env.define(intern_arena, rel_name, Term::relation(rn));
            any = true;
        }
        return any;
    }

    // Enqueue a query by relation name. Returns assigned query_id, or 0.
    std::uint32_t enqueue_query(const char* rel_name) {
        const SymEntry* name_sym = intern_cstr(intern_arena, intern, rel_name);
        if (!name_sym) return 0;
        Term rel_term = rel_env.lookup(name_sym);
        if (rel_term.tag != TermTag::Rel) return 0;
        return agenda.enqueue(rel_term, Term::nil());
    }

    // Enqueue a query from a raw Rel term. Returns assigned query_id, or 0.
    std::uint32_t enqueue_term(Term rel_term) {
        if (rel_term.tag != TermTag::Rel) return 0;
        return agenda.enqueue(rel_term, Term::nil());
    }

    // Run until no runnable entries remain.
    void run_until_empty(std::uint32_t max_steps = 10000) {
        for (std::uint32_t s = 0; s < max_steps && agenda.has_runnable(); ++s)
            run_one();
    }

    // Execute one runnable query from the agenda.
    void run_one() {
        if (!evaluator) return;
        QueryEntry entry;
        if (!agenda.dequeue_runnable(entry)) return;

        // Build agenda list term from remaining entries.
        spine.reset();
        Term agenda_term = agenda.as_term(spine.get());

        // Initialize ChangeSet in the client region (before runN so that
        // runN's initial State captures the post-header offset, preventing
        // backtracking from overwriting the ChangeSet header).
        evaluator->init_changeset();

        if (entry.query_term.tag == TermTag::Rel) {
            const RelNode* rel     = entry.query_term.rel;
            std::uint32_t  nparams = rel->param_count;

            // Allocate exactly nparams call args.
            Term* call_args = static_cast<Term*>(
                eval_arena.alloc(nparams * sizeof(Term), alignof(Term)));
            if (!call_args) goto apply;

            // Param 0 always = agenda_term.
            // Param 1 (if present): pass published state, OR Var(1) if nil
            // (nil means runnable — use unbound Var so cons-ops can bind it).
            call_args[0] = agenda_term;
            if (nparams >= 2)
                call_args[1] = (entry.args.tag == TermTag::Nil)
                              ? Term::var(1) : entry.args;

            {
                GoalCall gc;
                gc.rel_term  = entry.query_term;
                gc.args      = call_args;
                gc.arg_count = nparams;

                Goal* call_goal = eval_arena.make<Goal>();
                if (!call_goal) goto apply;
                call_goal->tag  = GoalTag::Call;
                call_goal->call = gc;

                // vars_used = 2 always: Var(0) = result var, Var(1) reserved for
                // wrapper ops (enqueue_handle_input inner call uses Term::var(1)).
                evaluator->runN(1, call_goal, Term::var(0), 2, rel_env,
                    [](Term, State) {});
            }
        }

    apply:
        // Extract ChangeSet and apply its ops.
        ChangeSet* cs = evaluator->get_changeset();
        if (cs)
            apply_changeset(*cs);

        // Reset per-query arena for the next query.
        eval_arena.reset();
    }

    // Print all collected output terms.
    void print_output() const {
        for (std::uint32_t i = 0; i < output.count; ++i) {
            print_term(output.terms[i]);
            std::printf("\n");
        }
    }

private:
    void apply_changeset(const ChangeSet& cs) {
        for (std::uint32_t i = 0; i < cs.op_count; ++i) {
            const Op& op = cs.ops[i];
            switch (op.tag) {
                case OpTag::Add:
                    agenda.enqueue(op.add.rel_term, op.add.args);
                    break;
                case OpTag::Remove:
                    agenda.remove(op.query_id);
                    break;
                case OpTag::Output: {
                    // Deep-copy into intern_arena (stable, never reset) so the
                    // Term remains valid after eval_arena is reset below.
                    Term stable = deep_copy_term(intern_arena, op.output_term);
                    output.push(stable);
                    std::printf("[output] ");
                    print_term(stable);
                    std::printf("\n");
                    break;
                }
            }
        }
    }
};
