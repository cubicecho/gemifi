/**
 * Recognising gemifi ports by name (`protocol/sysex.md` §7). The product string is
 * "gemifi boot (<board>)" in the bootloader and "<board> (gemifi)" in the app; the OS port name
 * contains it, sometimes with a suffix such as " MIDI 1". HELLO is still the authority on role.
 */

/** What a port's name says it is. */
export const PortRole = {
  App: 'app',
  Boot: 'boot',
  Other: 'other',
} as const;
export type PortRole = (typeof PortRole)[keyof typeof PortRole];

/** The bootloader's product string prefix. */
const BOOT_PATTERN = /gemifi boot/i;
/** Any gemifi product string. */
const GEMIFI_PATTERN = /gemifi/i;

/**
 * Classifies a MIDI port by its name.
 *
 * @param name - The port name the OS reports.
 * @returns `boot`, `app`, or `other` for a port that is not gemifi.
 */
export function classifyPortName(name: string): PortRole {
  if (BOOT_PATTERN.test(name)) {
    return PortRole.Boot;
  }
  if (GEMIFI_PATTERN.test(name)) {
    return PortRole.App;
  }
  return PortRole.Other;
}
