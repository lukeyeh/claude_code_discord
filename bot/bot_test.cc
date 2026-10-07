// The bot by example. Discord is played by bedrock's fakes, which send the
// bot what people say and record what it does about it, and Claude Code by
// a fake of this file's own, which says what the test has it say each time
// the bot writes to it.

#include "bot/bot.h"

#include <benchmark/benchmark.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "async/awaitable.h"
#include "async/task.h"
#include "bot/config.h"
#include "cli/cli.h"
#include "discord/client.h"
#include "discord/model.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "http/client.h"
#include "json/json.h"
#include "net/event_loop.h"
#include "net/stream.h"
#include "session/session.h"
#include "websocket/fake_server.h"

namespace {

using absl_testing::StatusIs;
using testing::ElementsAre;
using testing::IsEmpty;
using testing::SizeIs;

constexpr std::string_view kApi = "https://discord.com/api/v10";

void RunOnEventLoop(Task<> test) {
  absl::StatusOr<EventLoop> loop = EventLoop::Create();
  ABSL_ASSERT_OK(loop);
  loop->Run(std::move(test));
}

// -----------------------------------------------------------------------------
// Claude Code, played by the test
// -----------------------------------------------------------------------------

// Lines Claude Code says, as the real one writes them.
constexpr char kInit[] =
    R"({"type":"system","subtype":"init","session_id":"abc"})";
constexpr char kFour[] =
    R"({"type":"assistant","message":{"content":[{"type":"text","text":"4"}]}})";
constexpr char kResult[] =
    R"({"type":"result","subtype":"success","num_turns":1,"result":"4"})";
constexpr char kAsksToRemove[] =
    R"({"type":"control_request","request_id":"req_1","request":{)"
    R"("subtype":"can_use_tool","tool_name":"Bash","tool_use_id":"use_1",)"
    R"("input":{"command":"rm a.txt"}}})";

// A Claude Code that follows a script. Each time the bot writes to it, a
// prompt or an answer or an interruption, it says the next thing the test
// told it to, there and then, so that by the time the bot's write returns
// the bot has already dealt with the reply.
class FakeClaude {
 public:
  // What it says the next time it is written to: these lines.
  void Says(std::vector<std::string> lines) {
    script_.push_back(Reply{
        .lines = std::move(lines),
    });
  }

  // The next time it is written to it says `lines` and then exits.
  void SaysThenStops(std::vector<std::string> lines) {
    script_.push_back(Reply{
        .lines = std::move(lines),
        .stops = true,
    });
  }

  // From now on it cannot be started.
  void CannotBeStarted() { can_start_ = false; }

  // What to give the bot in place of claude_code::Start.
  bot::StartClaude starter() {
    return [this](const claude_code::Options& options)
               -> absl::StatusOr<std::unique_ptr<claude_code::Session>> {
      if (!can_start_) return absl::NotFoundError("cannot run claude");

      started_.push_back(options);
      return std::make_unique<claude_code::Session>(
          std::make_unique<Stream>(*this));
    };
  }

  // How it was started, once for each time it was.
  const std::vector<claude_code::Options>& started() const { return started_; }

  // Everything the bot wrote to it, a line at a time, oldest first.
  const std::vector<json::Value>& heard() const { return heard_; }

 private:
  struct Reply {
    std::vector<std::string> lines;
    bool stops = false;
  };

  // The bot's end of one run of Claude Code.
  class Stream final : public net::Stream {
   public:
    explicit Stream(FakeClaude& claude) : claude_(claude) {}

    // Waits, for as long as it takes, for the script to say something. Only
    // a deadline that is nearly here is kept, and then at once: that is the
    // bot asking whether there is anything more just now, and the answer is
    // that there is not.
    Task<absl::StatusOr<size_t>> Read(std::span<char> buffer,
                                      net::Deadline deadline) override {
      if (unread_.empty() && !stopped_ &&
          deadline < net::After(std::chrono::milliseconds(500))) {
        co_return absl::DeadlineExceededError("nothing more just now");
      }
      co_await Arrival(*this);

      const size_t taken = unread_.copy(buffer.data(), buffer.size());
      unread_.erase(0, taken);
      co_return taken;
    }

