#include "bot/bot.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include "async/status_macros.h"
#include "async/task.h"
#include "bot/config.h"
#include "cli/cli.h"
#include "discord/client.h"
#include "discord/model.h"
#include "net/event_loop.h"
#include "net/stream.h"
#include "protocol/message.h"
#include "render/render.h"
#include "session/session.h"

namespace bot {
namespace {

// The eyes emoji, as UTF-8: the bot has seen a message and passed it on.
constexpr char kSeen[] = "\xF0\x9F\x91\x80";

// What Claude is told about where its words go. The bot keeps the machine
// out of its own posts; what Claude chooses to say, only Claude can.
constexpr char kDiscretion[] =
    "Your replies are posted in a Discord channel that other people can "
    "read. Do not reveal anything about the machine you are running on: no "
    "file paths, directory or file names, user names, host names, "
    "environment details or command output that shows them. Describe your "
    "work in general terms instead.";

// The tick and the cross: the reactions that answer what Claude Code asks.
constexpr char kYes[] = "\xE2\x9C\x85";
constexpr char kNo[] = "\xE2\x9D\x8C";

// How often a conversation's relay looks up from waiting for Claude, to see
// whether the conversation has been ended in the meantime.
constexpr std::chrono::seconds kLookUpEvery(1);

// How many of the tools Claude has just used are listed. Enough to see what
// it is up to; a turn can use dozens.
constexpr size_t kNotesShown = 5;

// How long to wait for more notes of tools before showing the ones there
// are. Claude often uses many at once, and Discord would make the bot wait
// if it edited a message for each.
constexpr std::chrono::milliseconds kNotesSettle(300);

// How long to go on trusting that Discord still shows the bot as typing.
// It shows it for ten seconds, so this leaves no gap.
constexpr std::chrono::seconds kTypingShows(8);

using Clock = std::chrono::steady_clock;

std::vector<discord::Command> Commands() {
  return {
      discord::Command{
          .name = "stop",
          .description = "Cut short what Claude is doing",
      },
      discord::Command{
          .name = "new",
          .description = "Forget this conversation and start another",
      },
      discord::Command{
          .name = "allow",
          .description = "Let Claude use everything it is asking about",
      },
      discord::Command{
          .name = "deny",
          .description = "Refuse everything Claude is asking about",
          .options =
              {
                  discord::Option{
                      .name = "reason",
                      .description = "What to tell Claude",
                      .type = discord::OptionType::kText,
                      .required = false,
                  },
              },
      },
  };
}

// Something Claude Code has asked, and the message in the channel that
// puts the question: the one whose reactions answer it.
struct Asked {
  claude_code::PermissionRequest request;
  // The id 0 if the question could not be posted.
  discord::MessageId message;
};

// One channel's conversation with Claude. Shared between the task that
// serves Discord, which sends into it, and the task that relays what comes
// out of it; it lasts until both have let go.
struct Conversation {
  std::unique_ptr<claude_code::Session> session;

  // What Claude Code calls the conversation, once it has said: how to pick
  // it up again if Claude Code stops.
  std::string session_id;

  // What Claude Code has asked and not yet been told, oldest first. Claude
  // may want several tools at once, and Claude Code asks about each, and
  // waits until every one has been answered.
  std::vector<Asked> asked;

  // The notes of the tools Claude is using, which are gathered into one
  // message in the channel and kept up to date by editing it, so that a
  // busy stretch is a few lines changing in place and not a column of
  // posts. Only the latest few are shown, and the rest counted. Whatever
  // else is posted ends it; the next note starts another.
  struct ToolLog {
    // The message, or the id 0 if there is none yet.
    discord::MessageId message;
    // The latest notes, oldest first: at most kNotesShown of them.
    std::deque<std::string> latest;
    // How many came before those.
    size_t earlier = 0;
    // There are notes the channel has not been shown.
    bool behind = false;

    // What the message says, or is about to.
    std::string Text() const {
      std::string text;
      if (earlier > 0) {
        text = absl::StrCat("-# ", earlier, " earlier tool ",
                            earlier == 1 ? "use" : "uses");
      }
      for (const std::string& note : latest) {
        absl::StrAppend(&text, text.empty() ? "" : "\n", note);
      }
      return text;
    }
  };
  ToolLog tool_log;

