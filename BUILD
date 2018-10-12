package(default_visibility = ["//visibility:public"])

licenses(["notice"])  # BSD

exports_files(["LICENSE"])

cc_library(
    name = "pcc_monitor_interval_queue_lib",
    srcs = [
        "pcc_monitor_interval_queue.cc",
    ],
    hdrs = [
        "pcc_monitor_interval_queue.h",
    ],
    deps = [
        "//base",
        "//third_party/quic/core:quic_bandwidth_lib",
        "//third_party/quic/core:quic_connection_stats_lib",
        "//third_party/quic/core:quic_time_lib",
        "//third_party/quic/core:quic_types_lib",
        "//third_party/quic/core/congestion_control:quic_congestion_control_hdrs",
        "//third_party/quic/core/congestion_control:rtt_stats_lib",
        "//third_party/quic/platform/api:quic_logging_lib",
        "//third_party/quic/platform/api:quic_str_cat_lib",
    ],
)

cc_library(
    name = "pcc_utility_manager_lib",
    srcs = [
        "pcc_utility_manager.cc",
    ],
    hdrs = [
        "pcc_utility_manager.h",
    ],
    deps = [
        ":pcc_monitor_interval_queue_lib",
        "//base",
        "//third_party/quic/core:quic_bandwidth_lib",
        "//third_party/quic/core:quic_time_lib",
        "//third_party/quic/core:quic_types_lib",
        "//third_party/quic/core/congestion_control:rtt_stats_lib",
    ],
)

cc_library(
    name = "pcc_sender_lib",
    srcs = [
        "pcc_sender.cc",
    ],
    hdrs = [
        "pcc_sender.h",
    ],
    deps = [
        ":pcc_monitor_interval_queue_lib",
        ":pcc_utility_manager_lib",
        "//base",
        "//third_party/quic/core:quic_bandwidth_lib",
        "//third_party/quic/core:quic_connection_stats_lib",
        "//third_party/quic/core:quic_time_lib",
        "//third_party/quic/core:quic_types_lib",
        "//third_party/quic/core:quic_unacked_packet_map_lib",
        "//third_party/quic/core/congestion_control:bandwidth_sampler_lib",
        "//third_party/quic/core/congestion_control:quic_congestion_control_hdrs",
        "//third_party/quic/core/congestion_control:rtt_stats_lib",
        "//third_party/quic/core/congestion_control:windowed_filter_lib",
        "//third_party/quic/platform/api:quic_flag_utils_lib",
        "//third_party/quic/platform/api:quic_logging_lib",
        "//third_party/quic/platform/api:quic_str_cat_lib",
    ],
)

cc_test(
    name = "pcc_monitor_interval_queue_test",
    srcs = [
        "pcc_monitor_interval_queue_test.cc",
    ],
    deps = [
        ":pcc_monitor_interval_queue_lib",
        "//testing/base/public:gunit_main",
        "//third_party/quic/core:quic_time_lib",
        "//third_party/quic/core:quic_types_lib",
        "//third_party/quic/platform/api:quic_logging_lib",
        "//third_party/quic/platform/api:quic_test_lib",
    ],
)

cc_test(
    name = "pcc_sender_test",
    srcs = [
        "pcc_sender_test.cc",
    ],
    deps = [
        ":pcc_sender_lib",
        "//testing/base/public:gunit_main",
        "//third_party/quic/core/congestion_control:rtt_stats_lib",
        "//third_party/quic/platform/api:quic_logging_lib",
        "//third_party/quic/platform/api:quic_test_lib",
        "//third_party/quic/test_tools:mock_clock",
        "//third_party/quic/test_tools:quic_sent_packet_manager_peer",
        "//third_party/quic/test_tools:quic_test_utils",
        "//third_party/quic/test_tools/simulator:quic_endpoint_lib",
        "//third_party/quic/test_tools/simulator:simulator_lib",
    ],
)
