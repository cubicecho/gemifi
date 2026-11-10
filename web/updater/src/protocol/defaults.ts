/** Tunable timings for talking to a device. */

/** The stop-and-wait request loop (`protocol/sysex.md` §1). */
export interface RequestSettings {
  /** How long to wait for a reply before resending the same request with the same `seq`. */
  replyTimeoutMs: number;
  /** The reply timeout for FW_BEGIN, FW_END, COMMIT and FACTORY_RESET, which erase or write flash, or read back and CRC. */
  longReplyTimeoutMs: number;
  /** How many times a request is resent after the first try before giving up. */
  maxRetries: number;
  /**
   * After a LOCKED reply (§4.9): how long past the end of the device's unlock window to keep
   * waiting for UNLOCKED before giving up.
   */
  unlockGraceMs: number;
}

export const REQUEST_DEFAULTS: Readonly<RequestSettings> = Object.freeze({
  replyTimeoutMs: 200,
  longReplyTimeoutMs: 2000,
  maxRetries: 5,
  unlockGraceMs: 5000,
});

/** Waiting for the device to come back after a reboot. */
export interface RebootSettings {
  /** How long to wait for the device's port to reappear after REBOOT. */
  reappearTimeoutMs: number;
  /** How long to let a port that just appeared settle before talking to it. */
  portSettleMs: number;
  /**
   * A/B devices: how long to keep asking HELLO after REBOOT(0). Longer than a bootloader's
   * reboot: an ESP8266 copies the new image into place at boot before it starts.
   */
  abRestartTimeoutMs: number;
  /** A/B devices: the pause before each HELLO while waiting for the restart. */
  abPollMs: number;
}

export const REBOOT_DEFAULTS: Readonly<RebootSettings> = Object.freeze({
  reappearTimeoutMs: 5000,
  portSettleMs: 300,
  abRestartTimeoutMs: 15_000,
  abPollMs: 500,
});