  // Claude is at work on a turn: a prompt has gone in and its Result has
  // not come out.
  bool working = false;

  // When the channel was last shown that Claude is at work, by the bot
  // appearing to type. Unset if it has not been since the bot last posted
  // there, which is what makes Discord stop showing it.
  std::optional<Clock::time_point> typing_shown;

  // Nothing more is to be said to it or relayed from it: it was forgotten
  // with /new, or Claude Code has stopped.
  bool over = false;
};

// Has the bot appear to be typing in `channel` for as long as Claude is at
// work in `conversation` and not waiting on an answer, when called often
// enough: it does something only when Discord would be about to stop
// showing it.
Task<> ShowWorking(discord::Client& client, discord::ChannelId channel,
                   Conversation& conversation) {
  if (!conversation.working || !conversation.asked.empty() ||
      conversation.over) {
    co_return;
  }
  if (conversation.typing_shown.has_value() &&
      Clock::now() - *conversation.typing_shown < kTypingShows) {
    co_return;
  }

  conversation.typing_shown = Clock::now();
  // Only an appearance: failing to keep it up is not worth a word.
  (co_await client.ShowTyping(channel)).IgnoreError();
}

// Posts `text` in `channel`, and evaluates to the id of the message. Links
// in it are left as links: Claude cites pages freely, and a preview under
// each would bury what it said. A post
// that cannot be made is logged, there being nobody else to tell, and the
// id is then 0.
Task<discord::MessageId> Post(discord::Client& client,
                              discord::ChannelId channel,
                              std::string_view text) {
  const absl::StatusOr<discord::MessageId> posted =
      co_await client.Send(channel, text, discord::LinkPreviews::kHidden);
  if (!posted.ok()) {
    LOG(ERROR) << "could not post in channel " << channel.value << ": "
               << posted.status();
    co_return discord::MessageId{};
  }

  co_return *posted;
}

// Shows the channel the notes it has not been shown: in a new message if
// the log has none, and otherwise by editing the one it has.
Task<> ShowToolLog(discord::Client& client, discord::ChannelId channel,
                   Conversation& conversation) {
  Conversation::ToolLog& log = conversation.tool_log;
  if (!std::exchange(log.behind, false)) co_return;

  const std::string text = log.Text();
  if (log.message.value == 0) {
    log.message = co_await Post(client, channel, text);
    conversation.typing_shown.reset();
    co_return;
  }

  const absl::Status edited = co_await client.Edit(
      channel, log.message, text, discord::LinkPreviews::kHidden);
  if (!edited.ok()) {
    LOG(ERROR) << "could not add to a message in channel " << channel.value
               << ": " << edited;
  }
}

// Ends the log, having shown what is in it: what is posted next goes below
// it, and the next note starts a new one below that.
Task<> EndToolLog(discord::Client& client, discord::ChannelId channel,
                  Conversation& conversation) {
  co_await ShowToolLog(client, channel, conversation);
  conversation.tool_log = {};
}

// Adds `note` to the log, to be shown with whatever else arrives shortly.
void AddToToolLog(Conversation& conversation, std::string note) {
  Conversation::ToolLog& log = conversation.tool_log;

  log.latest.push_back(std::move(note));
  if (log.latest.size() > kNotesShown) {
    log.latest.pop_front();
    ++log.earlier;
  }
  log.behind = true;
}

// Puts `request` to the channel: a post that asks, with the two reactions
// that answer it. Evaluates to that message.
Task<discord::MessageId> Ask(discord::Client& client,
                             discord::ChannelId channel,
                             Conversation& conversation,
                             const claude_code::Message& request) {
  co_await EndToolLog(client, channel, conversation);

  discord::MessageId message;
  for (const render::Post& post : render::Posts(request)) {
    message = co_await Post(client, channel, post.text);
    conversation.typing_shown.reset();
  }
  if (message.value == 0) co_return message;

  // In this order, so that yes is on the left.
  for (const char* const emoji : {kYes, kNo}) {
    const absl::Status offered = co_await client.React(channel, message, emoji);
    if (!offered.ok()) {
      LOG(ERROR) << "could not offer an answer in channel " << channel.value
                 << ": " << offered;
    }
  }

  co_return message;
}

// Posts what Claude says in `conversation` to `channel`, as it says it,
// until the conversation is over.
Task<> Relay(discord::Client& client, discord::ChannelId channel,
             std::shared_ptr<Conversation> conversation) {
  while (!conversation->over) {
    co_await ShowWorking(client, channel, *conversation);

    // With notes waiting to be shown, only as long as more might be about
    // to join them.
    const absl::StatusOr<claude_code::Message> message =
        co_await conversation->session->Next(conversation->tool_log.behind
                                                 ? net::After(kNotesSettle)
                                                 : net::After(kLookUpEvery));
    if (absl::IsDeadlineExceeded(message.status())) {
      co_await ShowToolLog(client, channel, *conversation);
      continue;
    }

    if (!message.ok()) {
      LOG(WARNING) << "Claude Code stopped in channel " << channel.value << ": "
                   << message.status();
      // Unless it was ended here, in which case the channel knows.
      if (!conversation->over) {
        conversation->over = true;
        co_await Post(client, channel,
                      "Claude Code has stopped. Your next message starts it "
                      "again, where it left off.");
      }
      co_return;
    }

    switch (message->kind()) {
      case claude_code::Message::Kind::kInit:
        conversation->session_id = message->init().session_id;
        break;
      case claude_code::Message::Kind::kPermissionRequest:
        conversation->asked.push_back(Asked{
            .request = message->permission_request(),
            .message = co_await Ask(client, channel, *conversation, *message),
        });
        // Asked, not posted like the rest.
        continue;
      case claude_code::Message::Kind::kResult:
        // A turn that is over is not waiting for an answer.
        conversation->asked.clear();
        conversation->working = false;
        break;
      case claude_code::Message::Kind::kTextDelta:
      case claude_code::Message::Kind::kAssistant:
      case claude_code::Message::Kind::kToolResults:
      case claude_code::Message::Kind::kOther: break;
    }

    for (const render::Post& post : render::Posts(*message)) {
      if (post.is_tool_note) {
        AddToToolLog(*conversation, post.text);
        continue;
      }

      co_await EndToolLog(client, channel, *conversation);
      co_await Post(client, channel, post.text);
      // Discord takes a post to mean the typing is done.
      conversation->typing_shown.reset();
    }
  }
}

// The bot at work: its connection to Discord, and the conversations it is
// holding.
class Bot {
 public:
  Bot(discord::Client& client, const Config& config, StartClaude start)
      : client_(client), config_(config), start_(std::move(start)) {}

