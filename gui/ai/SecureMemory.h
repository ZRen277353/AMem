#pragma once

#ifdef HAVE_AI_CHAT

#include <cstddef>
#include <string>

namespace AI {

inline void secureClearMemory(void* memory, size_t size) noexcept {
    volatile unsigned char* cursor =
        static_cast<volatile unsigned char*>(memory);
    while (size != 0) {
        *cursor++ = 0;
        --size;
    }
}

inline void secureClearString(std::string& value) noexcept {
    if (!value.empty()) {
        secureClearMemory(value.data(), value.size());
    }
    value.clear();
}

} // namespace AI

#endif // HAVE_AI_CHAT
