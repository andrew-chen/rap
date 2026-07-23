// rap/agenda.hpp — Stage 2: Queue 2 (Agenda), QueryEntry
#pragma once
#include "../core/mktypes.hpp"
#include "../core/core.hpp"
#include "changeset.hpp"

constexpr std::uint32_t QUEUE2_ARENA_SIZE = 64 * 1024;  // 64 KiB, paper-scale

// ============================================================================
// QueryEntry: header stored at the start of each slot in the agenda buffer.
// The term data (PairNodes) follows immediately in memory after the header.
//
// Every agenda entry is runnable — there is no concept of a passive
// "state-holder" entry. This matches the paper's formal semantics.
//
// args: the compound data term carried by this entry (Term::nil() if the
// relation needs no state beyond the agenda snapshot). For arity-3 wrapper
// relations (agenda args ops), run_one passes this as the middle argument.
// For arity-2 wrapper relations (agenda ops), args is ignored by the caller
// but may still be inspected via find-by-contento in the agenda snapshot.
// ============================================================================
struct QueryEntry {
    std::uint32_t id;          // stable ID for Remove ops
    std::uint32_t byte_size;   // total bytes this entry occupies (header + term data)
    Term          query_term;  // must be a Rel with param_count 2 or 3
    Term          args;        // carried data; Term::nil() if none
};
static_assert(std::is_trivially_destructible_v<QueryEntry>);

// ============================================================================
// Agenda: non-wrapping linear FIFO buffer.
//
// head >= tail always (no wraparound).
// Wraparound returns OOM rather than wrapping; this invariant is required for
// safe pointer-rewriting compaction in remove().
// ============================================================================
struct Agenda {
    std::uint8_t  buf[QUEUE2_ARENA_SIZE];
    std::uint32_t head    = 0;  // next write position
    std::uint32_t tail    = 0;  // oldest unread entry
    std::uint32_t count   = 0;  // number of pending entries
    std::uint32_t next_id = 1;  // monotonically increasing

    bool empty() const { return count == 0; }

    // Enqueue a query. rel_term must be a Rel with param_count 2 or 3.
    //
    // Arity convention (deliberate design limit, not a temporary restriction):
    //   2 params (agenda ops)       — relation needs only the agenda snapshot.
    //   3 params (agenda args ops)  — relation also receives stored data as its
    //                                 middle argument; pass that data as `args`.
    // Anything needing more than one data value should pack those values into a
    // single compound term (list, alist, nested tuple) passed as `args` — not by
    // widening param_count beyond 3.  enqueue rejects param_count outside {2,3}.
    //
    // `args` is deep-copied into the agenda buffer alongside rel_term.
    // Pass Term::nil() when the relation needs no data beyond the agenda snapshot.
    // Returns assigned id, or 0 on rejection or OOM.
    std::uint32_t enqueue(Term rel_term, Term args) {
        if (rel_term.tag != TermTag::Rel || !rel_term.rel) return 0;
        std::uint32_t pc = rel_term.rel->param_count;
        if (pc < 2 || pc > 3) return 0;

        if (count == 0) { head = 0; tail = 0; }  // reset on empty

        std::uint32_t avail = available_from_head();
        if (avail < sizeof(QueryEntry) + 128) return 0;  // conservative OOM guard

        std::uint8_t* entry_base = buf + head;
        Arena sub(entry_base + sizeof(QueryEntry),
                  avail - static_cast<std::uint32_t>(sizeof(QueryEntry)));

        // Deep-copy both terms into the same sub-arena so they share byte_size.
        Term copied_rel  = deep_copy_term(sub, rel_term);
        Term copied_args = deep_copy_term(sub, args);

        // Bytes used by both deep copies combined.
        std::uint32_t term_bytes = static_cast<std::uint32_t>(sub.cur - sub.base);
        std::uint32_t total = static_cast<std::uint32_t>(sizeof(QueryEntry)) + term_bytes;

        // Non-wrapping OOM check.
        if (head + total > QUEUE2_ARENA_SIZE) return 0;

        QueryEntry* entry  = reinterpret_cast<QueryEntry*>(entry_base);
        entry->id          = next_id;
        entry->byte_size   = total;
        entry->query_term  = copied_rel;
        entry->args        = copied_args;

        head += total;
        count++;
        return next_id++;
    }

    // Dequeue front entry (FIFO). Returns false if empty.
    bool dequeue(QueryEntry& out) {
        if (count == 0) return false;
        QueryEntry* e = reinterpret_cast<QueryEntry*>(buf + tail);
        out    = *e;
        tail  += e->byte_size;
        count--;
        if (count == 0) { head = 0; tail = 0; }  // reset on empty
        return true;
    }

    // Every entry is runnable. has_runnable() is an alias for !empty() and is
    // provided only for sites that haven't been updated yet; prefer !empty().
    bool has_runnable() const { return !empty(); }

