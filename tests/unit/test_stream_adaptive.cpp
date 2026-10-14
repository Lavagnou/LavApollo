/**
 * @file tests/unit/test_stream_adaptive.cpp
 * @brief Test src/stream_adaptive.*
 */
#include "../tests_common.h"

#include <array>
#include <src/stream_adaptive.h>

using namespace std::literals;
using stream::adaptive_controller_t;
using stream::frame_fec_status_t;

namespace {
  using clock = adaptive_controller_t::clock;

  adaptive_controller_t::config_t make_config(bool bitrate_supported = true) {
    adaptive_controller_t::config_t config;
    config.adaptive_fec = true;
    config.adaptive_bitrate = true;
    config.base_fec_percentage = 20;
    config.negotiated_bitrate_kbps = 20000;
    config.encoder_supports_bitrate_change = bitrate_supported;
    return config;
  }

  frame_fec_status_t make_status(std::uint16_t total_data, std::uint16_t total_parity, std::uint16_t received_data, std::uint16_t received_parity) {
    frame_fec_status_t status {};
    status.total_data_packets = total_data;
    status.total_parity_packets = total_parity;
    status.received_data_packets = received_data;
    status.received_parity_packets = received_parity;
    return status;
  }

  // A frame that lost two data shards, repaired from ten parity shards: well within margin.
  const auto repaired = make_status(50, 10, 48, 10);
  // Lost six data shards out of ten parity: repaired, but with little margin left.
  const auto near_miss = make_status(50, 10, 44, 10);
  // Not enough shards to rebuild the frame.
  const auto unrecovered = make_status(50, 10, 30, 5);

  /**
   * Run `seconds` one-second windows of 60 frames each, calling `per_window` once per window
   * before its frames are sent. Returns the time after the last window.
   */
  template<class F>
  clock::time_point run_windows(adaptive_controller_t &controller, clock::time_point now, int seconds, F &&per_window) {
    for (int s = 0; s < seconds; ++s) {
      per_window();
      for (int f = 0; f < 60; ++f) {
        controller.on_frame_sent();
        now += 1000ms / 60;
        controller.evaluate(now);
      }
    }
    return now;
  }

  const auto nothing = [] {
  };
}  // namespace

TEST(FrameFecStatusTests, ParsesBigEndianFields) {
  // clang-format off
  const std::array<unsigned char, 21> wire {
    0x00, 0x01, 0x02, 0x03,  // frameIndex
    0x10, 0x20,  // highestReceivedSequenceNumber
    0x10, 0x10,  // nextContiguousSequenceNumber
    0x00, 0x03,  // missingPacketsBeforeHighestReceived
    0x00, 0x32,  // totalDataPackets = 50
    0x00, 0x0A,  // totalParityPackets = 10
    0x00, 0x2F,  // receivedDataPackets = 47
    0x00, 0x09,  // receivedParityPackets = 9
    20,  // fecPercentage
    1,  // multiFecBlockIndex
    3,  // multiFecBlockCount
  };
  // clang-format on

  auto status = stream::parse_frame_fec_status({(const char *) wire.data(), wire.size()});
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(status->frame_index, 0x00010203u);
  EXPECT_EQ(status->highest_received_sequence_number, 0x1020);
  EXPECT_EQ(status->next_contiguous_sequence_number, 0x1010);
  EXPECT_EQ(status->missing_packets_before_highest_received, 3);
  EXPECT_EQ(status->total_data_packets, 50);
  EXPECT_EQ(status->total_parity_packets, 10);
  EXPECT_EQ(status->received_data_packets, 47);
  EXPECT_EQ(status->received_parity_packets, 9);
  EXPECT_EQ(status->fec_percentage, 20);
  EXPECT_EQ(status->multi_fec_block_index, 1);
  EXPECT_EQ(status->multi_fec_block_count, 3);
}

TEST(FrameFecStatusTests, RejectsShortPayload) {
  const std::array<char, 20> wire {};
  EXPECT_FALSE(stream::parse_frame_fec_status({wire.data(), wire.size()}).has_value());
  EXPECT_FALSE(stream::parse_frame_fec_status({}).has_value());
}

TEST(AdaptiveControllerTests, StartsFromConfiguredValues) {
  adaptive_controller_t controller {make_config(), clock::now()};
  EXPECT_EQ(controller.fec_percentage(), 20);
  EXPECT_EQ(controller.bitrate_kbps(), 20000);
  EXPECT_EQ(controller.fec_floor(), 10);
  EXPECT_EQ(controller.fec_ceiling(), 55);
  EXPECT_EQ(controller.bitrate_floor_kbps(), 5000);
  EXPECT_TRUE(controller.bitrate_active());
}

TEST(AdaptiveControllerTests, CleanNetworkLowersFecDownToFloor) {
  adaptive_controller_t controller {make_config(), clock::now()};
  auto now = clock::now();

  // Nothing moves before ten quiet seconds.
  now = run_windows(controller, now, 9, nothing);
  EXPECT_EQ(controller.fec_percentage(), 20);

  now = run_windows(controller, now, 2, nothing);
  EXPECT_EQ(controller.fec_percentage(), 15);

  // One step every ten seconds, never below the floor.
  run_windows(controller, now, 60, nothing);
  EXPECT_EQ(controller.fec_percentage(), 10);
  EXPECT_EQ(controller.bitrate_kbps(), 20000);
}

