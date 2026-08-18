/**
 * @file src/platform/windows/hidmaestro_client.h
 * @brief Sunshine client for the out-of-process HIDMaestro DualSense host.
 */
#pragma once

// standard includes
#include <memory>

// local includes
#include "src/platform/common.h"

namespace platf::hidmaestro {
  /**
   * @brief Owns the HIDMaestro helper and its virtual DualSense controllers.
   */
  class client_t {
  public:
    /**
     * @brief Construct an inactive HIDMaestro client.
     */
    client_t();

    /**
     * @brief Stop the helper and destroy all virtual DualSense controllers.
     */
    ~client_t();

    /** @brief HIDMaestro clients cannot be copied. */
    client_t(const client_t &) = delete;
    /** @brief HIDMaestro clients cannot be copy-assigned. */
    client_t &operator=(const client_t &) = delete;

    /**
     * @brief Launch the helper and wait for its driver/profile initialization.
     * @return Zero on success.
     */
    int init();

    /**
     * @brief Report whether the helper is ready.
     * @return True when messages can be submitted.
     */
    [[nodiscard]] bool available() const;

    /**
     * @brief Create a virtual DualSense controller.
     * @param id Sunshine controller identifiers.
     * @param feedback_queue Queue used to return controller output to Moonlight.
     * @param enable_native_pcm Whether to request the composite USB profile and PCM capture.
     * @return Zero on success.
     */
    int alloc_gamepad(const gamepad_id_t &id, feedback_queue_t feedback_queue, bool enable_native_pcm);

    /**
     * @brief Unbind a Moonlight controller while preserving its virtual DualSense device.
     * @param nr Sunshine global controller index.
     */
    void detach_gamepad(int nr);

    /**
     * @brief Destroy a virtual DualSense controller.
     * @param nr Sunshine global controller index.
     */
    void free_gamepad(int nr);

    /**
     * @brief Submit buttons, axes, and triggers.
     * @param nr Sunshine global controller index.
     * @param state Current gamepad state.
     * @param back_as_touchpad_click Whether Back/Select should also press the touchpad.
     */
    void update(int nr, const gamepad_state_t &state, bool back_as_touchpad_click);

    /**
     * @brief Submit a touchpad contact update.
     * @param touch Touch event from Moonlight.
     */
    void touch(const gamepad_touch_t &touch);

    /**
     * @brief Submit an IMU sample.
     * @param motion Motion event from Moonlight.
     */
    void motion(const gamepad_motion_t &motion);

    /**
     * @brief Submit battery state.
     * @param battery Battery event from Moonlight.
     */
    void battery(const gamepad_battery_t &battery);

    /**
     * @brief Check whether a controller index belongs to this backend.
     * @param nr Sunshine global controller index.
     * @return True when a virtual DualSense occupies the index.
     */
    [[nodiscard]] bool owns_gamepad(int nr) const;

    /**
     * @brief Check whether a virtual DualSense device exists at an index.
     * @param nr Sunshine global controller index.
     * @return True when the helper still owns the device, including while detached.
     */
    [[nodiscard]] bool has_gamepad(int nr) const;

  private:
    class impl_t;
    std::unique_ptr<impl_t> impl_;  ///< Platform implementation.
  };
}  // namespace platf::hidmaestro
