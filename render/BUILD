load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# What Claude Code says, as Discord messages. Nothing here waits or does I/O.
package(default_visibility = ["//bot:__pkg__"])

cc_library(
    name = "render",
    srcs = ["render.cc"],
    hdrs = ["render.h"],
    deps = [
        "@abseil-cpp",
        "@bedrock//:json",
        "@claude_code_co//:claude_code",
    ],
)

cc_test(
    name = "render_test",
    size = "small",
    srcs = ["render_test.cc"],
    deps = [
        ":render",
        "@abseil-cpp",
        "@bedrock//:json",
        "@bedrock//:testing_main",
        "@claude_code_co//:claude_code",
        "@gbenchmark",
        "@gtest",
    ],
)
