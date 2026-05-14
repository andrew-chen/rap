// rap/spine.hpp — Stage 2: short-lived Pair nodes for agenda list term
#pragma once
#include "../core/arena.hpp"

constexpr std::uint32_t SPINE_ARENA_SIZE = 4 * 1024;

struct SpineArena {
    std::uint8_t buf[SPINE_ARENA_SIZE];
    Arena        arena;

    SpineArena() : arena(buf, SPINE_ARENA_SIZE) {}

    Arena& get()  { return arena; }
    void   reset() { arena.reset(); }
};

static_assert(std::is_trivially_destructible_v<SpineArena>);
