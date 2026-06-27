// rap/agenda.hpp — Stage 2: Queue 2 (Agenda), QueryEntry
#pragma once
#include "../core/mktypes.hpp"
#include "../core/core.hpp"
#include "changeset.hpp"

constexpr std::uint32_t QUEUE2_ARENA_SIZE = 64 * 1024;  // 64 KiB, paper-scale

// ============================================================================
// QueryEntry: header stored at the start of each slot in the agenda buffer.
// The term data (PairNodes) follows immediately in memory after the header.
// ============================================================================
struct QueryEntry {
    std::uint32_t id;          // stable ID for Remove ops
    std::uint32_t byte_size;   // total bytes this entry occupies (header + term data)
    Term          query_term;  // the query to execute (Pair nodes in this buffer)
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

    // Enqueue a query term. Deep-copies into this buffer.
    // Returns assigned query_id, or 0 on OOM.
    std::uint32_t enqueue(Term query_term) {
        if (count == 0) { head = 0; tail = 0; }  // reset on empty

        std::uint32_t avail = available_from_head();
        if (avail < sizeof(QueryEntry) + 64) return 0;  // conservative OOM guard

        std::uint8_t* entry_base = buf + head;
        Arena sub(entry_base + sizeof(QueryEntry),
                  avail - static_cast<std::uint32_t>(sizeof(QueryEntry)));
        Term copied = deep_copy_term(sub, query_term);

        // Bytes used by the deep copy (arena cursor advanced).
        std::uint32_t term_bytes = static_cast<std::uint32_t>(sub.cur - sub.base);
        std::uint32_t total = static_cast<std::uint32_t>(sizeof(QueryEntry)) + term_bytes;

        // Non-wrapping OOM check.
        // Wraparound not supported: if head + total would exceed QUEUE2_ARENA_SIZE,
        // return OOM. Keeps buffer linear (head >= tail) — required for
        // pointer-rewriting compaction in remove(). Future work: amortized growth.
        if (head + total > QUEUE2_ARENA_SIZE) return 0;

        QueryEntry* entry  = reinterpret_cast<QueryEntry*>(entry_base);
        entry->id          = next_id;
        entry->byte_size   = total;
        entry->query_term  = copied;

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

                    // IMPORTANT: save id and byte_size BEFORE deep_copy_term.
                    // The copy may write new PairNodes that overlap src's header
                    // region (when write_pos is close to read_pos), corrupting
                    // src->id and src->byte_size if read after the copy.
                    std::uint32_t src_id        = src->id;
                    std::uint32_t src_byte_size = src->byte_size;
                    Term          src_query     = src->query_term;

                    std::uint8_t* dst_base = buf + write_pos;
                    std::uint32_t dst_avail = QUEUE2_ARENA_SIZE - write_pos
                                            - static_cast<std::uint32_t>(sizeof(QueryEntry));
                    Arena sub(dst_base + sizeof(QueryEntry), dst_avail);
                    Term copied = deep_copy_term(sub, src_query);

                    std::uint32_t term_bytes = static_cast<std::uint32_t>(sub.cur - sub.base);
                    std::uint32_t new_size   = static_cast<std::uint32_t>(sizeof(QueryEntry))
                                            + term_bytes;

                    QueryEntry* dst = reinterpret_cast<QueryEntry*>(dst_base);
                    dst->id         = src_id;
                    dst->byte_size  = new_size;
                    dst->query_term = copied;

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

    // Remove by structural value (for non-Int terms like (wc-state 0 0 0 0)).
    static bool terms_equal(Term a, Term b) {
        if (a.tag != b.tag) return false;
        switch (a.tag) {
            case TermTag::Int:  return a.value == b.value;
            case TermTag::Nil:  return true;
            case TermTag::Sym:  return a.sym == b.sym;
            case TermTag::Var:  return a.id == b.id;
            case TermTag::BVar: return a.id == b.id;
            case TermTag::Rel:  return a.rel == b.rel;
            case TermTag::Pair:
                if (!a.pair || !b.pair) return !a.pair && !b.pair;
                return terms_equal(a.pair->car, b.pair->car) &&
                       terms_equal(a.pair->cdr, b.pair->cdr);
            default: return false;
        }
    }

    bool remove_by_term(Term t) {
        std::uint32_t pos = tail;
        for (std::uint32_t i = 0; i < count; ++i) {
            const QueryEntry* e = reinterpret_cast<const QueryEntry*>(buf + pos);
            if (terms_equal(e->query_term, t))
                return remove(e->id);
            pos += e->byte_size;
        }
        return false;
    }

    // Returns true if any Rel-tagged item is present (data items don't count).
    bool has_rel() const {
        std::uint32_t pos = tail;
        for (std::uint32_t i = 0; i < count; ++i) {
            const QueryEntry* e = reinterpret_cast<const QueryEntry*>(buf + pos);
            if (e->query_term.tag == TermTag::Rel) return true;
            pos += e->byte_size;
        }
        return false;
    }

    // Dequeue the first Rel-tagged item, skipping data items.
    bool dequeue_rel(QueryEntry& out) {
        if (count == 0) return false;
        std::uint32_t pos = tail;
        for (std::uint32_t i = 0; i < count; ++i) {
            const QueryEntry* e = reinterpret_cast<const QueryEntry*>(buf + pos);
            if (e->query_term.tag == TermTag::Rel) {
                std::uint32_t saved_id   = e->id;
                Term          saved_term = e->query_term;
                remove(saved_id);
                out.id         = saved_id;
                out.byte_size  = 0;
                out.query_term = saved_term;
                return true;
            }
            pos += e->byte_size;
        }
        return false;
    }

    // Build the agenda as a linked-list term for passing to the next query.
    // Uses spine_arena for the Pair nodes (reset after query starts).
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
        // Build list back-to-front.
        Term result = Term::nil();
        for (std::int32_t i = static_cast<std::int32_t>(n) - 1; i >= 0; --i) {
            PairNode* p = spine_arena.make<PairNode>();
            if (!p) break;
            p->car = entries[i]->query_term;
            p->cdr = result;
            result = Term::make_pair(p);
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
