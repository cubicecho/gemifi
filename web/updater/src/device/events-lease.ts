/**
 * EVENT messages while a tool needs them: `debug.events` (param 13) is set to 1 by the first tool
 * that asks and put back by the last one to let go, so the input tester and "find a control" can
 * both be on and a value a DAW script set is left as it was.
 */

import type { SysexClient } from '../protocol/client.ts';
import { getParam, setParam } from '../protocol/config.ts';

/** `debug.events`: EVENT messages on (§4.6). */
export const EVENTS_PARAM = 13;

/** One client's lease. */
interface Lease {
  holders: number;
  /** The value before the first holder, once read; null while it is being read. */
  before: Promise<number | null>;
}

const leases = new WeakMap<SysexClient, Lease>();

/**
 * Turns EVENTs on until the returned function is called.
 *
 * @param client - The device, with config (caps bit 1).
 * @returns Lets go: the last holder puts `debug.events` back.
 */
export function holdEvents(client: SysexClient): () => void {
  let lease = leases.get(client);
  if (lease === undefined) {
    const before = getParam(client, EVENTS_PARAM, 0).then(
      async (value) => {
        if (value !== 1) {
          await setParam(client, { id: EVENTS_PARAM, index: 0, value: 1 });
        }
        return value;
      },
      () => null,
    );
    lease = { holders: 0, before };
    leases.set(client, lease);
  }
  const held = lease;
  held.holders += 1;
  let isReleased = false;
  return () => {
    if (isReleased) {
      return;
    }
    isReleased = true;
    held.holders -= 1;
    if (held.holders > 0) {
      return;
    }
    leases.delete(client);
    void held.before.then((value) => {
      const isStillFree = leases.has(client) === false;
      if (value !== null && value !== 1 && isStillFree) {
        void setParam(client, { id: EVENTS_PARAM, index: 0, value }).catch(() => undefined);
      }
    });
  };
}
