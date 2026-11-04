/**
 * @file src/virtual_display_policy.h
 * @brief Resolve client, global and legacy per-application display preferences.
 */
#pragma once
#include <optional>

namespace virtual_display_policy {
  /**
   * @brief Choose the effective display mode; an explicit client disable wins.
   * @param client Client mode, or no value when absent.
   * @param enabled Global virtual display toggle.
   * @param mode Global topology (1 extends, 2 is exclusive).
   * @param app_enabled Legacy per-app virtual display toggle.
   * @return Effective mode: 0 disabled, 1 extended, 2 exclusive.
   */
  inline int resolve(std::optional<int> client, bool enabled, int mode, bool app_enabled) {
    if (client) {
      return *client;
    }
    if (enabled) {
      return mode == 2 ? 2 : 1;
    }
    return app_enabled ? 1 : 0;
  }
}  // namespace virtual_display_policy
