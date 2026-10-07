# The libraries the project, its tests and its benchmarks are built against.
#
# This is the one place they are chosen. Bazel takes them from here (through
# bazel.nix) at the versions flake.lock pins.
pkgs:
let
  stdenv = pkgs.llvmPackages_21.stdenv;
  abseil-cpp = pkgs.abseil-cpp.override { cxxStandard = "20"; };

  # Bedrock and claude_code_co live in repositories of their own. They are
  # inputs of the flake, so flake.lock pins a revision of each, and these
  # fetch those revisions.
  lock = builtins.fromJSON (builtins.readFile ../flake.lock);
  bedrock-source =
    let
      locked = lock.nodes.${lock.nodes.root.inputs.bedrock}.locked;
    in
    builtins.fetchTarball {
      url = "https://github.com/${locked.owner}/${locked.repo}/archive/${locked.rev}.tar.gz";
      sha256 = locked.narHash;
    };
  claude-code-co-source =
    let
      locked = lock.nodes.${lock.nodes.root.inputs.claude_code_co}.locked;
    in
    builtins.fetchTarball {
      url = "https://github.com/${locked.owner}/${locked.repo}/archive/${locked.rev}.tar.gz";
      sha256 = locked.narHash;
    };

  bedrock = pkgs.callPackage "${bedrock-source}/nix/package.nix" {
    inherit stdenv abseil-cpp;
    liburing = pkgs.liburing;
    openssl = pkgs.openssl;
    sqlite = pkgs.sqlite;
    gtest = pkgs.gtest;
    gbenchmark = pkgs.gbenchmark;
  };
in
{
  # Built as C++20 like the project, so that the two agree on which standard
  # library types Abseil's own types stand for.
  inherit abseil-cpp;

  # What bedrock is built against: io_uring, TLS for the connections to
  # Discord, and SQLite, which nothing here uses but bedrock's build expects.
  liburing = pkgs.liburing;
  openssl = pkgs.openssl;
  sqlite = pkgs.sqlite;

  # Coroutines, the event loop, HTTP, WebSocket, JSON and Discord. Built
  # above by the package.nix it carries, with this project's compiler and
  # against the libraries here, so that the whole program is built one way.
  inherit bedrock;

  # Claude Code as a conversation. Built by its own package.nix too, against
  # the same bedrock.
  claude_code_co = pkgs.callPackage "${claude-code-co-source}/nix/package.nix" {
    inherit stdenv bedrock;
  };

  # For tests and benchmarks only.
  gtest = pkgs.gtest;
  gbenchmark = pkgs.gbenchmark;
}
