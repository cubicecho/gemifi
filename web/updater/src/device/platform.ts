/**
 * Which OS the page runs on, where it changes what the page asks of the user: on Windows, Chrome's
 * Web MIDI can stop responding when a MIDI device is switched off under it.
 */

/** The parts of `navigator` read here; `userAgentData` is Chromium's and not in the DOM types. */
export interface NavigatorLike {
  userAgent?: string | undefined;
  userAgentData?: { platform?: string | undefined } | undefined;
}

/**
 * @param nav - `navigator`, or undefined outside a browser.
 * @returns Whether it is Windows: `userAgentData.platform` where the browser has it, else the user agent.
 */
export function isWindows(nav: NavigatorLike | undefined = globalThis.navigator): boolean {
  const platform = nav?.userAgentData?.platform ?? '';
  if (platform !== '') {
    return platform === 'Windows';
  }
  return /\bWindows\b/i.test(nav?.userAgent ?? '');
}
