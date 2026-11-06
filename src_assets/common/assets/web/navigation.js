/**
 * @brief Match a local navigation destination to the current page without highlighting external links.
 * @param {string|null} href Link destination from the navigation element.
 * @param {string} currentHref Absolute URL of the current management page.
 * @returns {boolean} Whether the link points to this page on the same origin.
 */
export function isCurrentNavigationLink(href, currentHref) {
  if (!href || href === '#') return false;
  const current = new URL(currentHref);
  const destination = new URL(href, currentHref);
  const currentPath = current.pathname.replace(/\/$/, '') || '/';
  const destinationPath = destination.pathname.replace(/\/$/, '') || '/';
  return destination.origin === current.origin && destinationPath === currentPath;
}
