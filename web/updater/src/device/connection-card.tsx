import { useState } from 'react';
import { CardLayout } from '@/components/card-layout';
import { Combobox } from '@/components/combobox';
import { FormField } from '@/components/form-field';
import { Alert } from '@/components/ui/alert';
import { Button } from '@/components/ui/button';
import { Plug } from '@/components/ui/icons';
import { Input } from '@/components/ui/input';
import { SegmentedButton, SegmentedGroup } from '@/components/ui/segmented';
import type { SlotNode } from '@/lib/utils';
import type { ConnectionSnapshot } from '../midi/device-manager.ts';
import { MidiUnavailable } from '../midi/device-manager.ts';
import { PortRole } from '../protocol/ports.ts';
import { DEFAULT_SERIAL_BAUD, type SerialSnapshot, SerialUnavailable } from '../serial/serial-link.ts';
import type { AccessState } from './use-connection.ts';
import type { SerialAccess } from './use-serial.ts';

/** How the page talks to the device. */
export type TransportMode = 'midi' | 'serial';

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

/** Why Web Serial is unavailable, in the reader's words. */
const SERIAL_UNAVAILABLE_TEXT: Readonly<Record<SerialUnavailable, { title: string; description: string }>> =
  Object.freeze({
    [SerialUnavailable.InsecureContext]: {
      title: 'This page needs HTTPS',
      description: 'Web Serial only works on a page served over HTTPS or from localhost.',
    },
    [SerialUnavailable.NoWebSerial]: {
      title: 'This browser has no Web Serial',
      description: 'Use Chrome or Edge on a desktop. Firefox and Safari do not support Web Serial.',
    },
  });

/** The lowest baud rate accepted. */
const MIN_BAUD = 300;

/** Props of {@link ConnectionCard}. */
export interface ConnectionCardProps {
  mode: TransportMode;
  onModeChange: (mode: TransportMode) => void;
  access: AccessState;
  snapshot: ConnectionSnapshot;
  onConnect: () => void;
  onSelect: (id: string | null) => void;
  serialAccess: SerialAccess;
  serialSnapshot: SerialSnapshot;
  onSerialConnect: (baudRate: number) => void;
  onSerialDisconnect: () => void;
  /** No switching transports in the middle of an upload. */
  isBusy: boolean;
}

/**
 * Choosing how to reach the device, USB MIDI or a serial port, and then the port.
 *
 * @param props - Both transports' state and handlers.
 * @returns The card.
 */
export function ConnectionCard(props: ConnectionCardProps) {
  const { mode, onModeChange, isBusy } = props;
  const [baud, setBaud] = useState(String(DEFAULT_SERIAL_BAUD));
  const isMidi = mode === 'midi';
  const part = isMidi ? midiPart(props) : serialPart(props, baud, setBaud);
  return (
    <CardLayout
      title="Connection"
      iconSlot={<Plug />}
      description={
        isMidi
          ? 'USB MIDI: the Keystation, and ESP32-S2/S3 builds with native USB. A gemifi port is picked by itself, and followed when the device reboots.'
          : 'Serial: classic ESP32 and ESP8266 builds, which speak MIDI over the board’s USB-UART.'
      }
      contentSlot={
        <>
          <FormField
            label="Transport"
            asGroup
            controlSlot={(wired) => (
              <SegmentedGroup {...wired} value={mode} onValueChange={(next: TransportMode) => onModeChange(next)}>
                <SegmentedButton value="midi" disabled={isBusy}>
                  USB MIDI
                </SegmentedButton>
                <SegmentedButton value="serial" disabled={isBusy}>
                  Serial (ESP)
                </SegmentedButton>
              </SegmentedGroup>
            )}
          />
          {part.content}
        </>
      }
      footerActionsSlot={part.actions}
    />
  );
}

/** One transport's share of the card. */
interface Part {
  content: SlotNode;
  actions: SlotNode;
}

/**
 * @param props - The card's props.
 * @returns MIDI access and the port picker.
 */
