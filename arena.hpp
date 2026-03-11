#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

// ============================================================================
// Arena: injected-buffer bump allocator (arena-only; trivially destructible)
// ============================================================================
struct Arena {
  std::byte* base = nullptr;
  std::byte* cur  = nullptr;
  std::byte* end  = nullptr;

  Arena(void* mem, std::size_t bytes) {
    base = static_cast<std::byte*>(mem);
    cur  = base;
    end  = base + bytes;
  }

  void reset() { cur = base; }

  void* alloc(std::size_t bytes, std::size_t align) {
    std::uintptr_t p = reinterpret_cast<std::uintptr_t>(cur);
    std::uintptr_t aligned = (p + (align - 1)) & ~(align - 1);
    std::byte* out = reinterpret_cast<std::byte*>(aligned);
    if (out + bytes > end) return nullptr;
    cur = out + bytes;
    return out;
  }

  template<class T>
  T* make() {
    static_assert(std::is_trivially_destructible_v<T>,
                  "arena objects must be trivially destructible");
    void* p = alloc(sizeof(T), alignof(T));
    return p ? static_cast<T*>(p) : nullptr;
  }
};
