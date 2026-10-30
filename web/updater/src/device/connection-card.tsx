import { CardLayout } from '@/components/card-layout';
import { Combobox } from '@/components/combobox';
import { Alert } from '@/components/ui/alert';
import { Button } from '@/components/ui/button';
import { Plug } from '@/components/ui/icons';
import type { ConnectionSnapshot } from '../midi/device-manager.ts';
import { MidiUnavailable } from '../midi/device-manager.ts';
import { PortRole } from '../protocol/ports.ts';
import type { AccessState } from './use-connection.ts';

/** What each port role is called next to a port's name. */
const ROLE_LABELS: Readonly<Record<PortRole, string>> = Object.freeze({
  app: 'gemifi app',
  boot: 'gemifi bootloader',
  other: 'not gemifi',
});

/** Why Web MIDI is unavailable, in the reader's words. */
const UNAVAILABLE_TEXT: Readonly<Record<MidiUnavailable, { title: string; description: string }>> = Object.freeze({
  [MidiUnavailable.InsecureContext]: {
    title: 'This page needs HTTPS',
    description: 'Web MIDI only works on a page served over HTTPS or from localhost.',
  },
  [MidiUnavailable.NoWebMidi]: {
    title: 'This browser has no Web MIDI',
    description: 'Use Chrome, Edge or Firefox on a desktop. Safari does not support Web MIDI.',
  },
});

/** Props of {@link ConnectionCard}. */
export interface ConnectionCardProps {
  access: AccessState;
  snapshot: ConnectionSnapshot;
  onConnect: () => void;
  onSelect: (id: string | null) => void;
}

/**
 * Asking for MIDI access, and choosing the port the device is on.
 *
 * @param props - The access state, the ports and the handlers.
 * @returns The card.
 */
export function ConnectionCard({ access, snapshot, onConnect, onSelect }: ConnectionCardProps) {
  if (access.kind === 'unavailable') {
    const text = UNAVAILABLE_TEXT[access.reason];
    return <Alert variant="warning" title={text.title} description={text.description} />;
  }
  if (access.kind !== 'ready') {
    return (
      <CardLayout
        title="Connect"
        iconSlot={<Plug />}
        description="The browser asks for permission to use MIDI devices, including SysEx. Firefox asks you to add a small site-permission add-on first."
        contentSlot={
          access.kind === 'denied' ? (
            <Alert variant="destructive" title="MIDI access was not granted" description={access.message} />
          ) : null
        }
        footerActionsSlot={
          <Button
            variant="info"
            iconSlot={<Plug />}
            content={access.kind === 'denied' ? 'Try again' : 'Connect'}
            loading={access.kind === 'requesting'}
            loadingLabel="Waiting for permission…"
            onClick={onConnect}
          />
        }
      />
    );
  }

  const options = snapshot.pairs.map((pair) => ({
    value: pair.id,
    label: pair.name,
    hint: ROLE_LABELS[pair.role],
  }));
  const hasGemifi = snapshot.pairs.some((pair) => pair.role !== PortRole.Other);
  return (
    <CardLayout
      title="Port"
      iconSlot={<Plug />}
      description="A gemifi port is picked by itself, and followed when the device reboots."
      contentSlot={
        <>
          <Combobox
            aria-label="MIDI port"
            options={options}
            value={snapshot.selected?.id ?? ''}
            onValueChange={(id) => onSelect(id === '' ? null : id)}
            placeholder={options.length === 0 ? 'No MIDI ports' : 'Choose a port'}
          />
          {hasGemifi ? null : (
            <p className="text-foreground/60 text-sm">
              No gemifi device found yet. Plug it in; it appears here as soon as the browser sees it.
            </p>
          )}
        </>
      }
    />
  );
}
