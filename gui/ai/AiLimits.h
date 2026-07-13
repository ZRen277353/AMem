#pragma once
#ifdef HAVE_AI_CHAT

#include <cstddef>

namespace AI::Limits {

inline constexpr size_t kKiB = 1024u;
inline constexpr size_t kMiB = 1024u * kKiB;

inline constexpr size_t kMaxHttpResponseBytes = 16u * kMiB;
inline constexpr size_t kMaxSseLineBytes = 1u * kMiB;
inline constexpr size_t kMaxSseEventBytes = 2u * kMiB;

inline constexpr size_t kMaxAssistantContentBytes = 4u * kMiB;
inline constexpr size_t kMaxMessageContentBytes = 8u * kMiB;
inline constexpr size_t kMaxToolCallsPerMessage = 64u;
inline constexpr size_t kMaxToolCallIdBytes = 256u;
inline constexpr size_t kMaxToolCallNameBytes = 64u;
inline constexpr size_t kMaxToolArgumentsPerCallBytes = 512u * kKiB;
inline constexpr size_t kMaxToolArgumentsPerMessageBytes = 4u * kMiB;
inline constexpr size_t kMaxToolResultBytes = 4u * kMiB;

inline constexpr size_t kMaxPersistenceFileBytes = 32u * kMiB;
inline constexpr size_t kMaxSessionPayloadBytes = 16u * kMiB;
inline constexpr size_t kMaxSessionMessagesOnDisk = 10000u;

inline bool wouldExceed(size_t current, size_t additional, size_t limit) {
    return current > limit || additional > limit - current;
}

} // namespace AI::Limits

#endif // HAVE_AI_CHAT
