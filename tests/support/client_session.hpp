// Collective Traffic Fabric - a small, strict protocol client used by the
// integration and multiprocess suites.
// Copyright 2026 Summon Software Labs.
//
// The client performs the same envelope discipline a real peer must: a strictly
// increasing frame sequence per session, the coordinator's session identity and
// boot incarnation on every frame, and the epoch the handshake reported.  Tests
// that want to violate that discipline use the raw connection instead of
// loosening this client.
#ifndef CTF_TESTS_CLIENT_SESSION_HPP
#define CTF_TESTS_CLIENT_SESSION_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "ctf/protocol.hpp"
#include "ctf/snapshot.hpp"
#include "ctf/transport.hpp"

namespace ctf::test {

class ClientSession {
 public:
  [[nodiscard]] Status connect(const std::string& host, std::uint16_t port, const std::string& label,
                              std::uint64_t now_ms);

  // Sends a request with the current envelope and waits for the matching
  // response.  An error response is returned as a Status carrying the wire
  // error code, so a test can assert on the deterministic code.
  [[nodiscard]] Status request(protocol::MessageKind kind, const std::vector<std::uint8_t>& payload,
                               protocol::MessageKind& response_kind,
                               std::vector<std::uint8_t>& response_payload);

  [[nodiscard]] Status heartbeat(ParticipantId participant, BootIncarnation incarnation, NodeId node,
                                 ParticipantState state);
  [[nodiscard]] Status register_collective(const CollectiveDefinition& definition,
                                           protocol::RegisterAckPayload& out);
  [[nodiscard]] Status begin_attempt(CollectiveId id, CollectiveGeneration generation,
                                     CollectiveAttemptId attempt, std::uint64_t transfer_bytes,
                                     protocol::AttemptAckPayload& out);
  [[nodiscard]] Status plan_flow_group(const DecisionRequest& request, DecisionRecord& out);
  [[nodiscard]] Status cancel(CollectiveId id, CollectiveGeneration generation, CollectiveAttemptId attempt,
                              protocol::LifecycleAckPayload& out);
  [[nodiscard]] Status retire(CollectiveId id, CollectiveGeneration generation, CollectiveAttemptId attempt,
                              protocol::LifecycleAckPayload& out);
  [[nodiscard]] Status inspect(std::uint8_t subject, CollectiveId id, std::uint32_t offset,
                               std::uint32_t limit, std::string& body);
  [[nodiscard]] Status explain(CollectiveId id, CollectiveAttemptId attempt, std::string& text);
  [[nodiscard]] Status ingest_capacity(const CapacityEvidence& evidence);
  [[nodiscard]] Status ingest_congestion(const CongestionEvidence& evidence);
  [[nodiscard]] Status ingest_topology(const TopologyEvidence& evidence);
  [[nodiscard]] Status shutdown_coordinator();

  [[nodiscard]] const protocol::HelloAckPayload& hello() const noexcept { return hello_; }
  [[nodiscard]] bool connected() const noexcept { return connected_; }
  void close();

  // Raw access for adversarial cases that must send malformed framing.
  [[nodiscard]] transport::Connection& connection() { return connection_; }
  [[nodiscard]] const transport::Connection& connection() const { return connection_; }

 private:
  [[nodiscard]] Status send(protocol::MessageKind kind, std::uint16_t flags,
                            const std::vector<std::uint8_t>& payload);

  transport::Connection connection_;
  protocol::HelloAckPayload hello_{};
  std::uint64_t sequence_ = 0;
  std::uint64_t correlation_ = 0;
  bool connected_ = false;
};

}  // namespace ctf::test

#endif  // CTF_TESTS_CLIENT_SESSION_HPP
