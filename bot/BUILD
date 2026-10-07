load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

# The bot itself: what joins Discord to Claude Code.
package(default_visibility = ["//:__pkg__"])

cc_library(
    name = "bot",
    srcs = [
        "bot.cc",
        "config.cc",
    ],
    hdrs = [
        "bot.h",
        "config.h",
    ],
    deps = [
        "//render",
        "@abseil-cpp",
        "@bedrock//:async",
        "@bedrock//:discord",
        "@bedrock//:net",
        "@claude_code_co//:claude_code",
    ],
)

[
    cc_test(
        name = name,
        size = "small",
        srcs = [name + ".cc"],
        deps = [
            ":bot",
            "@abseil-cpp",
            "@bedrock//:async",
            "@bedrock//:discord",
            "@bedrock//:http",
            "@bedrock//:json",
            "@bedrock//:net",
            "@bedrock//:testing_main",
            "@bedrock//:websocket",
            "@claude_code_co//:claude_code",
            "@gbenchmark",
            "@gtest",
        ],
    )
    for name in [
        "bot_test",
        "config_test",
    ]
]