TEST(AdaptiveControllerTests, RepairedLossDoesNotRaiseFec) {
  // Losses the FEC absorbs with room to spare are not a reason to spend more on FEC:
  // steering on raw loss is what used to drive FEC to its ceiling on healthy Wi-Fi.
  adaptive_controller_t controller {make_config(), clock::now()};
  run_windows(controller, clock::now(), 20, [&] {
    for (int i = 0; i < 5; ++i) {
      controller.on_fec_status(repaired);
    }
  });
  EXPECT_LE(controller.fec_percentage(), 20);
}

TEST(AdaptiveControllerTests, NearMissesRaiseFec) {
  adaptive_controller_t controller {make_config(), clock::now()};
  run_windows(controller, clock::now(), 3, [&] {
    controller.on_fec_status(near_miss);
    controller.on_fec_status(near_miss);
  });
  EXPECT_EQ(controller.fec_percentage(), 30);
}

TEST(AdaptiveControllerTests, SingleBurstIsIgnored) {
  adaptive_controller_t controller {make_config(), clock::now()};
  int window = 0;
  run_windows(controller, clock::now(), 5, [&] {
    if (window++ == 1) {
      controller.on_fec_status(unrecovered);
    }
  });
  EXPECT_EQ(controller.fec_percentage(), 20);
  EXPECT_EQ(controller.bitrate_kbps(), 20000);
}

TEST(AdaptiveControllerTests, SustainedLossRaisesFecToCeilingThenCutsBitrate) {
  adaptive_controller_t controller {make_config(), clock::now()};
  auto lossy = [&] {
    controller.on_fec_status(unrecovered);
  };

  // FEC first: +10 per second after two troubled windows, until the ceiling.
  auto now = run_windows(controller, clock::now(), 4, lossy);
  EXPECT_EQ(controller.fec_percentage(), 40);
  EXPECT_EQ(controller.bitrate_kbps(), 20000);

  now = run_windows(controller, now, 3, lossy);
  EXPECT_EQ(controller.fec_percentage(), 55);

  // Then bitrate, 20% at a time, no more than every two seconds, never below the floor.
  now = run_windows(controller, now, 2, lossy);
  EXPECT_LT(controller.bitrate_kbps(), 20000);

  run_windows(controller, now, 60, lossy);
  EXPECT_EQ(controller.bitrate_kbps(), controller.bitrate_floor_kbps());
  EXPECT_EQ(controller.fec_percentage(), 55);
}

TEST(AdaptiveControllerTests, RecoveryRequestsCountAsLoss) {
  adaptive_controller_t controller {make_config(), clock::now()};
  run_windows(controller, clock::now(), 3, [&] {
    controller.on_recovery_request();
  });
  EXPECT_GT(controller.fec_percentage(), 20);
}

TEST(AdaptiveControllerTests, BitrateRestoresAfterQuietPeriod) {
  adaptive_controller_t controller {make_config(), clock::now()};
  auto now = run_windows(controller, clock::now(), 20, [&] {
    controller.on_fec_status(unrecovered);
  });
  const auto lowered = controller.bitrate_kbps();
  ASSERT_LT(lowered, 20000);

  // Fifteen clean seconds before the first step back up.
  now = run_windows(controller, now, 14, nothing);
  EXPECT_EQ(controller.bitrate_kbps(), lowered);

  now = run_windows(controller, now, 2, nothing);
  EXPECT_EQ(controller.bitrate_kbps(), lowered + 2000);

  // And all the way back, but never above what the client negotiated.
  run_windows(controller, now, 600, nothing);
  EXPECT_EQ(controller.bitrate_kbps(), 20000);
}

TEST(AdaptiveControllerTests, UnsupportedEncoderKeepsBitrate) {
  adaptive_controller_t controller {make_config(false), clock::now()};
  EXPECT_FALSE(controller.bitrate_active());

  run_windows(controller, clock::now(), 30, [&] {
    controller.on_fec_status(unrecovered);
  });
  EXPECT_EQ(controller.fec_percentage(), 55);
  EXPECT_EQ(controller.bitrate_kbps(), 20000);
}

TEST(AdaptiveControllerTests, FixedFecCutsBitrateDirectly) {
  auto config = make_config();
  config.adaptive_fec = false;
  adaptive_controller_t controller {config, clock::now()};

  run_windows(controller, clock::now(), 3, [&] {
    controller.on_fec_status(unrecovered);
  });
  EXPECT_EQ(controller.fec_percentage(), 20);
  EXPECT_LT(controller.bitrate_kbps(), 20000);
}

TEST(AdaptiveControllerTests, FloorAndCeilingFollowConfiguredFec) {
  auto config = make_config();
  config.base_fec_percentage = 80;
  adaptive_controller_t high {config, clock::now()};
  EXPECT_EQ(high.fec_floor(), 10);
  EXPECT_EQ(high.fec_ceiling(), 80);

  config.base_fec_percentage = 5;
  adaptive_controller_t low {config, clock::now()};
  EXPECT_EQ(low.fec_floor(), 5);
  EXPECT_EQ(low.fec_ceiling(), 55);
}

TEST(AdaptiveControllerTests, LowNegotiatedBitrateFloorNeverExceedsIt) {
  auto config = make_config();
  config.negotiated_bitrate_kbps = 300;
  adaptive_controller_t controller {config, clock::now()};
  EXPECT_EQ(controller.bitrate_floor_kbps(), 300);
}