    Task<absl::Status> Write(std::string_view data) override {
      for (const std::string_view line :
           absl::StrSplit(data, '\n', absl::SkipEmpty())) {
        const absl::StatusOr<json::Value> document = json::Parse(line);
        ABSL_EXPECT_OK(document);
        if (document.ok()) claude_.heard_.push_back(*document);
      }

      if (!claude_.script_.empty()) {
        const Reply reply = std::move(claude_.script_.front());
        claude_.script_.pop_front();
        for (const std::string& line : reply.lines) {
          absl::StrAppend(&unread_, line, "\n");
        }
        stopped_ = reply.stops;

        // The bot's relay carries on from here, inside this call.
        if (reading_) std::exchange(reader_, Waker()).Wake();
      }

      co_return absl::OkStatus();
    }

   private:
    // Waits until there is something to read, or never will be.
    class Arrival : public Awaitable<Arrival> {
     public:
      explicit Arrival(Stream& stream) : stream_(stream) {}

      bool Ready() const {
        return !stream_.unread_.empty() || stream_.stopped_;
      }
      void Start(Waker waker) {
        stream_.reader_ = waker;
        stream_.reading_ = true;
      }
      void Finish() const { stream_.reading_ = false; }

     private:
      Stream& stream_;
    };

    FakeClaude& claude_;
    std::string unread_;
    bool stopped_ = false;
    // The task waiting in Read, if `reading_`.
    Waker reader_;
    bool reading_ = false;
  };

  std::deque<Reply> script_;
  bool can_start_ = true;
  std::vector<claude_code::Options> started_;
  std::vector<json::Value> heard_;
};

// -----------------------------------------------------------------------------
// Discord, played by bedrock's fakes
// -----------------------------------------------------------------------------

// Discord's HTTP API: remembers what the bot asked of it, and answers the
// two things whose answers the bot reads. Each message posted is given the
// next id, from 1001.
class FakeDiscordApi final : public http::Client {
 public:
  Task<absl::StatusOr<http::Response>> Send(
      const http::Request& request) override {
    requests_.push_back(request);

    if (request.url.ends_with("/gateway/bot")) {
      co_return http::Response{
          .status = 200,
          .body = R"({"url":"wss://gateway.test"})",
      };
    }
    if (request.method == http::Method::kPost &&
        request.url.ends_with("/messages")) {
      co_return http::Response{
          .status = 200,
          .body = absl::StrCat(R"({"id":")", next_message_++, R"("})"),
      };
    }
    co_return http::Response{
        .status = 204,
    };
  }

  const std::vector<http::Request>& requests() const { return requests_; }

 private:
  std::vector<http::Request> requests_;
  int next_message_ = 1001;
};

// Someone posting a message, as the gateway reports it. The defaults are
// one of the bot's people, in channel 22 of server 33, speaking to the bot,
// which is user 99.
struct Said {
  int sequence = 0;
  // Whether the message mentions the bot, ahead of `content`.
  bool to_bot = true;
  int guild = 33;
  int channel = 22;
  int author = 44;
  bool bot = false;
  std::string_view content;
};

std::string Dispatch(const Said& said) {
  return absl::StrCat(
      R"({"op":0,"t":"MESSAGE_CREATE","s":)", said.sequence, R"(,"d":{"id":")",
      said.sequence, R"(","channel_id":")", said.channel, R"(","guild_id":")",
      said.guild, R"(","content":")", said.to_bot ? "<@99> " : "", said.content,
      R"(","author":{"id":")", said.author, R"(","username":"luke","bot":)",
      said.bot ? "true" : "false", "}}}");
}

// Someone using a slash command. `options` is the JSON of the options they
// filled in.
struct Used {
  int sequence = 0;
  int channel = 22;
  int author = 44;
  std::string_view command;
  std::string_view options = "[]";
};

std::string Dispatch(const Used& used) {
  return absl::StrCat(
      R"({"op":0,"t":"INTERACTION_CREATE","s":)", used.sequence,
      R"(,"d":{"type":2,"id":")", used.sequence, R"(","token":"token-)",
      used.sequence, R"(","channel_id":")", used.channel,
      R"(","guild_id":"33","data":{"name":")", used.command, R"(","options":)",
      used.options, R"(},"member":{"user":{"id":")", used.author,
      R"(","username":"luke"}}}})");
}

// Someone reacting to a message.
struct Reacted {
  int sequence = 0;
  int author = 44;
  int message = 0;
  std::string_view emoji;
};

