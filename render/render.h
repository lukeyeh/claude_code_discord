// What Claude Code says, as Discord messages. This is the one place that
// decides how a turn reads in a channel, and that knows what Discord will
// accept as a message.

#ifndef RENDER_RENDER_H_
#define RENDER_RENDER_H_

#include <string>
#include <vector>

#include "protocol/message.h"

namespace render {

// One thing to post.
struct Post {
  std::string text;
  // The post is a note of a tool Claude is using, rather than something
  // said. Notes come thick and fast, and read best gathered together. One
  // is always a single line.
  bool is_tool_note = false;

  friend bool operator==(const Post&, const Post&) = default;
};

// What to post in the channel about `message`, in order. Often nothing: much
// of what Claude Code says is not worth a reader's time.
//
//  - What Claude says is posted as it stands, and each tool it uses is noted
//    in small print, in a post of its own. A note names the tool. It says
//    what the tool is being used for only where that gives away nothing
//    about the machine Claude Code runs on: a web search says what for, a
//    command or a file does not say which.
//  - A request for permission asks about the tool, in the same terms.
//  - The end of a turn is marked, with what it cost; one that ended badly
//    says why.
//
// Every post is one Discord will take: not empty, and no longer than its
// limit, longer text being split over several, at the end of a line where
// there is one. Nothing in a post summons everyone in the channel, whatever
// Claude wrote.
std::vector<Post> Posts(const claude_code::Message& message);

}  // namespace render

#endif  // RENDER_RENDER_H_
