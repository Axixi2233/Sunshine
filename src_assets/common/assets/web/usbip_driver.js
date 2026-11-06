/** @brief Derive driver maintenance UI state from the authenticated status response. */
export function driverView(status) {
  const state = status?.state ?? 'checking';
  return {
    warning: ['stale', 'error', 'helper_missing'].includes(state),
    disabled: !status || status.repairing || status.streaming || ['healthy', 'helper_missing', 'unknown', 'checking'].includes(state),
    state,
  };
}
