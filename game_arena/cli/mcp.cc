#include "game_arena/cli/mcp.h"

#include <boost/json/array.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>
#include <cstddef>
#include <istream>
#include <ostream>
#include <string_view>

namespace arena_cli {
namespace {

// The most of a command's output one result puts in an agent's context.
constexpr size_t kMaxOutputBytes = 20000;

struct Arg {
  const char *name;
  const char *type;  // JSON schema; an "array" is of strings
  const char *description;
  bool positional = false;
  bool required = false;
};

struct Tool {
  const char *name;
  const char *command;
  const char *description;
  std::vector<Arg> args;
};

const std::vector<Tool> &Tools() {
  static const auto *tools = new std::vector<Tool>{
      {"arena_rules",
       "rules",
       "This tournament's problem, its limits and how to submit. Read this "
       "first: the server is the authority on what the problem is.",
       {}},
      {"arena_submit",
       "submit",
       "Submits a solution and queues its build and placement. With neither "
       "file nor patch it sends your own directory, which is the usual way. "
       "Returns the candidate and job ids; poll arena_job.",
       {{"file", "array",
         "Files to submit, relative to the kit or absolute. Read from disk "
         "here, so never paste code back."},
        {"patch", "string",
         "A unified diff against the kit's tree, instead of files."},
        {"name", "string", "Display name. Default: you."},
        {"entry_header", "string",
         "The header defining the entry point, when there are several."},
        {"parent_id", "string", "The candidate this one builds on."},
        {"notes", "string", "Free-form notes."},
        {"game", "string", "The game, when the server has several."},
        {"cancel_running", "boolean",
         "Replace your in-flight work instead of being refused for it."},
        {"param", "array", "k=v registry parameters."},
        {"extra_dep", "array",
         "Extra bazel deps within the problem's allowlist."}}},
      {"arena_job",
       "job",
       "A build or match job's status, and a failed build's compiler errors.",
       {{"job_id", "string", "The job.", true, true},
        {"wait", "boolean", "Block until the job is finished."}}},
      {"arena_candidates",
       "candidates",
       "Every candidate, including ones still building or broken.",
       {{"order", "string", "best (default) or newest."},
        {"author", "string", "Only this author's."},
        {"limit", "integer", "Max rows."},
        {"game", "string", "Only this game's."}}},
      {"arena_leaderboard",
       "leaderboard",
       "Current standings.",
       {{"limit", "integer", "Max rows."}}},
      {"arena_source",
       "source",
       "A participant's code. Without path it is pulled into their directory "
       "beside yours; with path, that one file is returned.",
       {{"name", "string", "The participant.", true, true},
        {"path", "string", "One file to return.", true}}},
      {"arena_spar",
       "spar",
       "Plays your directory against a participant's, or builtin:<name>, "
       "here, bounded as the tournament bounds a game.",
       {{"name", "string", "The rival, or builtin:<name>.", true, true},
        {"games", "integer", "Games to play."}}},
  };
  return *tools;
}

boost::json::object Schema(const Tool &tool) {
  boost::json::object properties;
  boost::json::array required;
  for (const Arg &arg : tool.args) {
    boost::json::object property{{"type", arg.type},
                                 {"description", arg.description}};
    if (std::string_view(arg.type) == "array") {
      property["items"] = boost::json::object{{"type", "string"}};
    }
    properties[arg.name] = std::move(property);
    if (arg.required) {
      required.push_back(boost::json::string(arg.name));
    }
  }
  return {{"type", "object"},
          {"properties", std::move(properties)},
          {"required", std::move(required)}};
}

std::string Text(const boost::json::value &value) {
  if (value.is_string()) {
    return std::string(value.as_string());
  }
  if (value.is_array()) {
    std::string text;
    for (const boost::json::value &item : value.as_array()) {
      text += (text.empty() ? "" : ",") + Text(item);
    }
    return text;
  }
  if (value.is_double()) {
    return std::to_string(static_cast<long long>(value.as_double()));
  }
  return boost::json::serialize(value);
}

// |arguments| as |tool|'s command line: its flags, then its positionals.
std::vector<std::string> Argv(const Tool &tool,
                              const boost::json::object &arguments) {
  std::vector<std::string> argv = {tool.command};
  std::vector<std::string> positional;
  for (const Arg &arg : tool.args) {
    const boost::json::value *value = arguments.if_contains(arg.name);
    if (value == nullptr || value->is_null()) {
      continue;
    }
    if (arg.positional) {
      positional.push_back(Text(*value));
    } else {
      argv.push_back("--" + std::string(arg.name) + "=" + Text(*value));
    }
  }
  argv.insert(argv.end(), positional.begin(), positional.end());
  return argv;
}

boost::json::object Error(int code, std::string_view message) {
  return {{"code", code}, {"message", message}};
}

}  // namespace

std::optional<boost::json::object> HandleMcp(const boost::json::object &request,
                                             const McpRunner &run) {
  const boost::json::value *id = request.if_contains("id");
  if (id == nullptr) {
    return std::nullopt;
  }
  const boost::json::value *method = request.if_contains("method");
  const std::string_view name = method != nullptr && method->is_string()
                                    ? std::string_view(method->as_string())
                                    : std::string_view();
  const boost::json::object none;
  const boost::json::value *raw = request.if_contains("params");
  const boost::json::object &params =
      raw != nullptr && raw->is_object() ? raw->as_object() : none;

  boost::json::object reply{{"jsonrpc", "2.0"}, {"id", *id}};
  if (name == "initialize") {
    const boost::json::value *version = params.if_contains("protocolVersion");
    reply["result"] = boost::json::object{
        {"protocolVersion",
         version != nullptr ? *version : boost::json::value("2025-06-18")},
        {"capabilities", {{"tools", boost::json::object{}}}},
        {"serverInfo", {{"name", "arena"}, {"version", "1"}}},
        {"instructions",
         "Each tool is an arena_cli command and its arguments are that "
         "command's flags. Start with arena_rules."}};
  } else if (name == "ping") {
    reply["result"] = boost::json::object{};
  } else if (name == "tools/list") {
    boost::json::array tools;
    for (const Tool &tool : Tools()) {
      tools.push_back(boost::json::object{{"name", tool.name},
                                          {"description", tool.description},
                                          {"inputSchema", Schema(tool)}});
    }
    reply["result"] = boost::json::object{{"tools", std::move(tools)}};
  } else if (name == "tools/call") {
    const boost::json::value *tool_name = params.if_contains("name");
    const Tool *tool = nullptr;
    for (const Tool &candidate : Tools()) {
      if (tool_name != nullptr && tool_name->is_string() &&
          tool_name->as_string() == candidate.name) {
        tool = &candidate;
      }
    }
    if (tool == nullptr) {
      reply["error"] = Error(-32602, "unknown tool");
      return reply;
    }
    const boost::json::value *arguments = params.if_contains("arguments");
    auto [code, output] =
        run(Argv(*tool, arguments != nullptr && arguments->is_object()
                            ? arguments->as_object()
                            : none));
    if (output.size() > kMaxOutputBytes) {
      size_t end = kMaxOutputBytes;
      while (end > 0 && (output[end] & 0xC0) == 0x80) {
        --end;  // not inside a UTF-8 sequence
      }
      output.resize(end);
      output += "\n[truncated]";
    }
    reply["result"] = boost::json::object{
        {"content", boost::json::array{boost::json::object{
                        {"type", "text"}, {"text", std::string_view(output)}}}},
        {"isError", code != 0}};
  } else {
    reply["error"] = Error(-32601, "unknown method");
  }
  return reply;
}

void ServeMcp(std::istream &in, std::ostream &out, const McpRunner &run) {
  std::string line;
  while (std::getline(in, line)) {
    boost::system::error_code ec;
    const boost::json::value message = boost::json::parse(line, ec);
    if (ec || !message.is_object()) {
      continue;
    }
    if (const auto reply = HandleMcp(message.as_object(), run)) {
      out << boost::json::serialize(*reply) << "\n" << std::flush;
    }
  }
}

}  // namespace arena_cli
