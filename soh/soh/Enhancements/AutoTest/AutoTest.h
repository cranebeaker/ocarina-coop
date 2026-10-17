#pragma once
#ifdef __cplusplus

#include <string>
#include <vector>

// Scripted two-client Story Co-op test mode for the ZeldaOnline client. Completely inert unless the process is started
// with --coop-test <scenario.json> (or --coop-test-extract-only). See tools/coop-test/README.md.
//
// Flags:
//   --coop-test <scenario.json>     run the scenario
//   --coop-test-role <A|B>          which role of the scenario this process plays
//   --coop-test-out <log.json>      where to write the observation log
//   --coop-test-rom <rom.z64>       ROM to extract from when oot.o2r is missing
//   --coop-test-extract-only        generate oot.o2r unattended, then exit
//   --coop-test-host/-port          ZeldaOnline server endpoint (override scenario)
namespace AutoTest {
bool IsEnabled();
void ParseArgs(int argc, char** argv);
// Arguments handed to the extractor in place of the raw command line while test mode is on.
std::vector<std::string> ExtractArgs();
bool ExtractOnly();
// Sets ZeldaOnline CVars from the scenario. Must run before ZeldaOnlineClient::Enable().
void ApplyConfig();
// Registers the per-frame script runner. Safe to call once at startup.
void RegisterHooks();
} // namespace AutoTest

#endif
