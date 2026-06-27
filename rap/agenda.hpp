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
// args: published state for this entry, or Term::nil() if none.
// Entries with args == nil are "runnable" (dequeued by has_runnable /
// dequeue_runnable). Entries with non-nil args are state-holders that
// persist until explicitly removed.
// ============================================================================
struct QueryEntry {
    std::uint32_t id;          // stable ID for Remove ops
    std::uint32_t byte_size;   // total bytes this entry occupies (header + term data)
    Term          query_term;  // must be a Rel with param_count 1 or 2
    Term          args;        // published state; Term::nil() if none
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

    // Enqueue a query: rel_term must be a Rel with param_count 1 or 2.
    // args is published alongside it (Term::nil() if this entry should be
    // runnable; non-nil for state-holding entries that persist until removed).
    // Returns assigned id, or 0 on rejection or OOM.
    std::uint32_t enqueue(Term rel_term, Term args) {
        if (rel_term.tag != TermTag::Rel || !rel_term.rel) return 0;
        std::uint32_t pc = rel_term.rel->param_count;
        if (pc != 1 && pc != 2) return 0;

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

    // Returns true if any "runnable" entry exists: one with args == nil.
    // State-holding entries (non-nil args) are passive data and not runnable.
    bool has_runnable() const {
        std::uint32_t pos = tail;
        for (std::uint32_t i = 0; i < count; ++i) {
            const QueryEntry* e = reinterpret_cast<const QueryEntry*>(buf + pos);
            if (e->args.tag == TermTag::Nil)
                return true;
            pos += e->byte_size;
        }
        return false;
    }

    // Dequeue the first runnable entry (args == nil), skipping state-holders.
    bool dequeue_runnable(QueryEntry& out) {
        if (count == 0) return false;
        std::uint32_t pos = tail;
        for (std::uint32_t i = 0; i < count; ++i) {
            const QueryEntry* e = reinterpret_cast<const QueryEntry*>(buf + pos);
            if (e->args.tag == TermTag::Nil) {
                std::uint32_t saved_id   = e->id;
                Term          saved_term = e->query_term;
                Term          saved_args = e->args;
                remove(saved_id);
                out.id         = saved_id;
                out.byte_size  = 0;
                out.query_term = saved_term;
                out.args       = saved_args;
                return true;
            }
            pos += e->byte_size;
        }
        return false;
    }

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

                    // Copy both terms together into the destination sub-arena.
                    Term copied_rel  = deep_copy_term(sub, src_query);
                    Term copied_args = deep_copy_term(sub, src_args);

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
