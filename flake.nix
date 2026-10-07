{
  description = "claude_code_discord: Claude Code in a Discord channel";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs?ref=nixos-unstable";

    # The libraries the bot is built on. Not used as a flake: nix/deps.nix
    # builds it from the revision flake.lock pins here, with this project's
    # compiler. `nix flake update bedrock` moves to its latest revision.
    bedrock = {
      url = "github:lukeyeh/bedrock";
      flake = false;
    };

    # Claude Code as a coroutine library, taken the same way.
    claude_code_co = {
      url = "github:lukeyeh/claude_code_co";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, ... }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      devShells = forAllSystems (pkgs:
        let
          # The compiler Bazel builds with. Bazel picks up $CC from this shell,
          # so the exact clang version is whatever flake.lock pins for this
          # LLVM major version.
          llvm = pkgs.llvmPackages_21;
        in
        {
          default = (pkgs.mkShell.override { stdenv = llvm.stdenv; }) {
            packages = [
              # nixpkgs' plain `bazel` is an older major version.
              pkgs.bazel_9
              # buildifier, the BUILD file formatter.
              pkgs.bazel-buildtools
              # clangd, clang-format and clang-tidy, matching the compiler.
              llvm.clang-tools
            ];
          };
        });
    };
}