std::string Dispatch(const Reacted& reacted) {
  return absl::StrCat(R"({"op":0,"t":"MESSAGE_REACTION_ADD","s":)",
                      reacted.sequence, R"(,"d":{"user_id":")", reacted.author,
                      R"(","channel_id":"22","guild_id":"33","message_id":")",
                      reacted.message, R"(","member":{"user":{"id":")",
                      reacted.author,
                      R"(","username":"luke"}},"emoji":{"id":null,"name":")",
                      reacted.emoji, R"("}}})");
}

constexpr char kTick[] = "\xE2\x9C\x85";
constexpr char kCross[] = "\xE2\x9D\x8C";

// What the bot did, as Discord's API saw it, in words:
//
//   "edit 1001: hi" it changed message 1001 to say "hi"
//   "seen 2"        it marked message 2 with the eyes
//   "yes? 1001"     it put a tick on message 1001, to be pressed
//   "no? 1001"      and a cross
//   "22: hello"     it posted "hello" in channel 22, with links in it not
//                   to be previewed; "(previewed)" is added if they are
//   "reply: hello"  it answered a command with "hello"
//   "commands: 33"  it offered its commands in server 33
//   "typing 22"     it showed itself as typing in channel 22
//
// The last kind is left out unless `typing` asks for it: it accompanies
// every turn, and has a test of its own.
enum class Typing : uint8_t {
  kLeftOut,
  kIncluded,
};

std::vector<std::string> Actions(const FakeDiscordApi& http, Typing typing) {
  std::vector<std::string> actions;
  for (const http::Request& request : http.requests()) {
    const std::vector<std::string_view> path =
        absl::StrSplit(std::string_view(request.url).substr(kApi.size()), '/');
    const absl::StatusOr<json::Value> body = json::Parse(request.body);

    if (request.method == http::Method::kPut && path.size() > 5 &&
        path[5] == "reactions") {
      // The emoji is in the path, encoded.
      const std::string_view emoji = path.size() > 6 ? path[6] : "";
      actions.push_back(absl::StrCat(emoji == "%E2%9C%85"   ? "yes? "
                                     : emoji == "%E2%9D%8C" ? "no? "
                                                            : "seen ",
                                     path[4]));
    } else if (request.method == http::Method::kPut && path.size() == 6 &&
               path[3] == "guilds") {
      actions.push_back(absl::StrCat("commands: ", path[4]));
    } else if (request.method == http::Method::kPatch && path.size() == 5 &&
               body.ok()) {
      actions.push_back(
          absl::StrCat("edit ", path[4], ": ", (*body)["content"].AsString(),
                       (*body)["flags"].AsInt() == 4 ? "" : " (previewed)"));
    } else if (request.method == http::Method::kPost && path.size() == 4 &&
               path[3] == "typing") {
      if (typing == Typing::kIncluded) {
        actions.push_back(absl::StrCat("typing ", path[2]));
      }
    } else if (request.method == http::Method::kPost && path.size() == 4 &&
               path[3] == "messages" && body.ok()) {
      actions.push_back(
          absl::StrCat(path[2], ": ", (*body)["content"].AsString(),
                       (*body)["flags"].AsInt() == 4 ? "" : " (previewed)"));
    } else if (request.method == http::Method::kPost && path.size() > 1 &&
               path[1] == "interactions" && body.ok()) {
      actions.push_back(
          absl::StrCat("reply: ", (*body)["data"]["content"].AsString()));
    }
  }

  return actions;
}

// The bot listens to user 44.
bot::Config Listening() {
  return bot::Config{
      .token = "secret",
      .users =
          {
              discord::UserId{
                  .value = 44,
              },
          },
      .claude =
          claude_code::Options{
              .directory = "/work",
              .permissions = claude_code::Permissions::kAsk,
          },
  };
}

