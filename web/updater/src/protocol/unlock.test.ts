import { afterEach, describe, expect, it, vi } from 'vitest';
import {
  describeStatus,
  RequestError,
  RequestFailure,
  SysexClient,
  type Transport,
  type UnlockPrompt,
} from './client.ts';
import { factoryReset } from './config.ts';
import { encodeLstring, encodeU14, PayloadReader } from './encoding.ts';
import { reboot } from './flash.ts';
import { decodeFrame, encodeFrame } from './frame.ts';
import { capabilityNames, parseDeviceMessage, parseUnlock, unlockInstruction, unlockPayload } from './messages.ts';
import { Capability, Command, RebootMode, Status, Unsolicited } from './wire.ts';

const FAST = { replyTimeoutMs: 15, longReplyTimeoutMs: 30, maxRetries: 1, unlockGraceMs: 50 };

/** The fields of an UNLOCK reply after the status. */
function unlockReply(state: number, window: number, holdMs: number, buttons: string[]): number[] {
  return [state, window, ...encodeU14(holdMs), buttons.length, ...buttons.flatMap(encodeLstring)];
}

/** An app with physical unlock (§4.9): FACTORY_RESET, REBOOT(1) and FW_BEGIN answer LOCKED until unlocked. */
class FakeLockedDevice {
  isUnlocked = false;
  /** Seconds the unlock window has left; a LOCKED reply opens it. */
  window = 0;
  windowOnLocked = 30;
  /** Seconds UNLOCK(1) opens the window for. */
  windowOnOpen = 30;
  /** Become unlocked straight after the next LOCKED reply, without UNLOCKED (a race the host must survive). */
  unlockAfterLocked = false;
  holdMs = 2000;
  buttons = ['Pitch down', 'Pitch up'];
  /** Answer UNLOCK with this status instead (e.g. BAD_CMD). */
  unlockStatus: number = Status.Ok;
  /** Every request received, as `[cmd, first payload byte]`. */
  received: Array<[number, number | undefined]> = [];
  #listeners = new Set<(bytes: Uint8Array) => void>();

  transport(): Transport {
    return {
      send: (bytes) => queueMicrotask(() => this.#handle(bytes)),
      subscribe: (listener) => {
        this.#listeners.add(listener);
        return () => this.#listeners.delete(listener);
      },
    };
  }

  /** The user held the combo. */
  unlock(): void {
    this.isUnlocked = true;
    this.window = 0;
    this.#emit(encodeFrame(Unsolicited.Unlocked, 0, [1]));
  }

  /** The window ran out. */
  closeWindow(): void {
    this.window = 0;
    this.#emit(encodeFrame(Unsolicited.Unlocked, 0, [0]));
  }

  #emit(bytes: Uint8Array): void {
    for (const listener of this.#listeners) {
      listener(bytes);
    }
  }

  #handle(bytes: Uint8Array): void {
    const result = decodeFrame(bytes);
    if (result.ok === false) {
      return;
    }
    const { cmd, seq, payload } = result.frame;
    this.received.push([cmd, payload[0]]);
    const reply = (fields: number[]) => this.#emit(encodeFrame(cmd | 0x40, seq, fields));
    const isGuarded =
      cmd === Command.FactoryReset || cmd === Command.FwBegin || (cmd === Command.Reboot && payload[0] === 1);
    if (isGuarded && this.isUnlocked === false) {
      this.window = this.windowOnLocked;
      reply([Status.Locked]);
      this.isUnlocked = this.unlockAfterLocked;
      return;
    }
    switch (cmd) {
      case Command.Unlock: {
        if (this.unlockStatus !== Status.Ok) {
          reply([this.unlockStatus]);
          return;
        }
        const open = new PayloadReader(payload).byte();
        if (open === 1 && this.isUnlocked === false) {
          this.window = this.windowOnOpen;
        }
        reply([Status.Ok, ...unlockReply(Number(this.isUnlocked), this.window, this.holdMs, this.buttons)]);
        return;
      }
      case Command.FactoryReset:
      case Command.Reboot:
        reply([Status.Ok]);
        return;
      default:
        reply([Status.BadCmd]);
    }
  }
}

/**
 * @param client - The client.
 * @returns Resolves with the first prompt offered.
 */
function nextPrompt(client: SysexClient): Promise<UnlockPrompt> {
  return new Promise((resolve) => {
    const stop = client.onUnlockPrompt((prompt) => {
      if (prompt !== null) {
        stop();
        resolve(prompt);
      }
    });
  });
}

