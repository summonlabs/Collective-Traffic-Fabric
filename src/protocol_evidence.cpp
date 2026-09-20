// Collective Traffic Fabric - capacity, congestion and topology payload codecs.
// Copyright 2026 Summon Software Labs.
#include <algorithm>

#include "ctf/protocol.hpp"

namespace ctf::protocol {

Status encode_capacity(const CapacityEvidence& value, Writer& writer) {
  writer.identity(value.generation);
  writer.identity(value.topology_generation);
  writer.u64(value.captured_monotonic_ms);
  writer.boolean(value.synthetic);
  writer.u32(static_cast<std::uint32_t>(value.links.size()));
  for (const LinkCapacity& entry : value.links) {
    writer.identity(entry.link);
    writer.u64(entry.provisioned_bps);
    writer.u64(entry.available_bps);
    writer.u32(entry.utilization_bps);
    writer.boolean(entry.oversubscribed);
    writer.u32(entry.oversubscription_ratio_per_mille);
  }
  return Status::ok();
}

Status decode_capacity(Reader& reader, CapacityEvidence& out) {
  CapacityEvidence evidence;
  Status status = reader.identity(evidence.generation);
  if (!status.is_ok()) return status;
  status = reader.identity(evidence.topology_generation);
  if (!status.is_ok()) return status;
  status = reader.u64(evidence.captured_monotonic_ms);
  if (!status.is_ok()) return status;
  status = reader.boolean(evidence.synthetic);
  if (!status.is_ok()) return status;
  std::uint32_t links = 0;
  status = reader.count(links, limits::kTopologyLinksMax);
  if (!status.is_ok()) return status;
  evidence.links.resize(links);
  for (std::uint32_t index = 0; index < links; ++index) {
    LinkCapacity& entry = evidence.links[index];
    status = reader.identity(entry.link);
    if (!status.is_ok()) return status;
    status = reader.u64(entry.provisioned_bps);
    if (!status.is_ok()) return status;
    status = reader.u64(entry.available_bps);
    if (!status.is_ok()) return status;
    status = reader.u32(entry.utilization_bps);
    if (!status.is_ok()) return status;
    status = reader.boolean(entry.oversubscribed);
    if (!status.is_ok()) return status;
    status = reader.u32(entry.oversubscription_ratio_per_mille);
    if (!status.is_ok()) return status;
  }
  evidence.canonicalize();
  out = std::move(evidence);
  return Status::ok();
}

Status encode_congestion(const CongestionEvidence& value, Writer& writer) {
  writer.identity(value.generation);
  writer.identity(value.topology_generation);
  writer.u64(value.captured_monotonic_ms);
  writer.u64(value.valid_for_ms);
  writer.boolean(value.synthetic);
  writer.u32(static_cast<std::uint32_t>(value.links.size()));
  for (const LinkCongestion& entry : value.links) {
    writer.identity(entry.link);
    writer.u32(entry.utilization_bps);
    writer.u32(entry.queue_depth_bytes);
    writer.u32(entry.ecn_marks_per_mille);
    writer.u32(entry.pfc_pause_per_mille);
    writer.enumeration(entry.level);
  }
  return Status::ok();
}

Status decode_congestion(Reader& reader, CongestionEvidence& out) {
  CongestionEvidence evidence;
  Status status = reader.identity(evidence.generation);
  if (!status.is_ok()) return status;
  status = reader.identity(evidence.topology_generation);
  if (!status.is_ok()) return status;
  status = reader.u64(evidence.captured_monotonic_ms);
  if (!status.is_ok()) return status;
  status = reader.u64(evidence.valid_for_ms);
  if (!status.is_ok()) return status;
  status = reader.boolean(evidence.synthetic);
  if (!status.is_ok()) return status;
  std::uint32_t links = 0;
  status = reader.count(links, limits::kTopologyLinksMax);
  if (!status.is_ok()) return status;
  evidence.links.resize(links);
  for (std::uint32_t index = 0; index < links; ++index) {
    LinkCongestion& entry = evidence.links[index];
    status = reader.identity(entry.link);
    if (!status.is_ok()) return status;
    status = reader.u32(entry.utilization_bps);
    if (!status.is_ok()) return status;
    status = reader.u32(entry.queue_depth_bytes);
    if (!status.is_ok()) return status;
    status = reader.u32(entry.ecn_marks_per_mille);
    if (!status.is_ok()) return status;
    status = reader.u32(entry.pfc_pause_per_mille);
    if (!status.is_ok()) return status;
    std::uint8_t raw = 0;
    status = reader.u8(raw);
    if (!status.is_ok()) return status;
    if (raw > static_cast<std::uint8_t>(CongestionLevel::kSevere)) {
      return Status(ErrorCode::kDecodeInvalidEnum, "congestion level is not defined by this version");
    }
    entry.level = static_cast<CongestionLevel>(raw);
  }
  evidence.canonicalize();
  out = std::move(evidence);
  return Status::ok();
}

Status encode_topology(const TopologyEvidence& value, Writer& writer) {
  writer.identity(value.generation);
  writer.u64(value.captured_monotonic_ms);
  writer.boolean(value.synthetic);
  writer.u32(static_cast<std::uint32_t>(value.nodes.size()));
  for (const TopologyNode& node : value.nodes) {
    writer.identity(node.id);
    writer.identity(node.rack);
    writer.boolean(node.accelerator_attached);
    writer.u32(node.accelerator_count);
  }
  writer.u32(static_cast<std::uint32_t>(value.links.size()));
  for (const TopologyLink& link : value.links) {
    writer.identity(link.id);
    writer.identity(link.a);
    writer.identity(link.b);
    writer.enumeration(link.link_class);
    writer.u64(link.capacity_bps);
    writer.u32(link.lanes);
    writer.boolean(link.shared_oversubscribed);
  }
  return Status::ok();
}

Status decode_topology(Reader& reader, TopologyEvidence& out) {
  TopologyEvidence topology;
  Status status = reader.identity(topology.generation);
  if (!status.is_ok()) return status;
  status = reader.u64(topology.captured_monotonic_ms);
  if (!status.is_ok()) return status;
  status = reader.boolean(topology.synthetic);
  if (!status.is_ok()) return status;
  std::uint32_t nodes = 0;
  status = reader.count(nodes, limits::kTopologyNodesMax);
  if (!status.is_ok()) return status;
  topology.nodes.resize(nodes);
  for (std::uint32_t index = 0; index < nodes; ++index) {
    TopologyNode& node = topology.nodes[index];
    status = reader.identity(node.id);
    if (!status.is_ok()) return status;
    status = reader.identity(node.rack);
    if (!status.is_ok()) return status;
    status = reader.boolean(node.accelerator_attached);
    if (!status.is_ok()) return status;
    status = reader.u32(node.accelerator_count);
    if (!status.is_ok()) return status;
  }
  std::uint32_t links = 0;
  status = reader.count(links, limits::kTopologyLinksMax);
  if (!status.is_ok()) return status;
  topology.links.resize(links);
  for (std::uint32_t index = 0; index < links; ++index) {
    TopologyLink& link = topology.links[index];
    status = reader.identity(link.id);
    if (!status.is_ok()) return status;
    status = reader.identity(link.a);
    if (!status.is_ok()) return status;
    status = reader.identity(link.b);
    if (!status.is_ok()) return status;
    std::uint8_t raw = 0;
    status = reader.u8(raw);
    if (!status.is_ok()) return status;
    if (raw > static_cast<std::uint8_t>(LinkClass::kOversubscribed)) {
      return Status(ErrorCode::kDecodeInvalidEnum, "link class is not defined by this version");
    }
    link.link_class = static_cast<LinkClass>(raw);
    status = reader.u64(link.capacity_bps);
    if (!status.is_ok()) return status;
    status = reader.u32(link.lanes);
    if (!status.is_ok()) return status;
    status = reader.boolean(link.shared_oversubscribed);
    if (!status.is_ok()) return status;
  }
  topology.canonicalize();
  out = std::move(topology);
  return Status::ok();
}

Status encode_traffic_policy(const TrafficPolicy& value, Writer& writer) {
  writer.identity(value.generation);
  writer.u64(value.default_maximum_bandwidth_bps);
  writer.u64(value.burst_multiplier_per_mille);
  for (std::size_t index = 0; index < CongestionResponseTable::kBucketCount; ++index) {
    writer.u32(value.congestion_response.utilization_bps[index]);
    writer.u32(value.congestion_response.rate_numerator_per_mille[index]);
  }
  writer.u32(static_cast<std::uint32_t>(value.class_policies.size()));
  for (const TrafficClassPolicy& entry : value.class_policies) {
    writer.enumeration(entry.traffic_class);
    writer.u64(entry.priority);
    writer.u64(entry.minimum_bandwidth_bps);
    writer.u64(entry.maximum_bandwidth_bps);
    writer.u64(entry.burst_bytes);
    writer.boolean(entry.requires_simultaneous_start);
    writer.boolean(entry.requires_evidence_freshness);
    writer.enumeration(entry.isolation);
  }
  writer.u32(static_cast<std::uint32_t>(value.collective_policies.size()));
  for (const CollectiveClassPolicy& entry : value.collective_policies) {
    writer.enumeration(entry.collective_class);
    writer.enumeration(entry.traffic_class);
    writer.boolean(entry.admit_when_unknown_semantics);
    writer.boolean(entry.allow_member_override);
    writer.u64(entry.freshness_requirement_ms);
  }
  return Status::ok();
}

Status decode_traffic_policy(Reader& reader, TrafficPolicy& out) {
  TrafficPolicy policy;
  Status status = reader.identity(policy.generation);
  if (!status.is_ok()) return status;
  status = reader.u64(policy.default_maximum_bandwidth_bps);
  if (!status.is_ok()) return status;
  status = reader.u64(policy.burst_multiplier_per_mille);
  if (!status.is_ok()) return status;
  for (std::size_t index = 0; index < CongestionResponseTable::kBucketCount; ++index) {
    status = reader.u32(policy.congestion_response.utilization_bps[index]);
    if (!status.is_ok()) return status;
    status = reader.u32(policy.congestion_response.rate_numerator_per_mille[index]);
    if (!status.is_ok()) return status;
  }
  std::uint32_t classes = 0;
  status = reader.count(classes, 16);
  if (!status.is_ok()) return status;
  policy.class_policies.resize(classes);
  for (std::uint32_t index = 0; index < classes; ++index) {
    TrafficClassPolicy& entry = policy.class_policies[index];
    std::uint8_t raw = 0;
    status = reader.u8(raw);
    if (!status.is_ok()) return status;
    if (raw > static_cast<std::uint8_t>(TrafficClass::kProbe)) {
      return Status(ErrorCode::kDecodeInvalidEnum, "traffic class is not defined by this version");
    }
    entry.traffic_class = static_cast<TrafficClass>(raw);
    status = reader.u64(entry.priority);
    if (!status.is_ok()) return status;
    status = reader.u64(entry.minimum_bandwidth_bps);
    if (!status.is_ok()) return status;
    status = reader.u64(entry.maximum_bandwidth_bps);
    if (!status.is_ok()) return status;
    status = reader.u64(entry.burst_bytes);
    if (!status.is_ok()) return status;
    status = reader.boolean(entry.requires_simultaneous_start);
    if (!status.is_ok()) return status;
    status = reader.boolean(entry.requires_evidence_freshness);
    if (!status.is_ok()) return status;
    status = reader.u8(raw);
    if (!status.is_ok()) return status;
    if (raw > static_cast<std::uint8_t>(IsolationMode::kDedicatedPath)) {
      return Status(ErrorCode::kDecodeInvalidEnum, "isolation mode is not defined by this version");
    }
    entry.isolation = static_cast<IsolationMode>(raw);
  }
  std::uint32_t collectives = 0;
  status = reader.count(collectives, 16);
  if (!status.is_ok()) return status;
  policy.collective_policies.resize(collectives);
  for (std::uint32_t index = 0; index < collectives; ++index) {
    CollectiveClassPolicy& entry = policy.collective_policies[index];
    std::uint8_t raw = 0;
    status = reader.u8(raw);
    if (!status.is_ok()) return status;
    if (raw > static_cast<std::uint8_t>(CollectiveClass::kUnknownVendor)) {
      return Status(ErrorCode::kDecodeInvalidEnum, "collective class is not defined by this version");
    }
    entry.collective_class = static_cast<CollectiveClass>(raw);
    status = reader.u8(raw);
    if (!status.is_ok()) return status;
    if (raw > static_cast<std::uint8_t>(TrafficClass::kProbe)) {
      return Status(ErrorCode::kDecodeInvalidEnum, "traffic class is not defined by this version");
    }
    entry.traffic_class = static_cast<TrafficClass>(raw);
    status = reader.boolean(entry.admit_when_unknown_semantics);
    if (!status.is_ok()) return status;
    status = reader.boolean(entry.allow_member_override);
    if (!status.is_ok()) return status;
    status = reader.u64(entry.freshness_requirement_ms);
    if (!status.is_ok()) return status;
  }
  out = std::move(policy);
  return Status::ok();
}

}  // namespace ctf::protocol