// Runs the bot against a Discord that sends `dispatches` and then revokes
// the bot's token, which is what ends Serve. Evaluates to what the bot did.
Task<std::vector<std::string>> ServeEvents(std::vector<std::string> dispatches,
                                           FakeClaude& claude,
                                           Typing typing = Typing::kLeftOut) {
  auto http = std::make_unique<FakeDiscordApi>();
  const FakeDiscordApi& requests = *http;

  websocket::FakeServer gateway;
  gateway.Sends(R"({"op":10,"d":{"heartbeat_interval":3600000}})");
  gateway.Sends(R"({"op":0,"s":1,"t":"READY","d":{"session_id":"abc",
      "resume_gateway_url":"wss://resume.test",
      "user":{"id":"99","username":"claude","bot":true},
      "application":{"id":"77"}}})");
  for (std::string& dispatch : dispatches) gateway.Sends(std::move(dispatch));
  gateway.Closes(4004);

  absl::StatusOr<discord::Client> client = co_await discord::Client::Connect(
      "secret", std::move(http), gateway.connector());
  ABSL_EXPECT_OK(client);
  if (!client.ok()) co_return std::vector<std::string>{};

  const bot::Config config = Listening();
  EXPECT_THAT(co_await bot::Serve(*client, config, claude.starter()),
              StatusIs(absl::StatusCode::kUnauthenticated));

  co_return Actions(requests, typing);
}

// -----------------------------------------------------------------------------
// Tests
// -----------------------------------------------------------------------------

// The whole point: what someone says in the channel is said to Claude, and
// what Claude says comes back to the channel, with the end of the turn
// marked.
TEST(BotTest, RelaysATurn) {
  RunOnEventLoop([]() -> Task<> {
    FakeClaude claude;
    claude.Says({
        kInit,
        kFour,
        kResult,
    });

    const std::vector<std::string> actions = co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .content = "What is 2 + 2?",
            }),
        },
        claude);

    EXPECT_THAT(actions, ElementsAre("commands: 33", "seen 2", "22: 4",
                                     "22: -# Done in 1 turn, $0.00 so far"));
    EXPECT_THAT(claude.heard(), SizeIs(1));
    if (claude.heard().size() != 1) co_return;
    EXPECT_EQ(claude.heard()[0]["message"]["content"].AsString(),
              "What is 2 + 2?");
  }());
}

// While Claude is at work the bot appears to be typing: from the moment it
// has a prompt, and again after each thing it posts, since Discord takes a
// post to mean the typing is over. Once the turn ends, so does the typing.
TEST(BotTest, ShowsTypingWhileClaudeIsAtWork) {
  RunOnEventLoop([]() -> Task<> {
    FakeClaude claude;
    claude.Says({
        kInit,
        kFour,
        kResult,
    });

    const std::vector<std::string> actions = co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .content = "What is 2 + 2?",
            }),
        },
        claude, Typing::kIncluded);

    EXPECT_THAT(actions, ElementsAre("commands: 33", "seen 2", "typing 22",
                                     "22: 4", "typing 22",
                                     "22: -# Done in 1 turn, $0.00 so far"));
  }());
}

// Claude Code is started as the configuration says: where to work, and to
// ask before doing what it has not been allowed.
TEST(BotTest, StartsClaudeCodeAsConfigured) {
  RunOnEventLoop([]() -> Task<> {
    FakeClaude claude;

    co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .content = "hello",
            }),
        },
        claude);

    EXPECT_THAT(claude.started(), SizeIs(1));
    if (claude.started().size() != 1) co_return;
    EXPECT_EQ(claude.started()[0].directory, "/work");
    EXPECT_EQ(claude.started()[0].permissions, claude_code::Permissions::kAsk);
    EXPECT_EQ(claude.started()[0].resume, "");
    // Claude is told its words are public, and to keep the machine out of
    // them.
    EXPECT_THAT(claude.started()[0].instructions,
                testing::HasSubstr("Do not reveal anything about the machine"));
  }());
}

// A channel is one conversation: a second message there goes to the Claude
// Code that heard the first. Another channel has a conversation of its own.
TEST(BotTest, HoldsOneConversationInEachChannel) {
  RunOnEventLoop([]() -> Task<> {
    FakeClaude claude;

    co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .content = "first",
            }),
            Dispatch(Said{
                .sequence = 3,
                .content = "second",
            }),
            Dispatch(Said{
                .sequence = 4,
                .channel = 23,
                .content = "elsewhere",
            }),
        },
        claude);

    EXPECT_THAT(claude.started(), SizeIs(2));
    EXPECT_THAT(claude.heard(), SizeIs(3));
  }());
}

