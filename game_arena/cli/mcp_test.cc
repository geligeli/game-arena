#include "game_arena/cli/mcp.h"

#include <boost/json/parse.hpp>
#include <sstream>

#include "gtest/gtest.h"

namespace arena_cli {
namespace {

using Argv = std::vector<std::string>;

boost::json::object Call(std::string_view message, Argv *ran = nullptr,
                         int code = 0) {
  const auto reply =
      HandleMcp(boost::json::parse(message).as_object(), [&](const Argv &argv) {
        if (ran != nullptr) {
          *ran = argv;
        }
        return std::pair{code, std::string("out")};
      });
  return reply.value_or(boost::json::object{{"none", true}});
}

TEST(McpTest, Initialize) {
  const auto reply = Call(
      R"({"jsonrpc":"2.0","id":1,"method":"initialize",
          "params":{"protocolVersion":"2025-06-18"}})");
  EXPECT_EQ(reply.at("id"), 1);
  EXPECT_EQ(reply.at("result").at("protocolVersion"), "2025-06-18");
  EXPECT_TRUE(
      reply.at("result").at("capabilities").as_object().contains("tools"));
}

TEST(McpTest, ListsTools) {
  const auto reply = Call(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
  const auto &tools = reply.at("result").at("tools").as_array();
  ASSERT_FALSE(tools.empty());
  EXPECT_EQ(tools[0].at("name"), "arena_rules");
  EXPECT_EQ(tools[0].at("inputSchema").at("type"), "object");
}

TEST(McpTest, SubmitIsFlags) {
  Argv ran;
  const auto reply = Call(
      R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{
          "name":"arena_submit",
          "arguments":{"file":["a.h","b.cc"],"notes":"n","cancel_running":true}}})",
      &ran);
  EXPECT_EQ(ran, (Argv{"submit", "--file=a.h,b.cc", "--notes=n",
                       "--cancel_running=true"}));
  EXPECT_EQ(reply.at("result").at("content").at(0).at("text"), "out");
  EXPECT_EQ(reply.at("result").at("isError"), false);
}

TEST(McpTest, PositionalsFollowFlags) {
  Argv ran;
  Call(R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{
           "name":"arena_spar","arguments":{"name":"bob","games":4}}})",
       &ran);
  EXPECT_EQ(ran, (Argv{"spar", "--games=4", "bob"}));
}

TEST(McpTest, FailureIsAnError) {
  const auto reply =
      Call(R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{
               "name":"arena_job","arguments":{"job_id":"j"}}})",
           nullptr, 1);
  EXPECT_EQ(reply.at("result").at("isError"), true);
}

TEST(McpTest, NotificationGetsNoReply) {
  EXPECT_TRUE(Call(R"({"jsonrpc":"2.0","method":"notifications/initialized"})")
                  .contains("none"));
}

TEST(McpTest, UnknownMethodAndTool) {
  EXPECT_EQ(Call(R"({"jsonrpc":"2.0","id":6,"method":"nope"})")
                .at("error")
                .at("code"),
            -32601);
  EXPECT_EQ(Call(R"({"jsonrpc":"2.0","id":7,"method":"tools/call",
                     "params":{"name":"nope"}})")
                .at("error")
                .at("code"),
            -32602);
}

TEST(McpTest, ServesOneLinePerReply) {
  std::istringstream in(
      R"({"jsonrpc":"2.0","id":1,"method":"ping"})"
      "\n"
      R"({"jsonrpc":"2.0","method":"notifications/initialized"})"
      "\n");
  std::ostringstream out;
  ServeMcp(in, out, [](const Argv &) { return std::pair{0, std::string()}; });
  EXPECT_EQ(out.str(), "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n");
}

}  // namespace
}  // namespace arena_cli