afterEach(() => {
  vi.useRealTimers();
});

describe('UNLOCK codec (§4.9)', () => {
  it('builds the request: open 0 or 1', () => {
    expect(unlockPayload(false)).toEqual([0]);
    expect(unlockPayload(true)).toEqual([1]);
  });

  it('parses the reply: state, window, hold u14, n, then n lstrings', () => {
    const body = unlockReply(0, 30, 2000, ['Pitch down', 'Pitch up']);
    expect(body.slice(0, 5)).toEqual([0, 30, 0x50, 0x0f, 2]);
    expect(parseUnlock(body)).toEqual({
      isUnlocked: false,
      windowSeconds: 30,
      holdMs: 2000,
      buttons: ['Pitch down', 'Pitch up'],
    });
    expect(parseUnlock(unlockReply(1, 0, 0, ['Up']))).toEqual({
      isUnlocked: true,
      windowSeconds: 0,
      holdMs: 0,
      buttons: ['Up'],
    });
  });

  it('parses UNLOCKED (0x66)', () => {
    const frame = (state: number) => ({ version: 1, cmd: Unsolicited.Unlocked, seq: 0, payload: [state] });
    expect(parseDeviceMessage(frame(1))).toEqual({ kind: 'unlocked', isUnlocked: true });
    expect(parseDeviceMessage(frame(0))).toEqual({ kind: 'unlocked', isUnlocked: false });
  });

  it('names status 12 and caps bit 10', () => {
    expect(Status.Locked).toBe(12);
    expect(describeStatus(Status.Locked)).toMatch(/^LOCKED \(/);
    expect(Capability.Unlock).toBe(0x400);
    expect(capabilityNames(Capability.Config | Capability.Unlock)).toEqual(['config', 'physical unlock']);
  });

  it('words the prompt', () => {
    expect(unlockInstruction(['Pitch down', 'Pitch up'], 2000)).toBe(
      'Hold Pitch down + Pitch up on the device for 2 s.',
    );
    expect(unlockInstruction(['Up', 'Down'], 1500)).toBe('Hold Up + Down on the device for 1.5 s.');
    expect(unlockInstruction(['Shift'], 0)).toBe('Press Shift on the device.');
    expect(unlockInstruction(['Shift'], null)).toBe('Hold Shift on the device.');
    expect(unlockInstruction([], null)).toMatch(/unlock buttons/);
  });
});

describe('a LOCKED reply (§4.9)', () => {
  it('asks UNLOCK, offers the prompt, waits for UNLOCKED and sends the request again', async () => {
    const device = new FakeLockedDevice();
    const client = new SysexClient(device.transport(), FAST);
    const prompted = nextPrompt(client);
    const reset = factoryReset(client);
    const prompt = await prompted;
    expect(prompt.cmd).toBe(Command.FactoryReset);
    expect(prompt.buttons).toEqual(['Pitch down', 'Pitch up']);
    expect(prompt.holdMs).toBe(2000);
    expect(prompt.windowEndsAt - Date.now()).toBeGreaterThan(29_000);
    expect(client.unlockPrompt()).toBe(prompt);
    device.unlock();
    await expect(reset).resolves.toBeUndefined();
    expect(device.received).toEqual([
      [Command.FactoryReset, undefined],
      [Command.Unlock, 0],
      [Command.FactoryReset, undefined],
    ]);
    expect(client.unlockPrompt()).toBeNull();
  });

  it('unlocks REBOOT(1) the same way, and leaves REBOOT(0) alone', async () => {
    const device = new FakeLockedDevice();
    const client = new SysexClient(device.transport(), FAST);
    await expect(reboot(client, RebootMode.App)).resolves.toBe(true);
    const prompted = nextPrompt(client);
    const rebooting = reboot(client, RebootMode.Boot);
    await prompted;
    device.unlock();
    await expect(rebooting).resolves.toBe(true);
    expect(device.received.map(([cmd]) => cmd)).toEqual([
      Command.Reboot,
      Command.Reboot,
      Command.Unlock,
      Command.Reboot,
    ]);
  });

  it('gives up when UNLOCKED says 0: the window closed', async () => {
    const device = new FakeLockedDevice();
    const client = new SysexClient(device.transport(), FAST);
    const prompted = nextPrompt(client);
    const reset = factoryReset(client);
    await prompted;
    device.closeWindow();
    const error = await reset.catch((e: unknown) => e);
    expect(error).toBeInstanceOf(RequestError);
    expect((error as RequestError).failure).toBe(RequestFailure.Locked);
    expect((error as RequestError).message).toMatch(/wasn't unlocked/);
    expect(device.received.map(([cmd]) => cmd)).toEqual([Command.FactoryReset, Command.Unlock]);
    expect(client.unlockPrompt()).toBeNull();
  });

  it('gives up when no UNLOCKED comes within the window and the grace time', async () => {
    vi.useFakeTimers();
    const device = new FakeLockedDevice();
    device.windowOnLocked = 1;
    const client = new SysexClient(device.transport(), { ...FAST, unlockGraceMs: 5000 });
    const prompted = nextPrompt(client);
    const reset = factoryReset(client).catch((e: unknown) => e);
    await prompted;
    await vi.advanceTimersByTimeAsync(5900);
    expect(client.unlockPrompt()).not.toBeNull();
    await vi.advanceTimersByTimeAsync(200);
    const error = await reset;
    expect((error as RequestError).failure).toBe(RequestFailure.Locked);
    expect((error as RequestError).message).toMatch(/wasn't unlocked/);
    expect(client.unlockPrompt()).toBeNull();
  });

  it("stops when the prompt's cancel is pressed", async () => {
    const device = new FakeLockedDevice();
    const client = new SysexClient(device.transport(), FAST);
    const prompted = nextPrompt(client);
    const reset = factoryReset(client);
    (await prompted).cancel();
    const error = await reset.catch((e: unknown) => e);
    expect((error as RequestError).failure).toBe(RequestFailure.Aborted);
    expect((error as RequestError).message).toMatch(/Cancelled/);
    expect(client.unlockPrompt()).toBeNull();
  });

  it("stops when the caller's signal aborts", async () => {
    const device = new FakeLockedDevice();
    const client = new SysexClient(device.transport(), FAST);
    const controller = new AbortController();
    const prompted = nextPrompt(client);
    const reset = factoryReset(client, { signal: controller.signal });
    await prompted;
    controller.abort();
    await expect(reset).rejects.toMatchObject({ failure: RequestFailure.Aborted });
    expect(client.unlockPrompt()).toBeNull();
  });

  it('sends the request again at once when UNLOCK says it is unlocked already', async () => {
    const device = new FakeLockedDevice();
    const client = new SysexClient(device.transport(), FAST);
    const prompts: Array<UnlockPrompt | null> = [];
    client.onUnlockPrompt((prompt) => prompts.push(prompt));
    device.unlockAfterLocked = true;
    const reset = factoryReset(client);
    await expect(reset).resolves.toBeUndefined();
    expect(prompts).toEqual([]);
    expect(device.received.map(([cmd]) => cmd)).toEqual([Command.FactoryReset, Command.Unlock, Command.FactoryReset]);
  });

  it('opens the window with UNLOCK(1) when it has closed already', async () => {
    const device = new FakeLockedDevice();
    const client = new SysexClient(device.transport(), FAST);
    const prompted = nextPrompt(client);
    device.windowOnLocked = 0;
    const reset = factoryReset(client);
    const prompt = await prompted;
    expect(prompt.windowEndsAt - Date.now()).toBeGreaterThan(29_000);
    expect(device.received.slice(1)).toEqual([
      [Command.Unlock, 0],
      [Command.Unlock, 1],
    ]);
    device.unlock();
    await expect(reset).resolves.toBeUndefined();
  });

  it('still waits, naming no buttons, when UNLOCK is refused', async () => {
    const device = new FakeLockedDevice();
    device.unlockStatus = Status.BadCmd;
    const client = new SysexClient(device.transport(), FAST);
    const prompted = nextPrompt(client);
    const reset = factoryReset(client);
    const prompt = await prompted;
    expect(prompt.buttons).toEqual([]);
    expect(prompt.holdMs).toBeNull();
    device.unlock();
    await expect(reset).resolves.toBeUndefined();
  });

  it('ends the wait when the client closes', async () => {
    const device = new FakeLockedDevice();
    const client = new SysexClient(device.transport(), FAST);
    const prompted = nextPrompt(client);
    const reset = factoryReset(client);
    await prompted;
    client.close();
    await expect(reset).rejects.toMatchObject({ failure: RequestFailure.Aborted });
  });
});
