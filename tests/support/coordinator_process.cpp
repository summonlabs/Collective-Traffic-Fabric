// Collective Traffic Fabric - coordinator helper process used by the
// multiprocess proof suite.  It serves a real coordinator over loopback TCP
// until a peer asks it to shut down, which is how the parent proves that a
// coordinator restart advances the epoch.
// Copyright 2026 Summon Software Labs.
#include <cstdio>

#include "support/process_helper.hpp"

int main(int argc, char** argv) {
  ctf::test::CoordinatorOptions options;
  const ctf::Status parsed = ctf::test::parse_coordinator_options(argc, argv, options);
  if (!parsed.is_ok()) {
    std::fprintf(stderr, "coordinator: %s\n", parsed.message().c_str());
    return 2;
  }
  return ctf::test::run_coordinator(options);
}
