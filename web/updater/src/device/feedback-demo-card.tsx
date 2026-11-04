import { useState } from 'react';
import { CardLayout } from '@/components/card-layout';
import { EmptyState } from '@/components/empty-state';
import { Section } from '@/components/section';
import { Alert } from '@/components/ui/alert';
import { Button } from '@/components/ui/button';
import { ColorDot } from '@/components/ui/color-dot';
import { Play, Square } from '@/components/ui/icons';
import { SegmentedButton, SegmentedGroup } from '@/components/ui/segmented';
import { DEMO_EFFECTS, PREVIEW_CSS } from '../demo/effects.ts';
import type { SysexClient } from '../protocol/client.ts';
import { hostDrives } from '../protocol/feedback.ts';
import type { FeedbackController } from './use-feedback.ts';
import { DEMO_SPEEDS, type DemoSpeed, useFeedbackDemo } from './use-feedback-demo.ts';

/** Props of {@link FeedbackDemoCard}. */
export interface FeedbackDemoCardProps {
  /** The connected device, which has HELLO caps bit 6. */
  client: SysexClient;
  /** The component list and the sender, shared with the Host feedback card. */
  feedback: FeedbackController;
}

/** The first effect, picked before anything runs. */
const FIRST_EFFECT = DEMO_EFFECTS[0]?.id ?? '';

/**
 * Light shows on every LED and display the device lists, sent with the same host-feedback
 * notifications a DAW script uses (FB_LED_RGB, FB_TEXT). Stop hands everything back (FB_RELEASE).
 *
 * @param props - The client and the shared feedback controller.
 * @returns The card.
 */
export function FeedbackDemoCard({ client, feedback }: FeedbackDemoCardProps) {
  const demo = useFeedbackDemo(client, feedback);
  const [picked, setPicked] = useState(FIRST_EFFECT);
  const { state } = feedback;
  const effect = DEMO_EFFECTS.find((e) => e.id === picked) ?? DEMO_EFFECTS[0];
  const hasTargets = demo.leds.length > 0 || demo.displays.length > 0;
  const isRunning = demo.running !== null;
  const ledCount = demo.leds.filter(hostDrives).length;
  const skipped = demo.leds.length - ledCount;
  const pick = (id: string) => {
    setPicked(id);
    if (isRunning) {
      demo.start(id);
    }
  };
  return (
    <CardLayout
      title="Feedback demo"
      iconSlot={<Play />}
      description="Light shows on every LED and display the device has, sent the way a DAW script sends feedback. Stop hands them back to the device."
      loading={state.kind === 'loading'}
      contentSlot={
        <>
          {state.kind === 'error' ? (
            <Alert variant="destructive" title="The components could not be read" description={state.message} />
          ) : null}
          {state.kind === 'ready' && hasTargets === false ? (
            <EmptyState compact title="The device lists no LEDs or displays." />
          ) : null}
          {state.kind === 'ready' && hasTargets ? (
            <div className="flex flex-col gap-4">
              <div className="flex flex-wrap items-center gap-3">
                {/* On a narrow screen the five effects scroll sideways rather than run off the card. */}
                <div className="min-w-0 max-w-full overflow-x-auto">
                  <SegmentedGroup aria-label="Effect" value={picked} onValueChange={pick}>
                    {DEMO_EFFECTS.map((e) => (
                      <SegmentedButton key={e.id} value={e.id}>
                        {e.name}
                      </SegmentedButton>
                    ))}
                  </SegmentedGroup>
                </div>
                <SegmentedGroup
                  aria-label="Speed"
                  value={String(demo.speed)}
                  onValueChange={(next) => demo.setSpeed(Number(next) as DemoSpeed)}
                >
                  {DEMO_SPEEDS.map((s) => (
                    <SegmentedButton key={s} value={String(s)}>
                      {`${s}×`}
                    </SegmentedButton>
                  ))}
                </SegmentedGroup>
              </div>
              <p className="text-foreground/70 text-sm">{effect?.description}</p>
              <p className="text-foreground/60 text-xs">
                {`${ledCount} LED${ledCount === 1 ? '' : 's'}`}
                {demo.displays.length > 0
                  ? ` and ${demo.displays.length} display${demo.displays.length === 1 ? '' : 's'}`
                  : ''}
                {skipped > 0 ? `; ${skipped} with owner: local are left alone` : ''}. The LEDs are on/off per colour, so
                "dimmer" means fewer LEDs lit.
              </p>
              <Section
                title={isRunning ? 'Sending' : 'Preview (stopped)'}
                level={4}
                contentSlot={
                  <div className="flex flex-col gap-3">
                    <ul className="flex flex-wrap gap-1.5" aria-label="LEDs">
                      {demo.leds.map((led, i) => {
                        const colour = demo.frame?.leds[i] ?? 0;
                        const isDriven = hostDrives(led);
                        return (
                          <li key={led.index} title={led.name} className={isDriven ? '' : 'opacity-30'}>
                            <ColorDot color={PREVIEW_CSS[isDriven ? colour : 0] ?? ''} label={led.name} />
                          </li>
                        );
                      })}
                    </ul>
                    {demo.displays.map((display, d) => (
                      <div key={display.index} className="flex items-center gap-2">
                        <span className="text-foreground/60 text-xs">{display.name}</span>
                        <code className="whitespace-pre rounded bg-foreground/90 px-2 py-1 font-mono text-background text-sm tracking-[0.3em]">
                          {(demo.frame?.texts[d] ?? '').padEnd(display.cols)}
                        </code>
                      </div>
                    ))}
                  </div>
                }
              />
            </div>
          ) : null}
          {feedback.sendError === null ? null : (
            <Alert variant="destructive" title="That was not sent" description={feedback.sendError} />
          )}
        </>
      }
      footerActionsSlot={
        isRunning ? (
          <Button variant="outline" content="Stop" iconSlot={<Square />} onClick={demo.stop} />
        ) : (
          <Button
            content="Start"
            iconSlot={<Play />}
            disabled={state.kind !== 'ready' || hasTargets === false}
            onClick={() => demo.start(picked)}
          />
        )
      }
    />
  );
}
