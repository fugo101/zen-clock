// SPDX-License-Identifier: MIT
// ZenClock — provisioning session lifecycle (pure). See prov_session.h for what is and is not
// modelled here, and docs/adr/0002-wifi-and-ble-provisioning-boundaries.md for why the QR
// overlay's visibility is deliberately not part of it.

#include "prov_session.h"

static prov_step_t step_of(const prov_phase_t phase, const bool verified, const prov_action_t action)
{
  const prov_step_t r = {.next = {.phase = phase, .verified = verified}, .action = action};
  return r;
}

prov_session_t prov_session_initial(void)
{
  const prov_session_t s = {.phase = PROV_PHASE_IDLE, .verified = false};
  return s;
}

bool prov_session_holds_radio(const prov_phase_t phase)
{
  // Enumerated rather than expressed as "not IDLE and not UNAVAILABLE" so that adding a phase is a
  // -Wswitch build failure here instead of a silent verdict.
  switch (phase)
  {
  case PROV_PHASE_STARTING:
  case PROV_PHASE_ADVERTISING:
  case PROV_PHASE_STOPPING:
    return true;
  case PROV_PHASE_IDLE:
  case PROV_PHASE_UNAVAILABLE:
    return false;
  }
  return false;
}

prov_step_t prov_session_step(const prov_session_t state, const prov_input_t input)
{
  // Terminal first, and unconditionally: once the BLE controller's memory is gone there is nothing
  // left to advertise with, so no later input may move the phase or emit anything. Checking this
  // ahead of the phase switch is what makes that a property of the machine rather than five
  // separate default cases that could each be forgotten.
  if (state.phase == PROV_PHASE_UNAVAILABLE)
  {
    return step_of(PROV_PHASE_UNAVAILABLE, state.verified, PROV_ACT_NONE);
  }

  // Irreversible, and reachable from anywhere: the app releases the memory on the success path
  // after the session has already returned to IDLE, but a caller is not obliged to.
  if (input == PROV_IN_MEM_RELEASED)
  {
    return step_of(PROV_PHASE_UNAVAILABLE, false, PROV_ACT_NONE);
  }

  // Forwarded from every live phase, deliberately not gated on ADVERTISING. A credential that
  // reached us has to be stored: gating it meant that a phone completing provisioning while the
  // user happened to back out had its credential dropped, while the verification that followed
  // still latched an outcome — so END reported success, released the BT controller for good, and
  // reconnected with the *old* credential. Storing a credential we then cancel is recoverable;
  // reporting a success we never stored is not.
  if (input == PROV_IN_CRED_RECEIVED)
  {
    return step_of(state.phase, state.verified, PROV_ACT_EMIT_CRED_RECEIVED);
  }

  // An explicit app action, fed only after network_prov_mgr_init() has already succeeded, so it
  // resets the session from any live phase rather than only from IDLE. The booleans this replaced
  // were cleared unconditionally at the top of start() for the same reason: a start that landed
  // while a previous session was still winding down otherwise advanced nothing, and the session
  // sat in STOPPING with a live service behind it and no BLE_PROV_STARTED ever emitted — the user
  // saw no QR at all. Resurrection is prevented on START_OK, which is the racy one.
  if (input == PROV_IN_START_BEGUN)
  {
    return step_of(PROV_PHASE_STARTING, false, PROV_ACT_NONE);
  }

  switch (state.phase)
  {
  case PROV_PHASE_IDLE:
    break; // only START_BEGUN and MEM_RELEASED leave IDLE, both handled above

  case PROV_PHASE_STARTING:
    switch (input)
    {
    case PROV_IN_START_OK:
      return step_of(PROV_PHASE_ADVERTISING, state.verified, PROV_ACT_EMIT_STARTED);
    case PROV_IN_START_FAILED:
      return step_of(PROV_PHASE_IDLE, false, PROV_ACT_NONE);
    case PROV_IN_STOP_REQUESTED:
      // A cancel landing between network_prov_mgr_init() and the service coming up. deinit()
      // emits PROV_END itself, so the stop still completes through the normal path.
      return step_of(PROV_PHASE_STOPPING, state.verified, PROV_ACT_NONE);
    case PROV_IN_END:
      // The manager can end a session that never reached advertising — a start that unwound, or a
      // deinit issued from here. Resolving it is what keeps the phase from sticking at STARTING
      // with no further input able to clear it.
      return step_of(PROV_PHASE_IDLE, false, state.verified ? PROV_ACT_EMIT_SUCCESS : PROV_ACT_EMIT_STOPPED);
    default:
      break;
    }
    break;

  case PROV_PHASE_ADVERTISING:
    switch (input)
    {
    case PROV_IN_START_OK:
      // Monotonic guard, not an assignment. ble_provisioning.c learns "advertising" from both the
      // NETWORK_PROV_START handler and the tail of start(), on different tasks; only the first may
      // count, and neither may resurrect a session that has already ended.
      return step_of(PROV_PHASE_ADVERTISING, state.verified, PROV_ACT_NONE);
    case PROV_IN_CRED_VERIFIED:
      // Latches quietly. PROV_END carries no outcome of its own, so this flag is the only thing
      // separating "the phone provisioned us" from "the user backed out".
      return step_of(PROV_PHASE_ADVERTISING, true, PROV_ACT_NONE);
    case PROV_IN_CRED_FAILED:
      // Keeps advertising: the phone retries over the same BLE link rather than re-pairing.
      return step_of(PROV_PHASE_ADVERTISING, state.verified, PROV_ACT_EMIT_FAILED);
    case PROV_IN_STOP_REQUESTED:
      return step_of(PROV_PHASE_STOPPING, state.verified, PROV_ACT_NONE);
    case PROV_IN_END:
      return step_of(PROV_PHASE_IDLE, false, state.verified ? PROV_ACT_EMIT_SUCCESS : PROV_ACT_EMIT_STOPPED);
    default:
      break;
    }
    break;

  case PROV_PHASE_STOPPING:
    switch (input)
    {
    case PROV_IN_CRED_VERIFIED:
      // A credential that verified wins over a cancel that raced it — the session really did
      // complete, and the alternative discards a provisioning the user just finished.
      return step_of(PROV_PHASE_STOPPING, true, PROV_ACT_NONE);
    case PROV_IN_CRED_FAILED:
      // Raised by our own teardown. Forwarding it would have the app erase a perfectly good
      // stored credential and put the QR back up.
      return step_of(PROV_PHASE_STOPPING, state.verified, PROV_ACT_SUPPRESS);
    case PROV_IN_END:
      return step_of(PROV_PHASE_IDLE, false, state.verified ? PROV_ACT_EMIT_SUCCESS : PROV_ACT_EMIT_STOPPED);
    default:
      break;
    }
    break;

  case PROV_PHASE_UNAVAILABLE:
    break; // handled above
  }

  return step_of(state.phase, state.verified, PROV_ACT_NONE);
}
