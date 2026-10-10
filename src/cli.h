#pragma once
#include <string>
#include <vector>

namespace cli {

/// Pulls every `--home <dir>` out of `args` into config::addHomeOverride.
void takeHomeFlags(std::vector<std::string>& args);
/// True when `args[0]` names a CLI command — how the tray executable tells a command from a launch.
bool isCommand(const std::vector<std::string>& args);
/// claude-usage tick | status | fit | remind add|list|done | version. Returns the exit code.
int run(std::vector<std::string> args);

}  // namespace cli