    // Remove a pending entry by ID.
    // Copies all subsequent entries forward using deep_copy_term (not memmove)
    // so that interior PairNode* pointers are rewritten to their new addresses.
    // Returns true if found and removed.
    //
    // Correctness: this buffer is non-wrapping (head >= tail always, wraparound
    // returns OOM). Compaction moves entries toward lower addresses (toward tail).
    // Processing left-to-right means each entry's destination is strictly before
    // its source — deep_copy_term's write cursor never reaches the source data
    // being read.
    bool remove(std::uint32_t id) {
        if (count == 0) return false;

        std::uint32_t pos = tail;
        for (std::uint32_t i = 0; i < count; ++i) {
            QueryEntry* e = reinterpret_cast<QueryEntry*>(buf + pos);
            if (e->id == id) {
                // Found. Copy all subsequent entries forward to fill the gap.
                std::uint32_t write_pos = pos;
                std::uint32_t read_pos  = pos + e->byte_size;

                while (read_pos < head) {
                    QueryEntry* src = reinterpret_cast<QueryEntry*>(buf + read_pos);

                    // Save header fields before deep_copy_term may overwrite src.
                    std::uint32_t src_id        = src->id;
                    std::uint32_t src_byte_size = src->byte_size;
                    Term          src_query     = src->query_term;
                    Term          src_args      = src->args;

                    std::uint8_t* dst_base  = buf + write_pos;
                    std::uint32_t dst_avail = QUEUE2_ARENA_SIZE - write_pos
                                           - static_cast<std::uint32_t>(sizeof(QueryEntry));
                    Arena sub(dst_base + sizeof(QueryEntry), dst_avail);

                    // Diagnostic: check overlap between dst sub-arena and src data.
                    {
                        std::uint8_t* src_data_start = buf + read_pos + sizeof(QueryEntry);
                        std::uint8_t* dst_data_start = dst_base + sizeof(QueryEntry);
                        std::uint8_t* dst_data_end   = dst_data_start + src_byte_size
                                                      - sizeof(QueryEntry);
                        if (dst_data_end > src_data_start) {
                            std::fprintf(stderr,
                                "[remove-OVERLAP] id=%u removed=%u: "
                                "dst=[%u,%u) src=[%u,%u) overlap=%u gap=%u srcbsz=%u\n",
                                src_id, id,
                                (unsigned)(dst_data_start - buf),
                                (unsigned)(dst_data_end - buf),
                                (unsigned)(src_data_start - buf),
                                (unsigned)(src_data_start + src_byte_size - sizeof(QueryEntry) - buf),
                                (unsigned)(dst_data_end - src_data_start),
                                (unsigned)(src_data_start - dst_data_start),
                                src_byte_size);
                            std::fflush(stderr);
                        }
                    }

                    // Scan src_args for Var BEFORE copy.
                    {
                        Term t = src_args;
                        // Quick check: walk pairs looking for Var
                        // (inline to avoid dependency on raprunner.cpp helpers)
                        std::function<bool(Term)> has_var = [&](Term u) -> bool {
                            if (u.tag == TermTag::Var) return true;
                            if (u.tag == TermTag::Pair && u.pair)
                                return has_var(u.pair->car) || has_var(u.pair->cdr);
                            return false;
                        };
                        if (has_var(t))
                            std::fprintf(stderr,
                                "[remove-PRE-VAR] id=%u has Var in args\n", src_id);
                    }

                    // Copy both terms together into the destination sub-arena.
                    Term copied_rel  = deep_copy_term(sub, src_query);
                    Term copied_args = deep_copy_term(sub, src_args);

                    // Scan copied_args for Var AFTER copy.
                    {
                        std::function<bool(Term)> has_var = [&](Term u) -> bool {
                            if (u.tag == TermTag::Var) return true;
                            if (u.tag == TermTag::Pair && u.pair)
                                return has_var(u.pair->car) || has_var(u.pair->cdr);
                            return false;
                        };
                        if (has_var(copied_args))
                            std::fprintf(stderr,
                                "[remove-POST-VAR] id=%u has Var in copied args\n", src_id);
                    }

                    std::uint32_t term_bytes = static_cast<std::uint32_t>(sub.cur - sub.base);
                    std::uint32_t new_size   = static_cast<std::uint32_t>(sizeof(QueryEntry))
                                            + term_bytes;

                    QueryEntry* dst = reinterpret_cast<QueryEntry*>(dst_base);
                    dst->id         = src_id;
                    dst->byte_size  = new_size;
                    dst->query_term = copied_rel;
                    dst->args       = copied_args;

                    write_pos += new_size;
                    read_pos  += src_byte_size;
                }

                head = write_pos;
                count--;
                return true;
            }
            pos += e->byte_size;
        }
        return false;  // not found
    }

    // Build the agenda as a linked-list term for passing to the next query.
    // Each element is (id rel-term args) — a proper 3-element list.
    // Uses spine_arena for Pair nodes.
    Term as_term(Arena& spine_arena) const {
        // Collect entry pointers (bounded by MAX_CHANGESET_OPS for paper scale).
        const QueryEntry* entries[MAX_CHANGESET_OPS];
        std::uint32_t n   = 0;
        std::uint32_t pos = tail;
        for (std::uint32_t i = 0; i < count && n < MAX_CHANGESET_OPS; ++i) {
            entries[n] = reinterpret_cast<const QueryEntry*>(buf + pos);
            pos += entries[n]->byte_size;
            ++n;
        }
        // Build list back-to-front; each element is (id . (rel-term . (args . ()))).
        Term result = Term::nil();
        for (std::int32_t i = static_cast<std::int32_t>(n) - 1; i >= 0; --i) {
            PairNode* p3 = spine_arena.make<PairNode>();
            PairNode* p2 = spine_arena.make<PairNode>();
            PairNode* p1 = spine_arena.make<PairNode>();
            PairNode* lp = spine_arena.make<PairNode>();
            if (!p3 || !p2 || !p1 || !lp) break;

            p3->car = entries[i]->args;
            p3->cdr = Term::nil();

            p2->car = entries[i]->query_term;
            p2->cdr = Term::make_pair(p3);

            p1->car = Term::integer(static_cast<std::int32_t>(entries[i]->id));
            p1->cdr = Term::make_pair(p2);

            lp->car = Term::make_pair(p1);
            lp->cdr = result;
            result  = Term::make_pair(lp);
        }
        return result;
    }

private:
    std::uint32_t available_from_head() const {
        if (count == 0) return QUEUE2_ARENA_SIZE;
        return QUEUE2_ARENA_SIZE - head;
    }
};

static_assert(std::is_trivially_destructible_v<Agenda>);
