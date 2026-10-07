// How a turn with Claude reads in a Discord channel: which of the things
// Claude Code says are posted, and as what.

#include "render/render.h"

#include <benchmark/benchmark.h>

#include <string>
#include <utility>
#include <vector>

#include "absl/status/status_matchers.h"
#include "absl/status/statusor.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "json/json.h"
#include "protocol/message.h"

namespace {

using claude_code::Message;
using testing::ElementsAre;
using testing::IsEmpty;
using testing::SizeIs;

json::Value Json(const std::string& text) {
  const absl::StatusOr<json::Value> document = json::Parse(text);
  ABSL_EXPECT_OK(document);

  return document.ok() ? *document : json::Value();
}

// A post that is not about a tool.
render::Post Text(std::string text) {
  return render::Post{
      .text = std::move(text),
  };
}

// What Claude says is posted as it said it.
TEST(PostsTest, PostsWhatClaudeSays) {
  EXPECT_THAT(render::Posts(Message(claude_code::Assistant{
                  .text = "The tests pass.",
              })),
              ElementsAre(Text("The tests pass.")));
}

// The tools Claude uses are noted after its words, in small print, so that
// a reader can follow what it is doing without it drowning out what it says.
// Each is marked as a note, for whoever posts them to gather together.
TEST(PostsTest, NotesEachToolInSmallPrint) {
  EXPECT_THAT(render::Posts(Message(claude_code::Assistant{
                  .text = "Let me look.",
                  .tool_uses =
                      {
                          claude_code::ToolUse{
                              .id = "use_1",
                              .name = "Bash",
                              .input = Json(R"({"command":"ls"})"),
                          },
                          claude_code::ToolUse{
                              .id = "use_2",
                              .name = "Read",
                              .input = Json(R"({"file_path":"a.txt"})"),
                          },
                      },
              })),
              ElementsAre(Text("Let me look."),
                          render::Post{
                              .text = "-# **Bash**",
                              .is_tool_note = true,
                          },
                          render::Post{
                              .text = "-# **Read**",
                              .is_tool_note = true,
                          }));
}

// The text of the one post made for `use`.
std::string NoteOf(claude_code::ToolUse use) {
  const std::vector<render::Post> posts =
      render::Posts(Message(claude_code::Assistant{
          .tool_uses =
              {
                  std::move(use),
              },
      }));
  EXPECT_THAT(posts, SizeIs(1));

  return posts.empty() ? "" : posts[0].text;
}

// A tool that reaches outwards is noted with what it is being used for:
// the one argument that says so, without the JSON around it.
TEST(PostsTest, NotesWhatAnOutwardToolIsFor) {
  EXPECT_EQ(NoteOf(claude_code::ToolUse{
                .name = "WebSearch",
                .input = Json(R"({"query":"why are chickens orange",
                                  "mode":"standard"})"),
            }),
            "-# **WebSearch** why are chickens orange");
  EXPECT_EQ(NoteOf(claude_code::ToolUse{
                .name = "WebFetch",
                .input = Json(R"({"url":"https://example.com/",
                                  "prompt":"Summarise"})"),
            }),
            "-# **WebFetch** https://example.com/");
  EXPECT_EQ(NoteOf(claude_code::ToolUse{
                .name = "Agent",
                .input = Json(R"({"description":"Research tyres",
                                  "prompt":"Look in /home/me/notes"})"),
            }),
            "-# **Agent** Research tyres");
}

// A tool that works on the machine is noted by name alone. The channel is
// not shown the commands run there or the files touched: they say what is
// on the machine and where.
TEST(PostsTest, SaysNothingOfTheMachine) {
  EXPECT_EQ(NoteOf(claude_code::ToolUse{
                .name = "Bash",
                .input = Json(R"({"command":"ls /home/me/secret-project",
                                  "description":"List the project"})"),
            }),
            "-# **Bash**");
  EXPECT_EQ(NoteOf(claude_code::ToolUse{
                .name = "Read",
                .input = Json(R"({"file_path":"/home/me/.ssh/config"})"),
            }),
            "-# **Read**");
  EXPECT_EQ(NoteOf(claude_code::ToolUse{
                .name = "Grep",
                .input = Json(R"({"pattern":"TODO","path":"/home/me/src"})"),
            }),
            "-# **Grep**");
}

// A tool this does not know, as one from an MCP server may be, is treated
// the same way: what it is given might be anything.
TEST(PostsTest, NotesAnUnknownToolByNameAlone) {
  EXPECT_EQ(NoteOf(claude_code::ToolUse{
                .name = "mcp__files__open",
                .input = Json(R"({"query":"/home/me/diary.txt"})"),
            }),
            "-# **mcp__files__open**");
}

