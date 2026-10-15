/**
 * @file tests/unit/test_mic_passthrough.cpp
 * @brief Test src/mic_passthrough_policy.h
 */
#include "../tests_common.h"

#include <string>
#include <vector>

#include <src/mic_passthrough_policy.h>

using namespace mic_passthrough;

namespace {
  std::string make_message(std::uint16_t sequence, std::uint32_t timestamp, std::size_t opus_bytes) {
    std::string message;
    message.push_back(static_cast<char>(sequence & 0xFF));
    message.push_back(static_cast<char>(sequence >> 8));
    for (int shift = 0; shift < 32; shift += 8) {
      message.push_back(static_cast<char>((timestamp >> shift) & 0xFF));
    }
    message.append(opus_bytes, '\x42');
    return message;
  }

  std::vector<float> ramp(std::size_t count, float start = 1.0f) {
    std::vector<float> samples(count);
    for (std::size_t i = 0; i < count; ++i) {
      samples[i] = start + static_cast<float>(i);
    }
    return samples;
  }
}  // namespace

TEST(MicPassthroughFrame, ParsesLittleEndianHeader) {
  auto message = make_message(0xBEEF, 0x12345678, 120);
  auto frame = parse_frame(message);

  ASSERT_TRUE(frame);
  EXPECT_EQ(frame->sequence, 0xBEEF);
  EXPECT_EQ(frame->timestamp, 0x12345678u);
  EXPECT_EQ(frame->opus.size(), 120u);
  EXPECT_EQ(frame->opus.front(), '\x42');
}

TEST(MicPassthroughFrame, RejectsRuntsAndOversizedFrames) {
  EXPECT_FALSE(parse_frame(std::string_view {}));
  EXPECT_FALSE(parse_frame(make_message(1, 960, 0)));
  EXPECT_TRUE(parse_frame(make_message(1, 960, 1)));
  EXPECT_TRUE(parse_frame(make_message(1, 960, MAX_FRAME_BYTES)));
  EXPECT_FALSE(parse_frame(make_message(1, 960, MAX_FRAME_BYTES + 1)));
}

TEST(MicPassthroughSequence, CountsLostFrames) {
  sequence_tracker_t tracker;

  EXPECT_EQ(tracker.accept(10), 0);
  EXPECT_EQ(tracker.accept(11), 0);
  EXPECT_EQ(tracker.accept(13), 1);
  EXPECT_EQ(tracker.accept(16), 2);
}

TEST(MicPassthroughSequence, DropsLateAndDuplicateFrames) {
  sequence_tracker_t tracker;

  EXPECT_EQ(tracker.accept(20), 0);
  EXPECT_EQ(tracker.accept(22), 1);
  EXPECT_FALSE(tracker.accept(21));  // Arrived after 22: its slot was already concealed
  EXPECT_FALSE(tracker.accept(22));  // Duplicate
  EXPECT_EQ(tracker.accept(23), 0);
}

TEST(MicPassthroughSequence, WrapsAround) {
  sequence_tracker_t tracker;

  EXPECT_EQ(tracker.accept(0xFFFE), 0);
  EXPECT_EQ(tracker.accept(0xFFFF), 0);
  EXPECT_EQ(tracker.accept(0x0001), 1);
}

TEST(MicPassthroughSequence, DoesNotConcealAHugeGap) {
  sequence_tracker_t tracker;

  EXPECT_EQ(tracker.accept(100), 0);
  EXPECT_EQ(tracker.accept(100 + sequence_tracker_t::MAX_CONCEALED_FRAMES + 1), sequence_tracker_t::MAX_CONCEALED_FRAMES);
  EXPECT_EQ(tracker.accept(1000), 0);
}

TEST(MicPassthroughJitter, WaitsForTargetBeforePlaying) {
  jitter_buffer_t buffer(100, 300);
  std::vector<float> out(60, -1.0f);

  auto first = ramp(60);
  buffer.push(first.data(), first.size());
  EXPECT_EQ(buffer.pull(out.data(), out.size()), 0u);
  EXPECT_EQ(out[0], 0.0f);
  EXPECT_FALSE(buffer.playing());

  auto second = ramp(60, 61.0f);
  buffer.push(second.data(), second.size());
  EXPECT_EQ(buffer.pull(out.data(), out.size()), 60u);
  EXPECT_EQ(out[0], 1.0f);
  EXPECT_EQ(out[59], 60.0f);
  EXPECT_TRUE(buffer.playing());
}

TEST(MicPassthroughJitter, UnderrunPadsWithSilenceAndReprimes) {
  jitter_buffer_t buffer(50, 300);
  std::vector<float> out(80, -1.0f);

  auto samples = ramp(60);
  buffer.push(samples.data(), samples.size());
  EXPECT_EQ(buffer.pull(out.data(), out.size()), 60u);
  EXPECT_EQ(out[59], 60.0f);
  EXPECT_EQ(out[60], 0.0f);
  EXPECT_EQ(out[79], 0.0f);
  EXPECT_FALSE(buffer.playing());

  // After running dry, it waits for the target again
  auto few = ramp(10);
  buffer.push(few.data(), few.size());
  EXPECT_EQ(buffer.pull(out.data(), 10), 0u);
  EXPECT_EQ(buffer.level(), 10u);
}

TEST(MicPassthroughJitter, DropsOldestBackToTargetPastMaximum) {
  jitter_buffer_t buffer(100, 300);

  auto samples = ramp(301);
  EXPECT_EQ(buffer.push(samples.data(), samples.size()), 201u);
  EXPECT_EQ(buffer.level(), 100u);

  std::vector<float> out(1);
  buffer.pull(out.data(), out.size());
  EXPECT_EQ(out[0], 202.0f);  // The newest 100 samples were kept
}