// Whoever the bot listens to can act on its machine, so it listens only to
// its people, and never to a bot, itself included.
TEST(BotTest, IgnoresEveryoneElse) {
  RunOnEventLoop([]() -> Task<> {
    FakeClaude claude;

    const std::vector<std::string> actions = co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .author = 45,
                .content = "a stranger",
            }),
            Dispatch(Said{
                .sequence = 3,
                .bot = true,
                .content = "a bot",
            }),
            Dispatch(Used{
                .sequence = 4,
                .author = 45,
                .command = "stop",
            }),
        },
        claude);

    EXPECT_THAT(claude.started(), IsEmpty());
    EXPECT_THAT(actions, ElementsAre("reply: That is not for you."));
  }());
}

// The bot is spoken to by mentioning it. Anything else its people say is
// theirs: Claude is not started for it and never hears it, and a mention
// with nothing after it says nothing.
TEST(BotTest, HearsOnlyWhatIsSaidToIt) {
  RunOnEventLoop([]() -> Task<> {
    FakeClaude claude;

    const std::vector<std::string> actions = co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .to_bot = false,
                .content = "talking to someone else",
            }),
            Dispatch(Said{
                .sequence = 3,
                .content = "",
            }),
            // A mention need not come first, and is not part of the prompt.
            Dispatch(Said{
                .sequence = 4,
                .to_bot = false,
                .content = "what is 2 + 2, <@99>?",
            }),
        },
        claude);

    EXPECT_THAT(actions, ElementsAre("commands: 33", "seen 4"));
    EXPECT_THAT(claude.heard(), SizeIs(1));
    if (claude.heard().size() != 1) co_return;
    EXPECT_EQ(claude.heard()[0]["message"]["content"].AsString(),
              "what is 2 + 2, ?");
  }());
}

// The bot needs no telling where to listen: its people are heard in any
// channel it can see. Its commands are offered in a server the first time
// one of them speaks there, and not again.
TEST(BotTest, ListensInAnyChannelAndOffersItsCommandsThere) {
  RunOnEventLoop([]() -> Task<> {
    FakeClaude claude;

    const std::vector<std::string> actions = co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .channel = 22,
                .content = "here",
            }),
            Dispatch(Said{
                .sequence = 3,
                .channel = 23,
                .content = "there",
            }),
            Dispatch(Said{
                .sequence = 4,
                .guild = 34,
                .channel = 24,
                .content = "in another server",
            }),
        },
        claude);

    EXPECT_THAT(actions, ElementsAre("commands: 33", "seen 2", "seen 3",
                                     "commands: 34", "seen 4"));
    EXPECT_THAT(claude.started(), SizeIs(3));
  }());
}

// When Claude Code asks whether Claude may use a tool, the bot asks in the
// channel, briefly, with a tick and a cross to press. Pressing the tick is
// the answer, and the turn carries on.
TEST(BotTest, AsksAndTakesATickForYes) {
  RunOnEventLoop([]() -> Task<> {
    FakeClaude claude;
    claude.Says({
        kAsksToRemove,
    });
    claude.Says({
        kFour,
        kResult,
    });

    const std::vector<std::string> actions = co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .content = "Remove a.txt",
            }),
            Dispatch(Reacted{
                .sequence = 3,
                .message = 1001,
                .emoji = kTick,
            }),
        },
        claude);

    EXPECT_THAT(actions,
                ElementsAre("commands: 33", "seen 2", "22: Allow **Bash**?",
                            "yes? 1001", "no? 1001", "22: 4",
                            "22: -# Done in 1 turn, $0.00 so far"));
    EXPECT_THAT(claude.heard(), SizeIs(2));
    if (claude.heard().size() != 2) co_return;
    const json::Value& answer = claude.heard()[1]["response"];
    EXPECT_EQ(answer["request_id"].AsString(), "req_1");
    EXPECT_EQ(answer["response"]["behavior"].AsString(), "allow");
  }());
}

// A line as Claude Code writes it when Claude uses `tool` on `argument`.
std::string Uses(std::string_view tool, std::string_view argument) {
  return absl::StrCat(
      R"({"type":"assistant","message":{"content":[{"type":"tool_use",)",
      R"("id":"use","name":")", tool, R"(","input":{"query":")", argument,
      R"("}}]}})");
}

