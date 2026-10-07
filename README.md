# claude_code_discord

Claude Code in Discord. What you say to the bot, by mentioning it in any
channel it can see, is said to Claude Code running on this machine, and what
Claude says and does is posted back as it happens. Built on [bedrock](https://github.com/lukeyeh/bedrock)
and [claude_code_co](https://github.com/lukeyeh/claude_code_co).

- `@claude_code what does this project do?` is a prompt. Messages that do
  not mention the bot are not for it, and Claude never sees them.
- While Claude is working the bot shows as typing.
- A channel is one conversation, which goes on from one prompt to the next.
- The channel is shown nothing about the machine: tools that work on it
  (Bash, Read, Edit and so on) are noted by name only, and Claude is asked
  to keep paths and the like out of what it says.
- Links Claude posts stay plain links, with no preview cards under them.
- The tools Claude uses are noted in small print, in one message that is
  edited as it works: the latest five, and a count of the earlier ones.
- When Claude wants to use a tool it has not been allowed, the bot asks,
  with ✅ and ❌ on the question: press one to answer. `/allow` and
  `/deny` answer everything it is waiting on at once.
- `/stop` cuts a turn short. `/new` forgets the conversation. The commands
  appear in a server once you have said something there.

**Whoever the bot listens to can read, write and run things on this machine
as you.** It listens only to the people in `CLAUDE_USER_IDS`, and will not
start without them. It hears them in every channel it can see, so invite it
only where you want it.

`CLAUDE_PERMISSIONS` says what Claude may do unasked. `ask`, the default,
has it ask in the channel. `auto` has Claude Code judge each tool use for
itself and refuse what looks unsafe, with nobody asked: `claude auto-mode
config` shows the rules it goes by, which the `autoMode` section of your
Claude Code settings changes.

## Running it

1. In the [Discord Developer Portal](https://discord.com/developers/applications)
   make an application with a bot, turn on the **Message Content** intent,
   and invite it to your server with the `bot` and `applications.commands`
   scopes and permission to read and send messages and add reactions.
2. Install [Claude Code](https://claude.com/claude-code) and log in: the bot
   runs the `claude` it finds on `PATH`, as you, with your settings.
3. Copy `.env.example` to `.env` and fill it in. Your id comes from
   Discord's "Copy User ID", which Developer Mode turns on.
4. `nix develop` (or `direnv allow`, which also loads `.env`), then

   ```
   bazel run //:claude_code_discord
   ```

   Without direnv: `set -a; . ./.env; set +a` first.

## Layout

| Directory | What it is |
| --- | --- |
| `bot/` | The bot: its configuration, and what joins Discord to Claude Code. |
| `render/` | What Claude Code says, as Discord messages. |

Each file's `_test.cc` shows what it does. `bot/bot_test.cc` runs the whole
bot against a scripted Discord and a scripted Claude Code.

## Development

```
bazel test //...
bazel test --config=epoll //...
bazel run :compile_commands  # compile_commands.json, for clangd
```

Run Bazel inside the Nix shell: the compiler comes from there. Libraries come
from nixpkgs through `nix/deps.nix`, and bedrock and claude_code_co from the
revisions `flake.lock` pins; `nix flake update bedrock claude_code_co` moves
to their latest.

Nix only sees files Git knows about. After adding a file, `git add -N .`
before the next command that reads the flake.
