import { useCallback, useEffect, useState } from 'react';
import type { SysexClient } from '../protocol/client.ts';
import { type Component, fbReleasePayload, listComponents } from '../protocol/feedback.ts';
import type { HelloInfo } from '../protocol/messages.ts';
import { Capability, Notification, ReleaseWhat, Role } from '../protocol/wire.ts';

/** Where loading the component list is. */
export type FeedbackState =
  | { kind: 'loading' }
  | { kind: 'error'; message: string }
  | { kind: 'ready'; components: Component[] };

/** What {@link useFeedback} hands the card. */
export interface FeedbackController {
  state: FeedbackState;
  /** The last notification that could not be sent, in a reader's words; null when the last one went. */
  sendError: string | null;
  /**
   * Sends one notification. The payload is built here so a value the builder refuses becomes
   * {@link FeedbackController.sendError} rather than a thrown error.
   */
  send: (cmd: Notification, build: () => number[]) => void;
  /** FB_RELEASE 0: hands every state back to the device. */
  releaseAll: () => void;
  reload: () => void;
}

/**
 * @param error - Anything thrown.
 * @returns Its message.
 */
function messageOf(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

/**
 * @param info - HELLO from the connected device, or null.
 * @returns Whether it is the app and takes host feedback (§4.1 bit 6).
 */
export function offersFeedback(info: HelloInfo | null): boolean {
  return info !== null && info.role === Role.App && (info.caps & Capability.Feedback) !== 0;
}

/**
 * Loads the device's components with GET_COMPONENT and sends FB_* notifications to it.
 *
 * @param client - The connected device, which has HELLO caps bit 6.
 * @returns The state and the actions.
 */
export function useFeedback(client: SysexClient): FeedbackController {
  const [state, setState] = useState<FeedbackState>({ kind: 'loading' });
  const [sendError, setSendError] = useState<string | null>(null);
  const [attempt, setAttempt] = useState(0);

  // biome-ignore lint/correctness/useExhaustiveDependencies: `attempt` is the reload trigger.
  useEffect(() => {
    const controller = new AbortController();
    setState({ kind: 'loading' });
    setSendError(null);
    listComponents(client, { signal: controller.signal }).then(
      (components) => setState({ kind: 'ready', components }),
      (error: unknown) => {
        if (controller.signal.aborted === false) {
          setState({ kind: 'error', message: messageOf(error) });
        }
      },
    );
    return () => controller.abort();
  }, [client, attempt]);

  const send = useCallback(
    (cmd: Notification, build: () => number[]) => {
      try {
        client.notify(cmd, build());
        setSendError(null);
      } catch (error) {
        setSendError(messageOf(error));
      }
    },
    [client],
  );

  const releaseAll = useCallback(
    () => send(Notification.FbRelease, () => fbReleasePayload(ReleaseWhat.Everything)),
    [send],
  );

  const reload = useCallback(() => setAttempt((n) => n + 1), []);

  return { state, sendError, send, releaseAll, reload };
}
