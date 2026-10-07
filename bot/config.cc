#include "bot/config.h"

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "cli/cli.h"
#include "discord/model.h"

namespace bot {
namespace {

// The ids in `list`, which is the value of the variable `name`: numbers
// separated by commas, and at least one of them.
absl::StatusOr<std::vector<uint64_t>> ParseIds(std::string_view name,
                                               std::string_view list) {
  std::vector<uint64_t> ids;
  for (const std::string_view item :
       absl::StrSplit(list, ',', absl::SkipWhitespace())) {
    uint64_t id = 0;
    if (!absl::SimpleAtoi(absl::StripAsciiWhitespace(item), &id) || id == 0) {
      return absl::InvalidArgumentError(
          absl::StrCat(name, " has something that is not an id: ", item));
    }

    ids.push_back(id);
  }

  if (ids.empty()) {
    return absl::InvalidArgumentError(absl::StrCat(name, " is not set"));
  }
  return ids;
}

absl::StatusOr<claude_code::Permissions> ParsePermissions(
    std::string_view name) {
  if (name.empty() || name == "ask") return claude_code::Permissions::kAsk;
  if (name == "auto") return claude_code::Permissions::kAuto;
  if (name == "default") return claude_code::Permissions::kDefault;
  if (name == "accept-edits") return claude_code::Permissions::kAcceptEdits;
  if (name == "plan") return claude_code::Permissions::kPlan;
  if (name == "bypass") return claude_code::Permissions::kBypass;

  return absl::InvalidArgumentError(absl::StrCat(
      "CLAUDE_PERMISSIONS is not one of ask, auto, default, accept-edits, plan "
      "and bypass: ",
      name));
}

}  // namespace

std::optional<std::string> Environment(const std::string& name) {
  const char* const value = std::getenv(name.c_str());
  if (value == nullptr) return std::nullopt;

  return std::string(value);
}

absl::StatusOr<Config> LoadConfig(const Variables& variables) {
  // Set but empty means the same as not set, which is what a line like
  // `CLAUDE_MODEL=` in an environment file is taken to say.
  const auto lookup = [&variables](const std::string& name) {
    return variables(name).value_or("");
  };

  Config config;

  config.token = lookup("DISCORD_TOKEN");
  if (config.token.empty()) {
    return absl::InvalidArgumentError("DISCORD_TOKEN is not set");
  }

  ABSL_ASSIGN_OR_RETURN(const std::vector<uint64_t> users,
                        ParseIds("CLAUDE_USER_IDS", lookup("CLAUDE_USER_IDS")));
  for (const uint64_t user : users) {
    config.users.push_back(discord::UserId{
        .value = user,
    });
  }

  config.claude.directory = lookup("CLAUDE_DIRECTORY");
  config.claude.model = lookup("CLAUDE_MODEL");
  ABSL_ASSIGN_OR_RETURN(config.claude.permissions,
                        ParsePermissions(lookup("CLAUDE_PERMISSIONS")));

  return config;
}

}  // namespace bot
