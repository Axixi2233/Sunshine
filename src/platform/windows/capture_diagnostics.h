/**
 * @file src/platform/windows/capture_diagnostics.h
 * @brief Helpers for reporting Windows display capture stalls.
 */
#pragma once

// standard includes
#include <chrono>
#include <optional>

namespace platf::dxgi {
  /**
   * @brief Track intervals where display capture produces no new desktop frames.
   *
   * The tracker is intentionally independent of the capture backend so its timing
   * behavior can be unit tested without a DirectX device.
   */
  class capture_stall_tracker_t {
  public:
    using clock_t = std::chrono::steady_clock;  ///< Monotonic clock used for capture diagnostics.
    using time_point_t = clock_t::time_point;  ///< Monotonic timestamp used by the tracker.

    /**
     * @brief Construct a capture stall tracker.
     *
     * @param report_after Minimum stall duration before reporting it.
     * @param report_interval Interval between reports for an ongoing stall.
     */
    capture_stall_tracker_t(std::chrono::milliseconds report_after, std::chrono::milliseconds report_interval):
        report_after_ {report_after},
        report_interval_ {report_interval} {
    }

    /**
     * @brief Record a capture attempt that produced no new desktop frame.
     *
     * @param now Current monotonic time.
     * @return Current stall duration when a report is due; otherwise no value.
     */
    std::optional<std::chrono::milliseconds> on_timeout(time_point_t now) {
      if (!stall_started_at_) {
        stall_started_at_ = last_frame_at_.value_or(now);
        next_report_at_ = *stall_started_at_ + report_after_;
      }

      if (now < *next_report_at_) {
        return std::nullopt;
      }

      const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - *stall_started_at_);
      next_report_at_ = now + report_interval_;
      return duration;
    }

    /**
     * @brief Record a newly captured desktop frame.
     *
     * @param now Current monotonic time.
     * @return Completed stall duration when it exceeded the reporting threshold; otherwise no value.
     */
    std::optional<std::chrono::milliseconds> on_frame(time_point_t now) {
      const auto duration = active_duration(now);

      stall_started_at_.reset();
      next_report_at_.reset();
      last_frame_at_ = now;

      if (duration && *duration >= report_after_) {
        return duration;
      }

      return std::nullopt;
    }

    /**
     * @brief Get the duration of the active no-frame interval.
     *
     * @param now Current monotonic time.
     * @return Active stall duration, or no value when capture is not stalled.
     */
    std::optional<std::chrono::milliseconds> active_duration(time_point_t now) const {
      if (!stall_started_at_) {
        return std::nullopt;
      }

      return std::chrono::duration_cast<std::chrono::milliseconds>(now - *stall_started_at_);
    }

  private:
    std::chrono::milliseconds report_after_;  ///< Minimum duration before the first diagnostic report.
    std::chrono::milliseconds report_interval_;  ///< Interval between ongoing-stall reports.
    std::optional<time_point_t> last_frame_at_;  ///< Time of the most recently captured desktop frame.
    std::optional<time_point_t> stall_started_at_;  ///< Start of the current no-frame interval.
    std::optional<time_point_t> next_report_at_;  ///< Earliest time for the next ongoing-stall report.
  };
}  // namespace platf::dxgi