// Claude at work uses tool after tool. Their notes are gathered into one
// message, which is brought up to date by editing it, so that a busy turn
// is a few lines and what people say meanwhile is not buried. Notes that
// arrive together are shown together. Only the latest five are listed, and
// the ones before them counted. What Claude says ends the message, and
// later notes start another.
TEST(BotTest, GathersTheNotesOfToolsIntoOneMessage) {
  RunOnEventLoop([]() -> Task<> {
    FakeClaude claude;
    claude.Says({
        Uses("WebSearch", "one"),
        Uses("WebSearch", "two"),
    });
    claude.Says({
        Uses("WebSearch", "three"),
        Uses("WebSearch", "four"),
        Uses("WebSearch", "five"),
        Uses("WebSearch", "six"),
        Uses("WebSearch", "seven"),
        kFour,
        Uses("WebSearch", "eight"),
        kResult,
    });

    const std::vector<std::string> actions = co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .content = "Look these up",
            }),
            // Something more for Claude, which is what has the fake say the
            // rest. It lands in the channel below the notes so far.
            Dispatch(Said{
                .sequence = 3,
                .content = "and this",
            }),
        },
        claude);

    EXPECT_THAT(
        actions,
        ElementsAre("commands: 33", "seen 2",
                    "22: -# **WebSearch** one\n-# **WebSearch** two", "seen 3",
                    "edit 1001: -# 2 earlier tool uses"
                    "\n-# **WebSearch** three\n-# **WebSearch** four"
                    "\n-# **WebSearch** five\n-# **WebSearch** six"
                    "\n-# **WebSearch** seven",
                    "22: 4", "22: -# **WebSearch** eight",
                    "22: -# Done in 1 turn, $0.00 so far"));
  }());
}

// A cross refuses, and Claude is told who said no.
TEST(BotTest, TakesACrossForNo) {
  RunOnEventLoop([]() -> Task<> {
    FakeClaude claude;
    claude.Says({
        kAsksToRemove,
    });

    const std::vector<std::string> actions = co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .content = "Remove a.txt",
            }),
            Dispatch(Reacted{
                .sequence = 3,
                .message = 1001,
                .emoji = kCross,
            }),
        },
        claude);

    EXPECT_THAT(actions,
                ElementsAre("commands: 33", "seen 2", "22: Allow **Bash**?",
                            "yes? 1001", "no? 1001"));
    EXPECT_THAT(claude.heard(), SizeIs(2));
    if (claude.heard().size() != 2) co_return;
    const json::Value& answer = claude.heard()[1]["response"]["response"];
    EXPECT_EQ(answer["behavior"].AsString(), "deny");
    EXPECT_EQ(answer["message"].AsString(), "luke said no.");
  }());
}

// An answer is a tick or a cross, from one of the bot's people, on the
// message that asks. Anyone else's, any other emoji, a reaction to some
// other message, and the bot's own two, which are only there to be pressed,
// answer nothing. Nor does a second press once the first has answered.
TEST(BotTest, TakesOnlyItsPeoplesAnswers) {
  RunOnEventLoop([]() -> Task<> {
    FakeClaude claude;
    claude.Says({
        kAsksToRemove,
    });

    co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .content = "Remove a.txt",
            }),
            Dispatch(Reacted{
                .sequence = 3,
                .author = 99,
                .message = 1001,
                .emoji = kTick,
            }),
            Dispatch(Reacted{
                .sequence = 4,
                .author = 45,
                .message = 1001,
                .emoji = kTick,
            }),
            Dispatch(Reacted{
                .sequence = 5,
                .message = 1001,
                .emoji = "\xF0\x9F\x91\x8D",
            }),
            Dispatch(Reacted{
                .sequence = 6,
                .message = 555,
                .emoji = kTick,
            }),
        },
        claude);
    // Only the prompt: nothing was answered.
    EXPECT_THAT(claude.heard(), SizeIs(1));

    FakeClaude answered;
    answered.Says({
        kAsksToRemove,
    });
    co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .content = "Remove a.txt",
            }),
            Dispatch(Reacted{
                .sequence = 3,
                .message = 1001,
                .emoji = kCross,
            }),
            Dispatch(Reacted{
                .sequence = 4,
                .message = 1001,
                .emoji = kTick,
            }),
        },
        answered);
    // The prompt and the one answer.
    EXPECT_THAT(answered.heard(), SizeIs(2));
  }());
}

