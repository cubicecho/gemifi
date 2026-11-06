import { DialogLayout } from '@/components/dialog-layout';
import { Button } from '@/components/ui/button';
import { Progress } from '@/components/ui/progress';
import type { SysexClient } from '../protocol/client.ts';
import { unlockInstruction } from '../protocol/messages.ts';
import { Command, UNLOCK_WINDOW_SECONDS } from '../protocol/wire.ts';
import { useSecondsLeft, useUnlockPrompt } from './use-unlock-prompt.ts';

/** What waits for the unlock, in the reader's words, by the request that answered LOCKED. */
const WAITING_FOR: Readonly<Record<number, string>> = Object.freeze({
  [Command.FwBegin]: 'The firmware upload',
  [Command.Reboot]: 'Rebooting into the bootloader',
  [Command.FactoryReset]: 'The factory reset',
});

/**
 * Asks the user to unlock the device (§4.7) while a request that answered LOCKED waits. The
 * request goes on by itself once the device says it is unlocked; Cancel stops it.
 *
 * @param props - The connected device's client.
 * @returns The dialog, open while a request waits.
 */
export function UnlockDialog({ client }: { client: SysexClient | null }) {
  const prompt = useUnlockPrompt(client);
  const secondsLeft = useSecondsLeft(prompt?.windowEndsAt ?? null);
  const what = prompt === null ? 'This' : (WAITING_FOR[prompt.cmd] ?? 'This request');
  return (
    <DialogLayout
      open={prompt !== null}
      onOpenChange={(open) => {
        if (open === false) {
          prompt?.cancel();
        }
      }}
      size="sm"
      title="Unlock the device"
      description={`${what} needs your OK on the device first.`}
      contentSlot={
        prompt === null ? null : (
          <div className="flex flex-col gap-3">
            <p className="font-medium text-lg">{unlockInstruction(prompt.buttons, prompt.holdMs)}</p>
            <p className="text-foreground/60 text-sm">
              Its status LED blinks fast while it waits, and a display shows HOLD. The page goes on by itself once the
              device is unlocked.
            </p>
            <Progress
              value={secondsLeft}
              max={UNLOCK_WINDOW_SECONDS}
              label="Time left to unlock"
              valueLabel={`${secondsLeft} s left`}
            />
            <p className="text-foreground/60 text-sm">
              {secondsLeft > 0
                ? `The device waits ${secondsLeft} s more.`
                : 'The time to unlock has run out. Still listening for a few seconds.'}
            </p>
          </div>
        )
      }
      footerActionsSlot={(close) => <Button variant="outline" onClick={close} content="Cancel" />}
    />
  );
}
