/** Tunable timings for talking to a device. */

/** The stop-and-wait request loop (`protocol/sysex.md` §1). */
export interface RequestSettings {
  /** How long to wait for a reply before resending the same request with the same `seq`. */
  replyTimeoutMs: number;
  /** The reply timeout for FW_BEGIN and FW_END, which erase a page or read back and CRC the whole image. */
  longReplyTimeoutMs: number;
  /** How many times a request is resent after the first try before giving up. */
  maxRetries: number;
}

export const REQUEST_DEFAULTS: Readonly<RequestSettings> = Object.freeze({
  replyTimeoutMs: 200,
  longReplyTimeoutMs: 2000,
  maxRetries: 5,
});

/** Waiting for the device to come back after a reboot. */
export interface RebootSettings {
  /** How long to wait for the device's port to reappear after REBOOT. */
  reappearTimeoutMs: number;
  /** How long to let a port that just appeared settle before talking to it. */
  portSettleMs: number;
}

export const REBOOT_DEFAULTS: Readonly<RebootSettings> = Object.freeze({
  reappearTimeoutMs: 5000,
  portSettleMs: 300,
});
