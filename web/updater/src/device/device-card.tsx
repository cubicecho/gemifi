import { useState } from 'react';
import { CardLayout } from '@/components/card-layout';
import { DescriptionList, PropertyRow } from '@/components/description-list';
import { Alert } from '@/components/ui/alert';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { Monitor, RefreshCw } from '@/components/ui/icons';
import { boardLabel } from '../protocol/boards.ts';
import type { SysexClient } from '../protocol/client.ts';
import { toHex } from '../protocol/encoding.ts';
import { reboot } from '../protocol/flash.ts';
import { capabilityNames, formatVersion, type HelloInfo } from '../protocol/messages.ts';
import { RebootMode, Role } from '../protocol/wire.ts';
import type { HelloState } from './use-hello.ts';

/** Props of {@link DeviceCard}. */
export interface DeviceCardProps {
  client: SysexClient | null;
  hello: HelloState;
  onRefresh: () => void;
  /** An upload is running, so reboot buttons are held. */
  isBusy: boolean;
}

/**
 * The HELLO facts as rows.
 *
 * @param props - The device's answer.
 * @returns The list.
 */
function HelloFacts({ info }: { info: HelloInfo }) {
  const caps = capabilityNames(info.caps);
  return (
    <DescriptionList
      contentSlot={[
        <PropertyRow key="role" label="Role" value={info.role === Role.Boot ? 'Bootloader' : 'App'} />,
        <PropertyRow key="board" label="Board" value={boardLabel(info.boardId)} />,
        <PropertyRow key="fw" label="Firmware" value={formatVersion(info.fwVersion)} valueClassName="font-mono" />,
        <PropertyRow key="proto" label="Protocol" value={`v${info.protoVersion}`} valueClassName="font-mono" />,
        <PropertyRow
          key="chunk"
          label="Max chunk"
          value={info.maxChunk === 0 ? 'none (app)' : `${info.maxChunk} bytes`}
        />,
        <PropertyRow key="caps" label="Capabilities" value={caps.length === 0 ? 'none' : caps.join(', ')} />,
        <PropertyRow
          key="uid"
          label="UID"
          value={toHex(info.uid).replaceAll(' ', '')}
          valueClassName="font-mono break-all"
        />,
      ]}
    />
  );
}

/**
 * What the connected device says about itself, and the reboot buttons.
 *
 * @param props - The client, its HELLO answer and handlers.
 * @returns The card.
 */
export function DeviceCard({ client, hello, onRefresh, isBusy }: DeviceCardProps) {
  const [rebootError, setRebootError] = useState<string | null>(null);
  const [pending, setPending] = useState<RebootMode | null>(null);
  const role = hello.kind === 'ok' ? hello.info.role : null;

  const send = (mode: RebootMode) => {
    if (client === null) {
      return;
    }
    setRebootError(null);
    setPending(mode);
    reboot(client, mode)
      .catch((error: unknown) => setRebootError(error instanceof Error ? error.message : String(error)))
      .finally(() => setPending(null));
  };

  const canReboot = client !== null && isBusy === false;
  return (
    <CardLayout
      title="Device"
      iconSlot={<Monitor />}
      actionSlot={
        <>
          {role === null ? null : (
            <Badge variant={role === Role.Boot ? 'warning-soft' : 'positive-soft'}>
              {role === Role.Boot ? 'Bootloader' : 'App'}
            </Badge>
          )}
          <Button
            size="icon-sm"
            variant="outline"
            aria-label="Ask the device again"
            iconSlot={<RefreshCw />}
            disabled={client === null}
            onClick={onRefresh}
          />
        </>
      }
      loading={hello.kind === 'loading'}
      contentSlot={
        <>
          {hello.kind === 'ok' ? <HelloFacts info={hello.info} /> : null}
          {hello.kind === 'error' ? (
            <Alert variant="destructive" title="The device did not answer HELLO" description={hello.message} />
          ) : null}
          {hello.kind === 'none' ? <p className="text-foreground/60 text-sm">No device connected.</p> : null}
          {rebootError === null ? null : (
            <Alert variant="destructive" title="Reboot failed" description={rebootError} />
          )}
        </>
      }
      footerActionsSlot={
        <>
          <Button
            variant="outline"
            content="Reboot to bootloader"
            disabled={canReboot === false || role === Role.Boot}
            loading={pending === RebootMode.Boot}
            onClick={() => send(RebootMode.Boot)}
          />
          <Button
            variant="outline"
            content="Reboot to app"
            disabled={canReboot === false}
            loading={pending === RebootMode.App}
            onClick={() => send(RebootMode.App)}
          />
        </>
      }
    />
  );
}
