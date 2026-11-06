/**
 * The other MIDI ports of the connected device: a Code 61 with a DAW port or a DIN port shows up
 * as more than one port, all named after its USB product string ("gemifi (Code 61)"), with a
 * suffix the OS adds ("… MIDI 2", "… DAW").
 */

/** The gemifi product string inside an OS port name (`protocol/sysex.md` §7). */
const PRODUCT_PATTERN = /gemifi(?: boot)? \([^)]*\)/i;

/**
 * @param selectedName - The name of the port the page talks to.
 * @param inputs - Every connected input.
 * @returns The inputs that belong to the same device, other than the one named `selectedName`;
 *   none when the selected port is not a gemifi one.
 */
export function otherDevicePorts<T extends { name: string | null }>(
  selectedName: string | null,
  inputs: readonly T[],
): T[] {
  const product = selectedName === null ? null : (PRODUCT_PATTERN.exec(selectedName)?.[0] ?? null);
  if (product === null) {
    return [];
  }
  const wanted = product.toLowerCase();
  return inputs.filter((input) => {
    const name = input.name ?? '';
    return name !== selectedName && name.toLowerCase().includes(wanted);
  });
}

/**
 * A short tag for a port in the monitor: what its name adds to the device's product string.
 *
 * @param name - The port's name.
 * @returns E.g. `DAW` for "gemifi (Code 61) DAW", or the name itself when nothing is left.
 */
export function portTag(name: string): string {
  const rest = name.replace(PRODUCT_PATTERN, '').replace(/^[\s:-]+|[\s:-]+$/g, '');
  return rest === '' ? name : rest;
}
