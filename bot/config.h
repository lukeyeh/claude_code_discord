// What the bot needs to be told before it can run, and where it is told:
// environment variables, so that the token never appears on a command line.

#ifndef BOT_CONFIG_H_
#define BOT_CONFIG_H_

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "cli/cli.h"
#include "discord/model.h"

namespace bot {

struct Config {
  // The bot's Discord token.
  std::string token;

  // The people the bot listens to, in any channel it can see. Whoever can
  // talk to it can have Claude act on this machine, so there is no listening
  // to everyone. At least one.
  std::vector<discord::UserId> users;

  // How Claude Code is started for a conversation: where it works, with
  // which model, and what it may do unasked.
  claude_code::Options claude;
};

// Looks up a variable by name; nothing if it is not set.
using Variables =
    std::function<std::optional<std::string>(const std::string& name)>;

// The variables of the process's environment.
std::optional<std::string> Environment(const std::string& name);

// Reads the configuration from `variables`:
//
//   DISCORD_TOKEN       required
//   CLAUDE_USER_IDS     required; the ids of the people to listen to, as
//                       "Copy User ID" gives them, separated by commas
//   CLAUDE_DIRECTORY    optional; the directory Claude works in. The
//                       current directory if unset
//   CLAUDE_MODEL        optional; a model's name or alias, such as "sonnet".
//                       The user's default if unset
//   CLAUDE_PERMISSIONS  optional; what Claude may do unasked: "ask" (the
//                       default: it asks in the channel), "auto" (Claude
//                       Code judges what is safe), "default",
//                       "accept-edits", "plan" or "bypass". See
//                       claude_code::Permissions
//
// Fails with InvalidArgument, naming the variable, if one is missing or
// makes no sense.
absl::StatusOr<Config> LoadConfig(const Variables& variables = &Environment);

}  // namespace bot

#endif  // BOT_CONFIG_H_
