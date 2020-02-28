#pragma once

#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/base/macros.h>

#include "src/common/base/base.h"
#include "src/stirling/bcc_bpf_interface/common.h"
#include "src/stirling/common/parse_state.h"
#include "src/stirling/common/socket_trace.h"
#include "src/stirling/common/utils.h"

namespace pl {
namespace stirling {

struct BufferPosition {
  size_t seq_num;
  size_t offset;
};

// A ParseResult returns a vector of parsed frames, and also some position markers.
//
// It is templated based on the position type, because we have two concepts of position:
//    Position in a contiguous buffer: PositionType is uint64_t.
//    Position in a set of disjoint buffers: PositionType is BufferPosition.
//
// The two concepts are used by two different parse functions we have:
//
// ParseResult<size_t> Parse(MessageType type, std::string_view buf);
// ParseResult<BufferPosition> ParseFrames(MessageType type);
template <typename PositionType>
struct ParseResult {
  // Positions of frame start positions in the source buffer.
  std::vector<PositionType> start_positions;
  // Position of where parsing ended consuming the source buffer.
  // When PositionType is bytes, this is total bytes successfully consumed.
  PositionType end_position;
  // State of the last attempted frame parse.
  ParseState state = ParseState::kInvalid;
};

// NOTE: FindFrameBoundary() and ParseFrames() must be implemented per protocol.

/**
 * Attempt to find the next frame boundary.
 *
 * @tparam TFrameType Message type to search for.
 * @param type request or response.
 * @param buf the buffer in which to search for a frame boundary.
 * @param start_pos A start position from which to search.
 * @return Either the position of a frame start, if found (must be > start_pos),
 * or std::string::npos if no such frame start was found.
 */
template <typename TFrameType>
size_t FindFrameBoundary(MessageType type, std::string_view buf, size_t start_pos);

/**
 * Parses the input string as a sequence of TFrameType, and write the frames to frames.
 *
 * @tparam TFrameType Message type to parse.
 * @param type selects whether to parse for request or response.
 * @param buf the buffer of data to parse as frames.
 * @param frames the parsed frames
 * @return result of the parse, including positions in the source buffer where frames were found.
 */
template <typename TFrameType>
ParseResult<size_t> ParseFrames(MessageType type, std::string_view buf,
                                std::deque<TFrameType>* frames);

/**
 * Utility to convert positions from a position within a set of combined buffers,
 * to the position within a set of matching content in disjoint buffers.
 */
class PositionConverter {
 public:
  PositionConverter() { Reset(); }

  void Reset() {
    curr_seq_ = 0;
    size_ = 0;
  }

  /**
   * @brief Convert position within a set of combined buffers
   * to the position within a set of matching content in disjoint buffers.
   *
   * @param msgs The original set of disjoint buffers.
   * @param pos The position within the combined buffer to convert.
   * @return Position within disjoint buffers, as buffer number and offset within the buffer.
   */
  BufferPosition Convert(const std::vector<std::string_view>& msgs, size_t pos) {
    DCHECK_GE(pos, last_query_pos_)
        << "Position converter cannot go backwards (enforced for performance reasons).";
    // If we ever want to remove the restriction above, the following would do the trick:
    //   if (pos <= last_query_pos_) { Reset(); }

    // Record position of this call, to enforce that we never go backwards.
    last_query_pos_ = pos;

    while (curr_seq_ < msgs.size()) {
      const auto& msg = msgs[curr_seq_];

      // If next frame would cause the crossover,
      // then we have found the point we're looking for.
      if (pos < size_ + msg.size()) {
        return {curr_seq_, pos - size_};
      }

      ++curr_seq_;
      size_ += msg.size();
    }
    return {curr_seq_, 0};
  }

 private:
  // Optimization: keep track of last state, so we can efficiently resume search,
  // so long as the next position to Convert() is after the last one.
  size_t curr_seq_ = 0;
  size_t size_ = 0;
  size_t last_query_pos_ = 0;
};

/**
 * @brief Parses a stream of events traced from write/send/read/recv syscalls,
 * and emits as many complete parsed frames as it can.
 */
template <typename TFrameType>
class EventParser {
 public:
  /**
   * @brief Append raw data to the internal buffer.
   */
  void Append(const SocketDataEvent& event) {
    msgs_.push_back(event.msg);
    ts_nses_.push_back(event.attr.return_timestamp_ns);
    msgs_size_ += event.msg.size();
  }

  /**
   * @brief Parses internal data buffer (see Append()) for frames, and writes resultant
   * parsed frames into the provided frames container.
   *
   * This is a templated function. The caller must provide the type of frame to parsed (e.g.
   * http::Message), and must ensure that there is a corresponding Parse() function with the desired
   * frame type.
   *
   * @param type The Type of frames to parse.
   * @param frames The container to which newly parsed frames are added.
   * @param resync If set to true, Parse will first search for the next frame boundary (even
   * if it is currently at a valid frame boundary).
   *
   * @return ParseResult with locations where parseable frames were found in the source buffer.
   */
  ParseResult<BufferPosition> ParseFrames(MessageType type, std::deque<TFrameType>* frames,
                                          bool resync = false) {
    std::string buf = Combine();

    size_t start_pos = 0;
    if (resync) {
      VLOG(3) << "Finding next frame boundary";
      // Since we've been asked to resync, we search from byte 1 to find a new boundary.
      // Don't want to stay at the same position.
      constexpr int kStartPos = 1;
      start_pos = FindFrameBoundary<TFrameType>(type, buf, kStartPos);

      // Couldn't find a boundary, so stay where we are.
      // Chances are we won't be able to parse, but we have no other option.
      if (start_pos == std::string::npos) {
        start_pos = 0;
      }
    }

    // Grab size before we start, so we know where the new parsed frames are.
    const size_t prev_size = frames->size();

    // Parse and append new frames to the frames vector.
    std::string_view buf_view(buf);
    buf_view.remove_prefix(start_pos);
    ParseResult<size_t> result = stirling::ParseFrames(type, buf_view, frames);
    DCHECK(frames->size() >= prev_size);

    VLOG(3) << absl::Substitute("Parsed $0 new frames", frames->size() - prev_size);

    std::vector<BufferPosition> positions;

    PositionConverter converter;

    // Match timestamps with the parsed frames.
    for (size_t i = 0; i < result.start_positions.size(); ++i) {
      BufferPosition position = converter.Convert(msgs_, start_pos + result.start_positions[i]);
      DCHECK(position.seq_num < msgs_.size()) << absl::Substitute(
          "The sequence number must be in valid range of [0, $0)", msgs_.size());
      positions.push_back(position);

      auto& msg = (*frames)[prev_size + i];
      msg.timestamp_ns = ts_nses_[position.seq_num];
    }

    BufferPosition end_position = converter.Convert(msgs_, start_pos + result.end_position);

    // Reset all state. Call to ParseFrames() is destructive of Append() state.
    msgs_.clear();
    ts_nses_.clear();
    msgs_size_ = 0;

    return {std::move(positions), end_position, result.state};
  }

 private:
  std::string Combine() const {
    std::string result;
    result.reserve(msgs_size_);
    for (auto msg : msgs_) {
      result.append(msg);
    }
    return result;
  }

  // ts_nses_ is the time stamp in nanosecond for the frame in msgs_ with the same indexes.
  std::vector<uint64_t> ts_nses_;
  std::vector<std::string_view> msgs_;

  // The total size of all strings in msgs_. Used to reserve memory space for concatenation.
  size_t msgs_size_ = 0;
};

}  // namespace stirling
}  // namespace pl
