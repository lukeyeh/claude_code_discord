// How the bot is configured: which environment variables it reads, which it
// insists on, and what it makes of them.

#include "bot/config.h"

#include <benchmark/benchmark.h>

#include <map>
#include <optional>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "cli/cli.h"
#include "discord/model.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace {

using absl_testing::StatusIs;
using testing::ElementsAre;
using testing::HasSubstr;

// An environment holding `values` and nothing else.
bot::Variables Holding(std::map<std::string, std::string> values) {
  return [values = std::move(values)](
             const std::string& name) -> std::optional<std::string> {
    const auto found = values.find(name);
    if (found == values.end()) return std::nullopt;

    return found->second;
  };
}

// The least that will do: who the bot is, and whom it listens to.
std::map<std::string, std::string> Minimal() {
  return {
      {
          "DISCORD_TOKEN",
          "secret",
      },
      {
          "CLAUDE_USER_IDS",
          "44",
      },
  };
}

// With only what is required, Claude works where the bot was started, with
// the user's own model, and asks in the channel before doing anything it has
// not been allowed.
TEST(LoadConfigTest, NeedsOnlyTheTokenAndTheUsers) {
  const absl::StatusOr<bot::Config> config =
      bot::LoadConfig(Holding(Minimal()));

  ABSL_ASSERT_OK(config);
  EXPECT_EQ(config->token, "secret");
  EXPECT_THAT(config->users, ElementsAre(discord::UserId{
                                 .value = 44,
                             }));
  EXPECT_EQ(config->claude.directory, "");
  EXPECT_EQ(config->claude.model, "");
  EXPECT_EQ(config->claude.permissions, claude_code::Permissions::kAsk);
}

// Several people, separated by commas, with or without spaces.
TEST(LoadConfigTest, ReadsAListOfUsers) {
  std::map<std::string, std::string> values = Minimal();
  values["CLAUDE_USER_IDS"] = "44, 45";

  const absl::StatusOr<bot::Config> config =
      bot::LoadConfig(Holding(std::move(values)));

  ABSL_ASSERT_OK(config);
  EXPECT_THAT(config->users, ElementsAre(
                                 discord::UserId{
                                     .value = 44,
                                 },
                                 discord::UserId{
                                     .value = 45,
                                 }));
}

// Where Claude works, with what, and how freely.
TEST(LoadConfigTest, ReadsHowClaudeIsToBeStarted) {
  std::map<std::string, std::string> values = Minimal();
  values["CLAUDE_DIRECTORY"] = "/home/me/project";
  values["CLAUDE_MODEL"] = "haiku";
  values["CLAUDE_PERMISSIONS"] = "accept-edits";

  const absl::StatusOr<bot::Config> config =
      bot::LoadConfig(Holding(std::move(values)));

  ABSL_ASSERT_OK(config);
  EXPECT_EQ(config->claude.directory, "/home/me/project");
  EXPECT_EQ(config->claude.model, "haiku");
  EXPECT_EQ(config->claude.permissions, claude_code::Permissions::kAcceptEdits);
}

// Auto leaves it to Claude Code to judge what is safe to do unasked.
TEST(LoadConfigTest, ReadsAutoPermissions) {
  std::map<std::string, std::string> values = Minimal();
  values["CLAUDE_PERMISSIONS"] = "auto";

  const absl::StatusOr<bot::Config> config =
      bot::LoadConfig(Holding(std::move(values)));

  ABSL_ASSERT_OK(config);
  EXPECT_EQ(config->claude.permissions, claude_code::Permissions::kAuto);
}

// Each of the two required variables is asked for by name.
TEST(LoadConfigTest, SaysWhichRequiredVariableIsMissing) {
  for (const std::string name : {
           "DISCORD_TOKEN",
           "CLAUDE_USER_IDS",
       }) {
    std::map<std::string, std::string> values = Minimal();
    values.erase(name);

    EXPECT_THAT(bot::LoadConfig(Holding(std::move(values))),
                StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr(name)));
  }
}

// The people the bot listens to must be said: an empty list is not taken to
// mean everyone, since whoever it listens to can act on this machine.
TEST(LoadConfigTest, RefusesToListenToEveryone) {
  std::map<std::string, std::string> values = Minimal();
  values["CLAUDE_USER_IDS"] = " ";

  EXPECT_THAT(bot::LoadConfig(Holding(std::move(values))),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("CLAUDE_USER_IDS")));
}

// A value that makes no sense is an error at the start, not a surprise
// later.
TEST(LoadConfigTest, RejectsWhatMakesNoSense) {
  std::map<std::string, std::string> values = Minimal();
  values["CLAUDE_USER_IDS"] = "44,luke";
  EXPECT_THAT(bot::LoadConfig(Holding(values)),
              StatusIs(absl::StatusCode::kInvalidArgument, HasSubstr("luke")));

  values = Minimal();
  values["CLAUDE_PERMISSIONS"] = "anything";
  EXPECT_THAT(bot::LoadConfig(Holding(values)),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("CLAUDE_PERMISSIONS")));
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //bot:config_test -- --benchmark_filter=all

// Reading the configuration, which happens once, as the bot starts.
void BM_LoadConfig(benchmark::State& state) {
  const bot::Variables variables = Holding(Minimal());

  for (auto _ : state) benchmark::DoNotOptimize(bot::LoadConfig(variables));
}
BENCHMARK(BM_LoadConfig);

}  // namespace
