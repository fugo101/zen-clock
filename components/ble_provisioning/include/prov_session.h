// SPDX-License-Identifier: MIT
// ZenClock — provisioning session lifecycle, as a pure transition function
//
// This is the machine that used to be five loose booleans in ble_provisioning.c (s_active,
// s_stopping, s_cred_ok, s_mem_freed, plus the still-separate s_initialized). Nothing here calls
// ESP-IDF, so it builds for the host-side [env:native] tests — see the symlinks in
// test/test_pure_logic/.
//
// It exists because the outcome this machine computes gates ble_provisioning_release_memory(),
// which frees ~110 KB of BT RAM permanently: after it, the device cannot be re-provisioned until
// it is reflashed. That decision was previously a latched bool read in another file, with no test
// coverage at all.
//
// What deliberately is NOT here:
//
//   - Overlay visibility. Whether the QR is on screen is published UI intent owned by
//     prov_screen.c, and the two are independent — dismissed-but-advertising and
//     visible-and-advertising are the same session phase. ADR-0002 explains why the deep-sleep
//     inhibit must keep reading the overlay and not this phase.
//   - s_initialized. That tracks whether network_prov_mgr_deinit() is owed, which cycles within a
//     session rather than being a phase of one.
//   - Teardown ordering in the PROV_END handler. That is sequencing against a third-party
//     library, not a decision.

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

  // ============================================================
  // Phases
  // ============================================================

  typedef enum
  {
    PROV_PHASE_IDLE = 0,    // No session. A start may begin one.
    PROV_PHASE_STARTING,    // network_prov_mgr_init() succeeded; not advertising yet.
    PROV_PHASE_ADVERTISING, // Service is up; a phone may connect.
    PROV_PHASE_STOPPING,    // A deliberate stop was requested; PROV_END has not arrived.
    PROV_PHASE_UNAVAILABLE, // Terminal: BLE controller memory released. Only a reboot leaves it.
  } prov_phase_t;

  // ============================================================
  // Session state
  //
  // `verified` is a latch, not a phase, because it is orthogonal to stopping: a credential that
  // verified must still report a completed session even if a cancel raced it. Flattening the two
  // into one enum needs a STOPPING_VERIFIED enumerator, which is worse than the booleans this
  // replaced.
  // ============================================================

  typedef struct
  {
    prov_phase_t phase;
    bool verified;
  } prov_session_t;

  // ============================================================
  // Inputs
  //
  // The NETWORK_PROV_* events the manager emits, plus the two things the app itself does (begin a
  // start, request a stop) and the one irreversible step (release the controller memory).
  // ============================================================

  typedef enum
  {
    PROV_IN_START_BEGUN = 0, // network_prov_mgr_init() returned OK
    PROV_IN_START_OK,        // service is advertising (NETWORK_PROV_START, or start() returning OK)
    PROV_IN_START_FAILED,    // a start unwound partway
    PROV_IN_STOP_REQUESTED,  // deliberate cancel
    PROV_IN_CRED_RECEIVED,   // NETWORK_PROV_WIFI_CRED_RECV
    PROV_IN_CRED_VERIFIED,   // NETWORK_PROV_WIFI_CRED_SUCCESS
    PROV_IN_CRED_FAILED,     // NETWORK_PROV_WIFI_CRED_FAIL
    PROV_IN_END,             // NETWORK_PROV_END — the service has stopped
    PROV_IN_MEM_RELEASED,    // ble_provisioning_release_memory() ran
  } prov_input_t;

  // ============================================================
  // Actions
  //
  // These mirror ble_prov_event_t one-for-one where they emit, and ble_provisioning.c
  // _Static_assert()s the two enums together. They are re-declared rather than reused because
  // ble_provisioning.h includes esp_err.h, which would keep this file off the host test bench —
  // the same reason input_policy.h re-declares the BSP button enums.
  // ============================================================

  typedef enum
  {
    PROV_ACT_NONE = 0,
    PROV_ACT_EMIT_STARTED,       // -> BLE_PROV_STARTED
    PROV_ACT_EMIT_CRED_RECEIVED, // -> BLE_PROV_CRED_RECEIVED
    PROV_ACT_EMIT_SUCCESS,       // -> BLE_PROV_SUCCESS  (the only thing that permits the 110 KB free)
    PROV_ACT_EMIT_FAILED,        // -> BLE_PROV_FAILED
    PROV_ACT_EMIT_STOPPED,       // -> BLE_PROV_STOPPED
    PROV_ACT_SUPPRESS,           // a failure raised by our own stop — deliberately not reported
  } prov_action_t;

  typedef struct
  {
    prov_session_t next;
    prov_action_t action;
  } prov_step_t;

  /**
   * @brief A session that has not started: IDLE with no outcome latched.
   */
  prov_session_t prov_session_initial(void);

  /**
   * @brief Advance the session by one input.
   *
   * Total: every phase accepts every input. An input a phase has no transition for leaves the
   * state unchanged and returns PROV_ACT_NONE, so an unhandled pair cannot be introduced silently.
   */
  prov_step_t prov_session_step(prov_session_t state, prov_input_t input);

  /**
   * @brief Whether a session in this phase owns the radio.
   *
   * True for the three live phases (STARTING, ADVERTISING, STOPPING) — network_prov_mgr has the
   * radio and nothing else may start WiFi underneath it.
   *
   * False for PROV_PHASE_UNAVAILABLE, and that is the whole reason this is a function rather than
   * a comparison at the call site: UNAVAILABLE reads like the most dead phase of the five, but it
   * means the BLE controller's memory has been released, so the radio belongs to WiFi permanently.
   * A call site that spells this `phase != PROV_PHASE_IDLE` blocks every WiFi reconnect for the
   * rest of the device's uptime after the first successful provisioning.
   */
  bool prov_session_holds_radio(prov_phase_t phase);

#ifdef __cplusplus
}
#endif
