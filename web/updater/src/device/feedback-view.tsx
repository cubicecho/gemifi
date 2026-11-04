import type { SysexClient } from '../protocol/client.ts';
import { FeedbackCard } from './feedback-card.tsx';
import { FeedbackDemoCard } from './feedback-demo-card.tsx';
import { useFeedback } from './use-feedback.ts';

/** Props of {@link FeedbackView}. */
export interface FeedbackViewProps {
  /** The connected device, which has HELLO caps bit 6. */
  client: SysexClient;
}

/**
 * The host-feedback cards: the feedback demo and the per-component test controls. They share one
 * component list, read once with GET_COMPONENT.
 *
 * @param props - The client.
 * @returns The cards.
 */
export function FeedbackView({ client }: FeedbackViewProps) {
  const feedback = useFeedback(client);
  return (
    <div className="flex min-w-0 flex-col gap-4">
      <FeedbackDemoCard client={client} feedback={feedback} />
      <FeedbackCard feedback={feedback} />
    </div>
  );
}
