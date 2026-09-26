#ifndef GAME_ARENA_GAME_ARENA_CLI_MCP_H
#define GAME_ARENA_GAME_ARENA_CLI_MCP_H

// arena_cli's commands as MCP tools, on stdio: what `arena_cli mcp` serves an
// agent. A tool call is an arena_cli command line, its arguments the flags of
// that command, and its result what the command prints -- so the agent and the
// participant are handed the same operations, from one implementation.

#include <boost/json/object.hpp>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace arena_cli {

// Runs `arena_cli <argv...>` and returns its exit code and its output.
using McpRunner = std::function<std::pair<int, std::string>(
    const std::vector<std::string> &)>;

// The reply to one JSON-RPC message; nullopt for a notification.
std::optional<boost::json::object> HandleMcp(const boost::json::object &request,
                                             const McpRunner &run);

// One message per line from |in| until it ends, each reply a line on |out|.
void ServeMcp(std::istream &in, std::ostream &out, const McpRunner &run);

}  // namespace arena_cli

#endif  // GAME_ARENA_GAME_ARENA_CLI_MCP_H
