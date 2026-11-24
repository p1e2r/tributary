// SPDX-License-Identifier: MIT
#pragma once
#include <string>

namespace app {
// Run the stdio MCP server until stdin closes. `repo` is the default repository (may be empty).
int mcp_main(const std::string& repo);
// Remove what the last self-update left behind (old binaries, staging folders).
void cleanup_after_update();
}