  // Says to Claude what one of the bot's people said to the bot.
  Task<absl::Status> OnMessage(const discord::Message& message) {
    if (message.author.bot || !ListensTo(message.author.id)) {
      co_return absl::OkStatus();
    }

    co_await OfferCommandsIn(message.guild);

    const std::string prompt = SaidToBot(message.content);
    if (prompt.empty()) co_return absl::OkStatus();

    CO_ASSIGN_OR_RETURN(const std::shared_ptr<Conversation> conversation,
                        co_await ConversationIn(message.channel));

    // Before Claude can answer, so that the mark comes first. A message
    // that cannot be marked is still passed on.
    const absl::Status marked = co_await client_.React(message, kSeen);
    if (!marked.ok()) LOG(WARNING) << "could not mark a message: " << marked;

    conversation->working = true;
    co_await ShowWorking(client_, message.channel, *conversation);

    const absl::Status sent = co_await conversation->session->Send(prompt);
    if (!sent.ok()) {
      conversation->over = true;
      co_await Post(client_, message.channel,
                    "Claude Code has stopped. Say that again to start it "
                    "again, where it left off.");
    }

    co_return sent;
  }

  // Takes a tick or a cross from one of the bot's people, on the message
  // that puts a question, as the answer to it.
  Task<absl::Status> OnReaction(const discord::ReactionAdded& reaction) {
    // Which leaves out the bot's own, put there for them to press.
    if (!ListensTo(reaction.user.id)) co_return absl::OkStatus();
    if (reaction.emoji != kYes && reaction.emoji != kNo) {
      co_return absl::OkStatus();
    }

    const std::shared_ptr<Conversation> conversation = Find(reaction.channel);
    if (conversation == nullptr) co_return absl::OkStatus();
    const auto found = std::ranges::find(conversation->asked, reaction.message,
                                         &Asked::message);
    if (found == conversation->asked.end()) co_return absl::OkStatus();

    // Taken out before it is answered, so that it cannot be answered twice.
    const claude_code::PermissionRequest request = std::move(found->request);
    conversation->asked.erase(found);

    if (reaction.emoji == kYes) {
      co_return co_await conversation->session->Allow(request);
    }
    co_return co_await conversation->session->Deny(
        request, absl::StrCat(reaction.user.name, " said no."));
  }

