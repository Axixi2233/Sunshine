/** @brief User-visible product name shared by management pages. */
export const productName = 'StarCore星核';

/**
 * @brief Apply product branding to translated prose without changing technical identifiers.
 * @param {string} text Translated message.
 * @returns {string} Branded message, preserving URLs, filenames and service names.
 */
export function brandText(text) {
  return text.replace(/(?<![\w/\\.-])Sunshine(?:·阿西西)?(?![\w/\\.-])/g, productName);
}