// What a tool is for can run to pages. Only the beginning is shown.
TEST(PostsTest, ShortensALongDescription) {
  const std::string note = NoteOf(claude_code::ToolUse{
      .name = "WebSearch",
      .input = json::Value().Set("query", std::string(5000, 'x')),
  });

  EXPECT_LT(note.size(), 400);
  EXPECT_THAT(note, testing::EndsWith("…"));
}

// Discord takes 2000 characters at most. Longer text goes out as several
// posts, broken between lines so that each still reads well.
TEST(PostsTest, SplitsLongTextBetweenLines) {
  const std::string line = std::string(999, 'a') + "\n";

  EXPECT_THAT(render::Posts(Message(claude_code::Assistant{
                  .text = line + line + line,
              })),
              ElementsAre(Text(line + line), Text(line)));
}

// Text with nowhere to break is broken anyway, but never in the middle of a
// character: every post is still text.
TEST(PostsTest, NeverSplitsACharacter) {
  // 1500 characters of three bytes each.
  std::string text;
  for (int i = 0; i < 1500; ++i) text += "€";

  const std::vector<render::Post> posts =
      render::Posts(Message(claude_code::Assistant{
          .text = text,
      }));

  ASSERT_THAT(posts, SizeIs(3));
  for (const render::Post& post : posts) EXPECT_LE(post.text.size(), 2000);
  EXPECT_EQ(posts[0].text.size() % 3, 0);
  EXPECT_EQ(posts[0].text + posts[1].text + posts[2].text, text);
}

// Claude deciding to address everyone must not ping a whole server.
TEST(PostsTest, SummonsNobody) {
  EXPECT_THAT(render::Posts(Message(claude_code::Assistant{
                  .text = "Hello @everyone and @here",
              })),
              ElementsAre(Text("Hello @​everyone and @​here")));
}

// When Claude Code asks, the question names the tool, and like a note says
// nothing of the machine.
TEST(PostsTest, PutsARequestForPermissionAsAQuestion) {
  EXPECT_THAT(render::Posts(Message(claude_code::PermissionRequest{
                  .id = "req_1",
                  .tool_use =
                      claude_code::ToolUse{
                          .id = "use_1",
                          .name = "Bash",
                          .input = Json(R"({"command":"rm a.txt"})"),
                      },
              })),
              ElementsAre(render::Post{
                  .text = "Allow **Bash**?",
              }));
}

// The end of a turn is marked, so that the channel knows Claude has stopped
// and what the conversation has cost. The answer itself is not repeated:
// Claude has already said it.
TEST(PostsTest, MarksTheEndOfATurn) {
  EXPECT_THAT(render::Posts(Message(claude_code::Result{
                  .text = "The tests pass.",
                  .cost_usd = 0.126,
                  .turns = 3,
              })),
              ElementsAre(Text("-# Done in 3 turns, $0.13 so far")));
}

// A turn that ended badly says why.
TEST(PostsTest, SaysWhyATurnEndedEarly) {
  EXPECT_THAT(render::Posts(Message(claude_code::Result{
                  .text = "Reached the turn limit",
                  .is_error = true,
                  .turns = 1,
              })),
              ElementsAre(Text("The turn ended early: Reached the turn limit"),
                          Text("-# Done in 1 turn, $0.00 so far")));
}

// Most of what Claude Code says is its own business, and nothing is posted:
// how it is set up, the output of tools, progress reports.
TEST(PostsTest, SaysNothingOfTheRest) {
  EXPECT_THAT(render::Posts(Message(claude_code::Init{})), IsEmpty());
  EXPECT_THAT(render::Posts(Message(claude_code::ToolResults{})), IsEmpty());
  EXPECT_THAT(render::Posts(Message(claude_code::Other{})), IsEmpty());
  EXPECT_THAT(render::Posts(Message(claude_code::TextDelta{
                  .text = "Hel",
              })),
              IsEmpty());
}

// Claude sometimes uses a tool without a word. Discord refuses an empty
// message, so none is posted.
TEST(PostsTest, PostsNothingForNoText) {
  EXPECT_THAT(render::Posts(Message(claude_code::Assistant{
                  .text = "\n",
              })),
              IsEmpty());
}

// -----------------------------------------------------------------------------
// Benchmarks
// -----------------------------------------------------------------------------
//
//   bazel run -c opt //render:render_test -- --benchmark_filter=all

// Rendering a long answer, which is split into several posts: done once for
// everything Claude says.
void BM_PostsOfALongAnswer(benchmark::State& state) {
  std::string text;
  for (int i = 0; i < 200; ++i) text += "A line of what Claude had to say.\n";
  const Message message(claude_code::Assistant{
      .text = text,
  });

  for (auto _ : state) benchmark::DoNotOptimize(render::Posts(message));
}
BENCHMARK(BM_PostsOfALongAnswer);

}  // namespace