  // Answers a slash command.
  Task<absl::Status> OnCommand(const discord::CommandInvoked& command) {
    if (!ListensTo(command.user.id)) {
      co_return co_await client_.Respond(command, "That is not for you.");
    }

    const absl::Status answered = co_await Answer(command);

    // After the answer, which Discord wants within seconds. A command used
    // in a server that has not been offered this version's commands may
    // have been typed against an older version's, and should be the last.
    co_await OfferCommandsIn(command.guild);
    co_return answered;
  }

 private:
  // Does what `command`, from one of the bot's people, asks.
  Task<absl::Status> Answer(const discord::CommandInvoked& command) {
    const std::shared_ptr<Conversation> conversation = Find(command.channel);

    if (command.name == "new") {
      if (conversation != nullptr) conversation->over = true;
      conversations_.erase(command.channel.value);

      co_return co_await client_.Respond(
          command, "Forgotten. Your next message starts a new conversation.");
    }

    if (command.name == "stop") {
      if (conversation == nullptr) {
        co_return co_await client_.Respond(command, "Claude is not running.");
      }

      CO_RETURN_IF_ERROR(co_await conversation->session->Interrupt());
      co_return co_await client_.Respond(command, "Stopping.");
    }

    if (command.name == "allow" || command.name == "deny") {
      if (conversation == nullptr || conversation->asked.empty()) {
        co_return co_await client_.Respond(
            command, "Claude has not asked for anything.");
      }

      // Everything it is waiting on, at once: Claude Code goes no further
      // until all of it is answered. Reactions answer one at a time. Taken
      // out of the conversation first, so that none is answered twice.
      const std::vector<Asked> answered =
          std::exchange(conversation->asked, {});

      const bool allowed = command.name == "allow";
      const std::string given = command.Text("reason");
      const std::string reason =
          given.empty() ? absl::StrCat(command.user.name, " said no.") : given;

      std::vector<std::string_view> tools;
      for (const Asked& asked : answered) {
        if (allowed) {
          CO_RETURN_IF_ERROR(
              co_await conversation->session->Allow(asked.request));
        } else {
          CO_RETURN_IF_ERROR(
              co_await conversation->session->Deny(asked.request, reason));
        }
        tools.push_back(asked.request.tool_use.name);
      }

      const std::string reply = absl::StrCat(
          allowed ? "Allowed " : "Refused ",
          tools.size() == 1 ? "" : absl::StrCat("all ", tools.size(), ": "),
          absl::StrJoin(tools, ", "), ".");
      co_return co_await client_.Respond(command, reply);
    }

    co_return co_await client_.Respond(command, "I don't know that command.");
  }

  // What `content` says to the bot: the rest of a message that mentions it.
  // Empty if the message does not mention the bot, or says nothing else.
  std::string SaidToBot(std::string_view content) const {
    // Discord writes a mention as the id between these, the second being
    // how it once wrote one of somebody with a nickname.
    const std::string mention = discord::Mention(client_.self().id);
    const std::string old_mention =
        absl::StrCat("<@!", client_.self().id.value, ">");
    if (!absl::StrContains(content, mention) &&
        !absl::StrContains(content, old_mention)) {
      return "";
    }

    return std::string(absl::StripAsciiWhitespace(
        absl::StrReplaceAll(content, {
                                         {
                                             mention,
                                             "",
                                         },
                                         {
                                             old_mention,
                                             "",
                                         },
                                     })));
  }