// Claude waiting to be told whether it may use a tool is not Claude at
// work: the typing stops while the channel decides, and starts again with
// the answer.
TEST(BotTest, StopsTypingWhileItWaitsForAnAnswer) {
  RunOnEventLoop([]() -> Task<> {
    FakeClaude claude;
    claude.Says({
        kAsksToRemove,
    });
    claude.Says({
        kFour,
    });

    const std::vector<std::string> actions = co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .content = "Remove a.txt",
            }),
            Dispatch(Used{
                .sequence = 3,
                .command = "allow",
            }),
        },
        claude, Typing::kIncluded);

    EXPECT_THAT(actions,
                ElementsAre("commands: 33", "seen 2", "typing 22",
                            "22: Allow **Bash**?", "yes? 1001", "no? 1001",
                            "22: 4", "typing 22", "reply: Allowed Bash."));
  }());
}

// Claude may want several tools at once. Claude Code asks about each and
// goes no further until every one is answered. Each can be answered with
// its own tick or cross; /allow and /deny answer all that are waiting.
TEST(BotTest, AnswersEverythingClaudeIsWaitingOn) {
  RunOnEventLoop([]() -> Task<> {
    FakeClaude claude;
    claude.Says({
        R"({"type":"control_request","request_id":"req_1","request":{)"
        R"("subtype":"can_use_tool","tool_name":"WebSearch","input":{}}})",
        R"({"type":"control_request","request_id":"req_2","request":{)"
        R"("subtype":"can_use_tool","tool_name":"WebSearch","input":{}}})",
        R"({"type":"control_request","request_id":"req_3","request":{)"
        R"("subtype":"can_use_tool","tool_name":"Read","input":{}}})",
    });

    const std::vector<std::string> actions = co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .content = "Look these up",
            }),
            Dispatch(Used{
                .sequence = 3,
                .command = "allow",
            }),
        },
        claude);

    EXPECT_FALSE(actions.empty());
    if (actions.empty()) co_return;
    EXPECT_EQ(actions.back(),
              "reply: Allowed all 3: WebSearch, WebSearch, Read.");

    // The prompt, then an answer to each request, in the order asked.
    EXPECT_THAT(claude.heard(), SizeIs(4));
    if (claude.heard().size() != 4) co_return;
    EXPECT_EQ(claude.heard()[1]["response"]["request_id"].AsString(), "req_1");
    EXPECT_EQ(claude.heard()[2]["response"]["request_id"].AsString(), "req_2");
    EXPECT_EQ(claude.heard()[3]["response"]["request_id"].AsString(), "req_3");
    EXPECT_EQ(claude.heard()[3]["response"]["response"]["behavior"].AsString(),
              "allow");
  }());
}

// A command is the first the bot hears from a server when it has only just
// started, so that is a moment to offer its commands there too: they may
// have changed since the server last saw them.
TEST(BotTest, OffersItsCommandsWhereACommandIsUsed) {
  RunOnEventLoop([]() -> Task<> {
    FakeClaude claude;

    const std::vector<std::string> actions = co_await ServeEvents(
        {
            Dispatch(Used{
                .sequence = 2,
                .command = "stop",
            }),
        },
        claude);

    EXPECT_THAT(actions,
                ElementsAre("reply: Claude is not running.", "commands: 33"));
  }());
}

// /deny refuses, and Claude is told why if a reason is given.
TEST(BotTest, DeniesWithAReason) {
  RunOnEventLoop([]() -> Task<> {
    FakeClaude claude;
    claude.Says({
        kAsksToRemove,
    });

    const std::vector<std::string> actions = co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .content = "Remove a.txt",
            }),
            Dispatch(Used{
                .sequence = 3,
                .command = "deny",
                .options = R"([{"name":"reason","type":3,"value":"keep it"}])",
            }),
            // Already answered: there is nothing left to allow.
            Dispatch(Used{
                .sequence = 4,
                .command = "allow",
            }),
        },
        claude);

    EXPECT_EQ(actions.size(), 7);
    if (actions.size() != 7) co_return;
    EXPECT_EQ(actions[5], "reply: Refused Bash.");
    EXPECT_EQ(actions[6], "reply: Claude has not asked for anything.");
    EXPECT_THAT(claude.heard(), SizeIs(2));
    if (claude.heard().size() != 2) co_return;
    const json::Value& answer = claude.heard()[1]["response"]["response"];
    EXPECT_EQ(answer["behavior"].AsString(), "deny");
    EXPECT_EQ(answer["message"].AsString(), "keep it");
  }());
}

