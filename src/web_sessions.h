/**
 * @file src/web_sessions.h
 * @brief Bounded, process-local browser authentication sessions.
 */
#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace web_sessions {
  /**
   * @brief Read the session cookie, rejecting duplicate cookie names.
   * @param cookies Raw Cookie header.
   * @return Opaque token, or an empty string for missing/ambiguous cookies.
   */
  inline std::string cookie_token(std::string_view cookies) {
    std::string result;
    bool found = false;
    while (!cookies.empty()) {
      const auto end = cookies.find(';');
      auto part = cookies.substr(0, end);
      const auto first = part.find_first_not_of(" \t");
      if (first != std::string_view::npos) {
        part.remove_prefix(first);
        part = part.substr(0, part.find_last_not_of(" \t") + 1);
        constexpr std::string_view prefix = "sunshine_session=";
        if (part.starts_with(prefix)) {
          if (found) {
            return {};
          }
          found = true;
          result = part.substr(prefix.size());
        }
      }
      if (end == std::string_view::npos) {
        break;
      }
      cookies.remove_prefix(end + 1);
    }
    return result;
  }

  /**
   * @brief Thread-safe session storage; tokens must come from a cryptographic RNG.
   */
  class store {
    /**
     * @brief Credential snapshot and fixed expiration for a session.
     */
    struct entry {
      std::string credentials;  ///< Credential fingerprint at login time.
      std::chrono::steady_clock::time_point expires;  ///< Absolute monotonic expiration.
    };

    std::mutex mutex_;  ///< Serializes session access.
    std::map<std::string, entry, std::less<>> entries_;  ///< Active sessions keyed by random token.

  public:
    /**
     * @brief Add a session, expiring old entries and bounding memory usage.
     * @param token Fresh random token.
     * @param credentials Current credential fingerprint.
     * @param now Monotonic time.
     */
    void add(const std::string &token, const std::string &credentials, std::chrono::steady_clock::time_point now) {
      std::scoped_lock lock(mutex_);
      std::erase_if(entries_, [now](const auto &item) {
        return item.second.expires <= now;
      });
      if (entries_.size() >= 256) {
        entries_.erase(entries_.begin());
      }
      entries_[token] = {credentials, now + std::chrono::hours(12)};
    }

    /**
     * @brief Check a session and discard expired or credential-stale entries.
     * @param token Presented cookie token.
     * @param credentials Current credential fingerprint.
     * @param now Monotonic time.
     * @return Whether the session is active.
     */
    bool valid(const std::string &token, const std::string &credentials, std::chrono::steady_clock::time_point now) {
      std::scoped_lock lock(mutex_);
      const auto it = entries_.find(token);
      if (it == entries_.end()) {
        return false;
      }
      if (it->second.expires <= now || it->second.credentials != credentials) {
        entries_.erase(it);
        return false;
      }
      return true;
    }

    /**
     * @brief Revoke a session.
     * @param token Token to revoke.
     */
    void remove(const std::string &token) {
      std::scoped_lock lock(mutex_);
      entries_.erase(token);
    }

    /**
     * @brief Revoke all sessions after credentials change or the server stops.
     */
    void clear() {
      std::scoped_lock lock(mutex_);
      entries_.clear();
    }
  };
}  // namespace web_sessions
