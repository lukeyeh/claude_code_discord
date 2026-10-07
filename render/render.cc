#include "render/render.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_replace.h"
#include "json/json.h"
#include "protocol/message.h"

namespace render {
namespace {

// The most Discord allows in one message. It counts characters, of which
// there are never more than bytes, so keeping to this many bytes is safe.
constexpr size_t kMaxPostBytes = 2000;

// How much of a tool's arguments to show. Enough to see what is being done;
// a file being written can run to pages.
constexpr size_t kMaxArgumentBytes = 300;

// Whether a character starts at `text[at]`, as opposed to carrying on.
// UTF-8 marks the bytes that carry on with the bits 10.
bool StartsCharacter(std::string_view text, size_t at) {
  return (static_cast<unsigned char>(text[at]) & 0xC0) != 0x80;
}

// The longest beginning of `text` that fits in `limit` bytes without cutting
// a character in two.
std::string_view Beginning(std::string_view text, size_t limit) {
  if (text.size() <= limit) return text;

  size_t end = limit;
  while (end > 0 && !StartsCharacter(text, end)) --end;

  return text.substr(0, end);
}

// `text` with its end left off, and marked as left off, if it is longer than
// `limit` bytes.
std::string Shortened(std::string_view text, size_t limit) {
  if (text.size() <= limit) return std::string(text);

  return absl::StrCat(Beginning(text, limit), "…");
}

// `text` so that Discord notifies nobody: a zero-width space after the @ of
// the two words that would summon the whole channel.
std::string Quietly(std::string_view text) {
  return absl::StrReplaceAll(text, {
                                       {
                                           "@everyone",
                                           "@​everyone",
                                       },
                                       {
                                           "@here",
                                           "@​here",
                                       },
                                   });
}

// Adds `text` to `posts`, as however many posts it takes.
void Add(std::string_view text, std::vector<Post>& posts) {
  const std::string quiet = Quietly(text);
  std::string_view rest = quiet;

  while (!rest.empty()) {
    std::string_view piece = Beginning(rest, kMaxPostBytes);

    // Break after the last line that fits, if there is more than one.
    const size_t line_end = piece.rfind('\n');
    if (piece.size() < rest.size() && line_end != std::string_view::npos &&
        line_end > 0) {
      piece = piece.substr(0, line_end + 1);
    }
    rest.remove_prefix(piece.size());

    // Discord refuses a message with nothing to show.
    if (piece.find_first_not_of(" \n\t") != std::string_view::npos) {
      posts.push_back(Post{
          .text = std::string(piece),
      });
    }
  }
}

// One line of small print: Discord shows a line that starts "-# " small and
// grey.
std::string SmallPrint(std::string_view text) {
  return absl::StrCat("-# ", absl::StrReplaceAll(text, {
                                                           {
                                                               "\n",
                                                               " ",
                                                           },
                                                       }));
}

// The tools whose use can be described without giving away anything about
// the machine Claude Code runs on, and the argument of each that says what
// it is being used for. These reach outwards, to the web or to Claude; the
// rest work on the machine, and their arguments are its commands and paths.
struct Describable {
  std::string_view tool;
  std::string_view argument;
};
constexpr Describable kDescribable[] = {
    {
        .tool = "WebSearch",
        .argument = "query",
    },
    {
        .tool = "WebFetch",
        .argument = "url",
    },
    {
        .tool = "ToolSearch",
        .argument = "query",
    },
    {
        .tool = "Skill",
        .argument = "skill",
    },
    {
        .tool = "Agent",
        .argument = "description",
    },
    {
        .tool = "Task",
        .argument = "description",
    },
};

// What a tool is being used for, in a line: the tool, and for one that is
// safe to describe, the argument that says most. For every other tool the
// name is all: what it is given says too much about the machine, and a tool
// not known here is taken to be one of those.
std::string Describe(const claude_code::ToolUse& use) {
  for (const Describable& describable : kDescribable) {
    if (use.name != describable.tool) continue;

    const std::string& value = use.input[describable.argument].AsString();
    if (value.empty()) break;
    return absl::StrCat("**", use.name, "** ",
                        Shortened(value, kMaxArgumentBytes));
  }

  return absl::StrCat("**", use.name, "**");
}

}  // namespace

std::vector<Post> Posts(const claude_code::Message& message) {
  std::vector<Post> posts;

  switch (message.kind()) {
    case claude_code::Message::Kind::kAssistant: {
      const claude_code::Assistant& said = message.assistant();
      Add(said.text, posts);
      for (const claude_code::ToolUse& use : said.tool_uses) {
        posts.push_back(Post{
            .text = Quietly(SmallPrint(Describe(use))),
            .is_tool_note = true,
        });
      }
      break;
    }

    case claude_code::Message::Kind::kPermissionRequest: {
      const claude_code::ToolUse& use = message.permission_request().tool_use;
      posts.push_back(Post{
          .text = Quietly(absl::StrCat("Allow ", Describe(use), "?")),
      });
      break;
    }

    case claude_code::Message::Kind::kResult: {
      const claude_code::Result& result = message.result();
      if (result.is_error) {
        Add(absl::StrCat("The turn ended early",
                         result.text.empty() ? "." : ": ", result.text),
            posts);
      }
      Add(SmallPrint(absl::StrFormat(
              "Done in %d %s, $%.2f so far", result.turns,
              result.turns == 1 ? "turn" : "turns", result.cost_usd)),
          posts);
      break;
    }

    // The channel has seen the prompt, and has no use for the rest.
    case claude_code::Message::Kind::kInit:
    case claude_code::Message::Kind::kTextDelta:
    case claude_code::Message::Kind::kToolResults:
    case claude_code::Message::Kind::kOther: break;
  }

  return posts;
}

}  // namespace render
