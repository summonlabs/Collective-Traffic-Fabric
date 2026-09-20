// Collective Traffic Fabric - hard resource bounds for every externally
// reachable surface.  Nothing in this runtime sizes an allocation, a loop, or a
// retained container from an externally supplied value without first being
// clamped or rejected against these limits.
// Copyright 2026 Summon Software Labs.
#ifndef CTF_LIMITS_HPP
#define CTF_LIMITS_HPP

#include <cstddef>
#include <cstdint>

namespace ctf::limits {

// ---- transport framing ----------------------------------------------------
inline constexpr std::uint32_t kProtocolMajor = 1;
inline constexpr std::uint32_t kProtocolMinor = 0;
inline constexpr std::uint32_t kFrameHeaderBytes = 32;
inline constexpr std::uint32_t kFramePayloadMaxBytes = 1u << 20;      // 1 MiB per frame
inline constexpr std::uint32_t kFramePayloadMinBytes = 0;
inline constexpr std::uint32_t kSessionWriteQueueFramesMax = 64;
inline constexpr std::uint32_t kSessionWriteQueueBytesMax = 4u << 20; // 4 MiB
inline constexpr std::uint32_t kIoBufferBytes = 64u << 10;            // 64 KiB read/write scratch

// ---- sessions and workers -------------------------------------------------
inline constexpr std::uint32_t kSessionsMax = 32;
inline constexpr std::uint32_t kSessionThreadsMax = 32;
inline constexpr std::uint32_t kWorkerThreadsMax = 8;
inline constexpr std::uint32_t kBacklogMax = 64;

// ---- domain collections ---------------------------------------------------
inline constexpr std::uint32_t kParticipantsMaxPerCollective = 4096;
inline constexpr std::uint32_t kEdgesMaxPerFlowGroup = 65536;
inline constexpr std::uint32_t kPhasesMaxPerCollective = 64;
inline constexpr std::uint32_t kStepsMaxPerPhase = 1024;
inline constexpr std::uint32_t kCollectivesMaxLive = 1000000;
inline constexpr std::uint32_t kPeersMax = 65536;
inline constexpr std::uint32_t kTopologyNodesMax = 65536;
inline constexpr std::uint32_t kTopologyLinksMax = 262144;
inline constexpr std::uint32_t kEvidenceRecordsMax = 65536;
inline constexpr std::uint32_t kMetadataEntriesMax = 16;
inline constexpr std::uint32_t kMetadataKeyBytesMax = 64;
inline constexpr std::uint32_t kMetadataValueBytesMax = 256;
inline constexpr std::uint32_t kReasonChainMax = 32;
inline constexpr std::uint32_t kReasonDetailBytesMax = 256;
inline constexpr std::uint32_t kLabelBytesMax = 128;
inline constexpr std::uint32_t kCollectiveSetMax = 65536;

// ---- retained history -----------------------------------------------------
inline constexpr std::uint32_t kDecisionHistoryDefaultCapacity = 4096;
inline constexpr std::uint32_t kDecisionHistoryCapacityMax = 1u << 20;
inline constexpr std::uint32_t kAuditHistoryDefaultCapacity = 4096;
inline constexpr std::uint32_t kAuditHistoryCapacityMax = 1u << 20;
inline constexpr std::uint32_t kInspectionPageMax = 4096;

// ---- persistence ----------------------------------------------------------
inline constexpr std::uint64_t kSnapshotBytesMax = 64ull << 20;  // 64 MiB
inline constexpr std::uint32_t kSnapshotCollectivesMax = 200000;
inline constexpr std::uint32_t kSnapshotPoliciesMax = 256;
inline constexpr std::uint32_t kSnapshotStringBytesMax = 4096;

// ---- protocol decode ------------------------------------------------------
inline constexpr std::uint32_t kDecodeDepthMax = 8;
inline constexpr std::uint32_t kDecodeItemsMax = 200000;

// ---- admission control ----------------------------------------------------
inline constexpr std::uint32_t kTokenBucketsMax = 4096;

}  // namespace ctf::limits

#endif  // CTF_LIMITS_HPP
