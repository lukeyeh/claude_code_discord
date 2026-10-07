load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

# For rules_nixpkgs, which reads the revisions of bedrock and claude_code_co
# from it.
exports_files(["flake.lock"])

cc_binary(
    name = "claude_code_discord",
    srcs = ["main.cc"],
    deps = [
        "//bot",
        "@abseil-cpp",
    ],
)

alias(
    name = "compile_commands",
    actual = "@wolfd_bazel_compile_commands//:generate_compile_commands",
)
