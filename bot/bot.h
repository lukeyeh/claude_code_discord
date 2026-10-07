// The bot: Claude Code in a Discord channel.
//
// In any channel the bot can see, what one of its people says to it, by
// mentioning it, is said to Claude, and what Claude says and does is posted
// back as it happens. A channel is one conversation, which goes on from one
// such message to the next. Messages that do not mention the bot are not
// for it, and Claude never sees them.
//
// While Claude is at work on a turn the bot appears to be typing, as a
// person would who was writing a reply.
//
// The channel is told nothing about the machine Claude Code runs on: not
// the commands run there, the files touched, or why Claude Code would not
// start. Claude is asked to keep it out of what it says, too.
//
// Links in what the bot posts are plain links: Discord is told not to
// preview the pages under them.
//
// The tools Claude uses are noted in small print, in one message that is
// kept up to date as it goes: the latest few, and a count of the rest, so
// that a busy turn does not fill the channel.
//
// When Claude Code asks whether Claude may use a tool, the bot asks in the
// channel with a tick and a cross on the question, and one of its people
// pressing either is the answer.
//
// It also answers slash commands, which it offers in a server once one of
// its people has spoken there:
//
//   /stop   cuts short what Claude is doing
//   /new    forgets the conversation; the next message starts another
//   /allow  lets Claude use everything it is asking about
//   /deny   refuses all of it, with a reason for Claude if one is given
//
// Everyone else is ignored.

#ifndef BOT_BOT_H_
#define BOT_BOT_H_

#include <functional>
#include <memory>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "async/task.h"
#include "bot/config.h"
#include "cli/cli.h"
#include "discord/client.h"
#include "session/session.h"

namespace bot {

// Starts Claude Code for a conversation. claude_code::Start is the real one;
// a test gives the bot a Claude of its own making.
using StartClaude =
    std::function<absl::StatusOr<std::unique_ptr<claude_code::Session>>(
        const claude_code::Options& options)>;

// Runs the bot as configured, on the calling thread, until it cannot go on.
// Never returns OK: the status says why it stopped, which is either that it
// could not start (Discord cannot be reached or does not accept the token)
// or that Discord has since turned it away.
absl::Status Run(const Config& config);

// The bot's work, given its connection to Discord: handles events from
// `client` until Discord turns the bot away, and evaluates to why.
//
// Nothing short of that stops it. A message that cannot be
// handled is logged and passed over, and a Claude Code that will not start
// or has stopped is reported in the channel and started again by the next
// message there.
//
// Conversations go on being relayed by tasks of their own, which use
// `client` after this returns for as long as the event loop runs.
Task<absl::Status> Serve(discord::Client& client, const Config& config,
                         StartClaude start = &claude_code::Start);

}  // namespace bot

#endif  // BOT_BOT_H_
