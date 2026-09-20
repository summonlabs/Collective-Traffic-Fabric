// Collective Traffic Fabric - participant helper process used by the
// multiprocess proof suite.  Its behaviour is driven entirely by argv so the
// parent can start, fence and kill it deterministically.
// Copyright 2026 Summon Software Labs.
#include <cstdio>

#include "support/process_helper.hpp"

int main(int argc, char** argv) {
  ctf::test::ParticipantOptions options;
  const ctf::Status parsed = ctf::test::parse_participant_options(argc, argv, options);
  if (!parsed.is_ok()) {
    std::fprintf(stderr, "participant: %s\n", parsed.message().c_str());
    return 2;
  }
  return ctf::test::run_participant(options);
}
