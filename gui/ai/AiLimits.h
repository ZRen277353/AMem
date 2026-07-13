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

inline constexpr size_t kMaxToolJsonDepth = 32u;
inline constexpr size_t kMaxToolArgumentJsonNodes = 8192u;
inline constexpr size_t kMaxToolResultJsonNodes = 65536u;
inline constexpr size_t kMaxToolSchemaJsonNodes = 8192u;
inline constexpr size_t kMaxToolJsonContainerItems = 4096u;

inline constexpr size_t kMaxProviderJsonDepth = 64u;
inline constexpr size_t kMaxProviderJsonNodes = 100000u;
inline constexpr size_t kMaxProviderEventJsonNodes = 20000u;
inline constexpr size_t kMaxProviderJsonContainerItems = 20000u;

inline constexpr size_t kMaxPersistenceFileBytes = 32u * kMiB;
inline constexpr size_t kMaxPersistenceJsonDepth = 64u;
inline constexpr size_t kMaxPersistenceJsonNodes = 250000u;
inline constexpr size_t kMaxPersistenceContainerItems = 20000u;
inline constexpr size_t kMaxPersistenceStringBytes = 8u * kMiB;
inline constexpr size_t kMaxPersistenceTotalStringBytes = 24u * kMiB;
// DPAPI adds a small opaque header to the protected plaintext. Keep a hard
// outer-file bound while preserving the existing 32 MiB decrypted JSON limit.
inline constexpr size_t kMaxProtectedPersistenceFileBytes = 33u * kMiB;
inline constexpr size_t kMaxSessionPayloadBytes = 16u * kMiB;
inline constexpr size_t kMaxSessionMessagesOnDisk = 10000u;

inline bool wouldExceed(size_t current, size_t additional, size_t limit) {
    return current > limit || additional > limit - current;
}

} // namespace AI::Limits

#endif // HAVE_AI_CHAT