function midiPart({ access, snapshot, onConnect, onSelect }: ConnectionCardProps): Part {
  if (access.kind === 'unavailable') {
    const text = UNAVAILABLE_TEXT[access.reason];
    return { content: <Alert variant="warning" title={text.title} description={text.description} />, actions: null };
  }
  if (access.kind !== 'ready') {
    return {
      content: (
        <>
          <p className="text-foreground/60 text-sm">
            The browser asks for permission to use MIDI devices, including SysEx. Firefox asks you to add a small
            site-permission add-on first.
          </p>
          {access.kind === 'denied' ? (
            <Alert variant="destructive" title="MIDI access was not granted" description={access.message} />
          ) : null}
        </>
      ),
      actions: (
        <Button
          variant="info"
          iconSlot={<Plug />}
          content={access.kind === 'denied' ? 'Try again' : 'Connect'}
          loading={access.kind === 'requesting'}
          loadingLabel="Waiting for permission…"
          onClick={onConnect}
        />
      ),
    };
  }
  const options = snapshot.pairs.map((pair) => ({
    value: pair.id,
    label: pair.name,
    hint: ROLE_LABELS[pair.role],
  }));
  const hasGemifi = snapshot.pairs.some((pair) => pair.role !== PortRole.Other);
  return {
    content: (
      <>
        <FormField
          label="MIDI port"
          controlSlot={(wired) => (
            <Combobox
              {...wired}
              options={options}
              value={snapshot.selected?.id ?? ''}
              onValueChange={(id) => onSelect(id === '' ? null : id)}
              placeholder={options.length === 0 ? 'No MIDI ports' : 'Choose a port'}
            />
          )}
        />
        {hasGemifi ? null : (
          <p className="text-foreground/60 text-sm">
            No gemifi device found yet. Plug it in; it appears here as soon as the browser sees it.
          </p>
        )}
      </>
    ),
    actions: null,
  };
}

/**
 * @param text - What was typed.
 * @returns The baud rate, or null when it is not one.
 */
function readBaud(text: string): number | null {
  const n = Number(text);
  return Number.isInteger(n) && n >= MIN_BAUD ? n : null;
}

/**
 * @param props - The card's props.
 * @param baud - The baud rate as typed.
 * @param setBaud - Changes it.
 * @returns The baud rate, the open port and Choose port / Disconnect.
 */
function serialPart(props: ConnectionCardProps, baud: string, setBaud: (text: string) => void): Part {
  const { serialAccess, serialSnapshot, onSerialConnect, onSerialDisconnect, isBusy } = props;
  if (serialAccess.kind === 'unavailable') {
    const text = SERIAL_UNAVAILABLE_TEXT[serialAccess.reason];
    return { content: <Alert variant="warning" title={text.title} description={text.description} />, actions: null };
  }
  const isOpen = serialSnapshot.port !== null;
  const baudRate = readBaud(baud);
  const content = (
    <>
      <FormField
        label="Baud rate"
        description="gemifi’s serial MIDI runs at 115200 baud."
        error={baudRate === null ? 'A whole number of at least 300.' : undefined}
        controlSlot={<Input type="number" inputMode="numeric" value={baud} disabled={isOpen} onChangeText={setBaud} />}
      />
      {isOpen ? (
        <p className="text-sm">
          Open: <span className="font-mono">{serialSnapshot.label}</span> at {serialSnapshot.baudRate} baud.
        </p>
      ) : (
        <p className="text-foreground/60 text-sm">
          The browser lists the serial ports it can see; choose the board’s. Opening it does not reset the board.
        </p>
      )}
      {serialAccess.kind === 'failed' ? (
        <Alert variant="destructive" title="The serial port did not open" description={serialAccess.message} />
      ) : null}
      {serialSnapshot.error === null ? null : (
        <Alert variant="warning" title="Disconnected" description={serialSnapshot.error} />
      )}
    </>
  );
  const actions = isOpen ? (
    <Button variant="outline" content="Disconnect" disabled={isBusy} onClick={onSerialDisconnect} />
  ) : (
    <Button
      variant="info"
      iconSlot={<Plug />}
      content="Choose port"
      disabled={baudRate === null}
      loading={serialAccess.kind === 'requesting'}
      loadingLabel="Waiting for a port…"
      onClick={() => onSerialConnect(baudRate ?? DEFAULT_SERIAL_BAUD)}
    />
  );
  return { content, actions };
}