// /stop interrupts the turn under way. Claude Code then ends it as it ends
// any turn, which the channel sees.
TEST(BotTest, StopInterruptsClaude) {
  RunOnEventLoop([]() -> Task<> {
    FakeClaude claude;
    claude.Says({
        kInit,
    });
    claude.Says({
        R"({"type":"result","subtype":"error_during_execution",)"
        R"("is_error":true,"num_turns":1})",
    });

    const std::vector<std::string> actions = co_await ServeEvents(
        {
            Dispatch(Used{
                .sequence = 2,
                .command = "stop",
            }),
            Dispatch(Said{
                .sequence = 3,
                .content = "Count to a million",
            }),
            Dispatch(Used{
                .sequence = 4,
                .command = "stop",
            }),
        },
        claude);

    EXPECT_THAT(
        actions,
        ElementsAre("reply: Claude is not running.", "commands: 33", "seen 3",
                    "22: The turn ended early.",
                    "22: -# Done in 1 turn, $0.00 so far", "reply: Stopping."));
    EXPECT_THAT(claude.heard(), SizeIs(2));
    if (claude.heard().size() != 2) co_return;
    EXPECT_EQ(claude.heard()[1]["request"]["subtype"].AsString(), "interrupt");
  }());
}

// /new forgets the conversation: the next message starts Claude Code again,
// and not where the last one left off.
TEST(BotTest, NewStartsAnotherConversation) {
  RunOnEventLoop([]() -> Task<> {
    FakeClaude claude;
    claude.Says({
        kInit,
    });

    const std::vector<std::string> actions = co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .content = "first",
            }),
            Dispatch(Used{
                .sequence = 3,
                .command = "new",
            }),
            Dispatch(Said{
                .sequence = 4,
                .content = "second",
            }),
        },
        claude);

    EXPECT_THAT(actions,
                ElementsAre("commands: 33", "seen 2",
                            "reply: Forgotten. Your next message starts a new "
                            "conversation.",
                            "seen 4"));
    EXPECT_THAT(claude.started(), SizeIs(2));
    if (claude.started().size() != 2) co_return;
    EXPECT_EQ(claude.started()[1].resume, "");
  }());
}

// If Claude Code exits, the channel is told, and the next message starts it
// again in the same conversation.
TEST(BotTest, PicksUpWhereAStoppedClaudeCodeLeftOff) {
  RunOnEventLoop([]() -> Task<> {
    FakeClaude claude;
    claude.SaysThenStops({
        kInit,
    });

    const std::vector<std::string> actions = co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .content = "first",
            }),
            Dispatch(Said{
                .sequence = 3,
                .content = "second",
            }),
        },
        claude);

    EXPECT_THAT(actions,
                ElementsAre("commands: 33", "seen 2",
                            "22: Claude Code has stopped. Your next message "
                            "starts it again, where it left off.",
                            "seen 3"));
    EXPECT_THAT(claude.started(), SizeIs(2));
    if (claude.started().size() != 2) co_return;
    EXPECT_EQ(claude.started()[1].resume, "abc");
  }());
}

// A Claude Code that cannot be started, because it is not installed, say,
// is reported where it was asked for, and does not stop the bot. The channel
// is not told why: the reason may name what is on the machine.
TEST(BotTest, SaysSoWhenClaudeCodeCannotBeStarted) {
  RunOnEventLoop([]() -> Task<> {
    FakeClaude claude;
    claude.CannotBeStarted();

    const std::vector<std::string> actions = co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .content = "hello",
            }),
        },
        claude);

    EXPECT_THAT(actions, ElementsAre("commands: 33",
                                     "22: Claude Code could not be started."));
  }());
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //bot:bot_test -- --benchmark_filter=all

Task<> ServeOneTurn(benchmark::State& state) {
  for (auto _ : state) {
    FakeClaude claude;
    claude.Says({
        kInit,
        kFour,
        kResult,
    });

    benchmark::DoNotOptimize(co_await ServeEvents(
        {
            Dispatch(Said{
                .sequence = 2,
                .content = "What is 2 + 2?",
            }),
        },
        claude));
  }
}

// Connecting, one message in and Claude's answer out: everything the bot
// itself adds to a turn, which should be nothing beside Claude.
void BM_ServeOneTurn(benchmark::State& state) {
  EventLoop::Create()->Run(ServeOneTurn(state));
}
BENCHMARK(BM_ServeOneTurn);

}  // namespace