  // Whether this is one of the bot's people.
  bool ListensTo(discord::UserId user) const {
    return std::ranges::find(config_.users, user) != config_.users.end();
  }

  // Offers the bot's commands in `guild`, the first time it is heard from.
  // Discord has no way to offer them wherever the bot is that takes effect
  // at once, so each server is offered them as it turns out to matter.
  Task<> OfferCommandsIn(discord::GuildId guild) {
    // Not a server at all, or one that has them.
    if (guild.value == 0 ||
        std::ranges::find(offered_, guild) != offered_.end()) {
      co_return;
    }

    const std::vector<discord::Command> commands = Commands();
    const absl::Status offered =
        co_await client_.OfferCommands(guild, commands);
    // Without them the bot still relays, so this is not worth stopping for.
    // The next message there tries again.
    if (!offered.ok()) {
      LOG(ERROR) << "could not offer commands in server " << guild.value << ": "
                 << offered;
      co_return;
    }

    offered_.push_back(guild);
  }

  // The conversation under way in `channel`, or nothing.
  std::shared_ptr<Conversation> Find(discord::ChannelId channel) const {
    const auto found = conversations_.find(channel.value);
    if (found == conversations_.end() || found->second->over) return nullptr;

    return found->second;
  }

  // The conversation in `channel`, starting Claude Code if it is not
  // running there. Says so in the channel if it cannot be started.
  Task<absl::StatusOr<std::shared_ptr<Conversation>>> ConversationIn(
      discord::ChannelId channel) {
    if (std::shared_ptr<Conversation> conversation = Find(channel)) {
      co_return conversation;
    }

    claude_code::Options options = config_.claude;
    options.instructions = kDiscretion;
    // One that stopped is taken up where it left off.
    const auto stopped = conversations_.find(channel.value);
    if (stopped != conversations_.end()) {
      options.resume = stopped->second->session_id;
    }

    absl::StatusOr<std::unique_ptr<claude_code::Session>> session =
        start_(options);
    if (!session.ok()) {
      // Why not is for the log, not the channel: it may name what is on
      // this machine.
      co_await Post(client_, channel, "Claude Code could not be started.");
      co_return session.status();
    }

    auto conversation = std::make_shared<Conversation>();
    conversation->session = std::move(*session);
    conversations_.insert_or_assign(channel.value, conversation);
    Spawn(Relay(client_, channel, conversation));

    co_return conversation;
  }

  discord::Client& client_;
  const Config& config_;
  StartClaude start_;
  // By the id of the channel each is in.
  std::unordered_map<uint64_t, std::shared_ptr<Conversation>> conversations_;
  // The servers the commands have been offered in.
  std::vector<discord::GuildId> offered_;
};

Task<absl::Status> ConnectAndServe(const Config& config) {
  CO_ASSIGN_OR_RETURN(discord::Client client,
                      co_await discord::Client::Connect(config.token),
                      _.SetPrepend() << "connecting to Discord: ");
  LOG(INFO) << "connected to Discord as " << client.self().name;

  co_return co_await Serve(client, config);
}

}  // namespace

absl::Status Run(const Config& config) {
  ABSL_ASSIGN_OR_RETURN(EventLoop loop, EventLoop::Create());

  return loop.Run(ConnectAndServe(config));
}

Task<absl::Status> Serve(discord::Client& client, const Config& config,
                         StartClaude start) {
  Bot bot(client, config, std::move(start));

  for (;;) {
    CO_ASSIGN_OR_RETURN(const discord::Event event,
                        co_await client.NextEvent());

    // One event going wrong is no reason to stop serving.
    absl::Status handled;
    if (const auto* created = std::get_if<discord::MessageCreated>(&event)) {
      handled = co_await bot.OnMessage(created->message);
    } else if (const auto* invoked =
                   std::get_if<discord::CommandInvoked>(&event)) {
      handled = co_await bot.OnCommand(*invoked);
    } else if (const auto* reacted =
                   std::get_if<discord::ReactionAdded>(&event)) {
      handled = co_await bot.OnReaction(*reacted);
    }
    if (!handled.ok()) LOG(ERROR) << "could not handle an event: " << handled;
  }
}

}  // namespace bot
