/**
 * @brief Resolve only known management pages as post-login destinations.
 * @param {string} search Login page query string.
 * @returns {string} Safe same-origin destination.
 */
export function loginDestination(search) {
  const next = new URLSearchParams(search).get('next');
  return ['/', '/apps', '/clients', '/config', '/featured', '/password', '/pin', '/troubleshooting'].includes(next) ? next : '/';
}

/**
 * @brief Redirect expired browser API sessions to the login form.
 * @param {Response} response API response.
 * @param {Location} location Browser location.
 */
export function handleAuthentication(response, location) {
  if (response.status === 401 && !['/login', '/login/', '/welcome', '/welcome/', '/logout', '/logout/'].includes(location.pathname)) {
    location.replace('/login?next=' + encodeURIComponent(location.pathname));
  }
}
