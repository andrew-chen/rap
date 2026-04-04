#pragma once
#include "arena.hpp"
#include <cstdint>

// ============================================================================
// Symbols: arena-local interning (raw bytes; pointer identity equality)
// ============================================================================
struct SymEntry {
  std::uint32_t hash;
  std::uint32_t len;
  const char* str;          // NUL-terminated in arena
  const SymEntry* next;     // chaining
};

struct Intern {
  std::uint32_t bucket_count;     // power of 2
  const SymEntry** buckets;       // array[bucket_count]
};

static_assert(std::is_trivially_destructible_v<SymEntry>);
static_assert(std::is_trivially_destructible_v<Intern>);

inline std::uint32_t fnv1a(const char* s, std::uint32_t len) {
  std::uint32_t h = 2166136261u;
  for (std::uint32_t i = 0; i < len; ++i) {
    h ^= static_cast<std::uint8_t>(s[i]);
    h *= 16777619u;
  }
  return h;
}

inline bool bytes_eq(const char* a, const char* b, std::uint32_t len) {
  for (std::uint32_t i = 0; i < len; ++i) if (a[i] != b[i]) return false;
  return true;
}

inline bool intern_init(Arena& a, Intern& in, std::uint32_t buckets_pow2) {
  if (buckets_pow2 == 0) return false;
  if ((buckets_pow2 & (buckets_pow2 - 1)) != 0) return false;

  auto* arr = static_cast<const SymEntry**>(
      a.alloc(sizeof(const SymEntry*) * buckets_pow2, alignof(const SymEntry*))
  );
  if (!arr) return false;
  for (std::uint32_t i = 0; i < buckets_pow2; ++i) arr[i] = nullptr;

  in.bucket_count = buckets_pow2;
  in.buckets = arr;
  return true;
}

inline const SymEntry* intern_sym(Arena& a, Intern& in, const char* s, std::uint32_t len) {
  if (!in.buckets || in.bucket_count == 0) return nullptr;

  std::uint32_t h = fnv1a(s, len);
  std::uint32_t idx = h & (in.bucket_count - 1);

  for (const SymEntry* e = in.buckets[idx]; e; e = e->next) {
    if (e->hash == h && e->len == len && bytes_eq(e->str, s, len)) return e;
  }

  char* copy = static_cast<char*>(a.alloc(len + 1, alignof(char)));
  if (!copy) return nullptr;
  for (std::uint32_t i = 0; i < len; ++i) copy[i] = s[i];
  copy[len] = '\0';

  SymEntry* ne = a.make<SymEntry>();
  if (!ne) return nullptr;
  *ne = SymEntry{h, len, copy, in.buckets[idx]};
  in.buckets[idx] = ne;
  return ne;
}

inline const SymEntry* intern_cstr(Arena& a, Intern& in, const char* s) {
  std::uint32_t len = 0;
  while (s[len] != '\0') ++len;
  return intern_sym(a, in, s, len);
}

inline bool sym_lit_eq(const SymEntry* s, const char* lit) {
  std::uint32_t len = 0;
  while (lit[len] != '\0') ++len;
  return s && s->len == len && bytes_eq(s->str, lit, len);
}
