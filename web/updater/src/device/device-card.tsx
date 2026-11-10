import { useState } from 'react';
import { CardLayout } from '@/components/card-layout';
import { PropertyList, PropertyRow } from '@/components/property-list';
import { Section } from '@/components/section';
import { Alert } from '@/components/ui/alert';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { FileText, Monitor, RefreshCw } from '@/components/ui/icons';
import type { BuilderDevice } from '../builder/api.ts';
import { ConfigMatch, configMatch, formatConfigHash } from '../builder/node-match.ts';
import { deviceTitle } from '../builder/platforms.ts';
import { boardLabel } from '../protocol/boards.ts';
import type { SysexClient } from '../protocol/client.ts';
import { toHex } from '../protocol/encoding.ts';
import { reboot } from '../protocol/flash.ts';
import { capabilityNames, formatVersion, type HelloInfo } from '../protocol/messages.ts';
import { RebootMode, Role } from '../protocol/wire.ts';
import type { DiagnosticsState } from './use-diagnostics.ts';
import type { HelloState } from './use-hello.ts';

/** Props of {@link DeviceCard}. */
export interface DeviceCardProps {
  client: SysexClient | null;
  hello: HelloState;
  /** The diagnostics (§4.10) of a device with config; `none` otherwise. */
  diagnostics: DiagnosticsState;
  /** Asks HELLO again and reads the diagnostics again. */
  onRefresh: () => void;
  /** An upload is running, so reboot buttons are held. */
  isBusy: boolean;
  /** With a builder: the device files with the connected device's node name, and how to open one. */
  deviceFiles: DeviceFiles | null;
  /** The device's `config.hash` (the YAML it was built from), or null when it has none or it is not read yet. */
  configHash: number | null;
}

/** The device files a connected device was built from. */
export interface DeviceFiles {
  /** The user's own first. */
  matches: BuilderDevice[];
  onOpen: (device: BuilderDevice) => void;
}

/** What each {@link ConfigMatch} looks like next to a file. */
const MATCH_LOOKS = {
  [ConfigMatch.Same]: { variant: 'positive-soft', label: 'Runs this config' },
  [ConfigMatch.LastBuild]: { variant: 'warning-soft', label: 'Runs its last build; the file has changed since' },
  [ConfigMatch.Different]: { variant: 'warning-soft', label: 'Runs a different config than this file' },
} as const;

/**
 * The files whose `name:` is the device's node name, as buttons that open them in the Devices view,
 * each with whether the device runs that file's config (its `config.hash`) when both sides say.
 *
 * @param props - The node name, the files and the device's config hash.
 * @returns The line, or the buttons.
 */
function MatchingFiles({
  nodeName,
  files,
  configHash,
}: {
  nodeName: string;
  files: DeviceFiles;
  configHash: number | null;
}) {
  if (files.matches.length === 0) {
    return (
      <p className="text-foreground/60 text-sm">
        No device file here has <span className="font-mono">name: {nodeName}</span>.
      </p>
    );
  }
  return (
    <div className="flex flex-col gap-2">
      {files.matches.map((device) => {
        const match = configMatch(device, configHash);
        const look = match === ConfigMatch.Unknown ? null : MATCH_LOOKS[match];
        return (
          <div key={device.id} className="flex flex-wrap items-center gap-2">
            <Button
              size="sm"
              variant="outline"
              iconSlot={<FileText />}
              content={`Open ${deviceTitle(device)}`}
              title={device.id}
              onClick={() => files.onOpen(device)}
            />
            {look === null ? null : <Badge variant={look.variant}>{look.label}</Badge>}
          </div>
        );
      })}
    </div>
  );
}

/**
 * The HELLO facts as rows.
 *
 * @param props - The device's answer.
 * @returns The list.
 */
function HelloFacts({ info, configHash }: { info: HelloInfo; configHash: number | null }) {
  const caps = capabilityNames(info.caps);
  return (
    <PropertyList
      contentSlot={[
        <PropertyRow key="role" label="Role" value={info.role === Role.Boot ? 'Bootloader' : 'App'} />,
        ...(info.nodeName === null
          ? []
          : [<PropertyRow key="node" label="Node name" value={info.nodeName} valueClassName="font-mono" />]),
        ...(configHash === null
          ? []
          : [
              <PropertyRow
                key="config"
                label="Config hash"
                value={formatConfigHash(configHash)}
                valueClassName="font-mono"
              />,
            ]),
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
 * The diagnostics the device reports (§4.10): why it started, scan timings, uptime. Those it does
 * not report are left out, and so is the section when it reports none.
 *
 * @param props - The reading.
 * @returns The section, a line while it is read, or nothing.
 */
function DiagnosticsFacts({ state }: { state: DiagnosticsState }) {
  if (state.kind === 'loading') {
    return <p className="text-foreground/60 text-sm">Reading the diagnostics…</p>;
  }
  if (state.kind === 'error') {
    return <Alert variant="warning" title="The diagnostics could not be read" description={state.message} />;
  }
  if (state.kind === 'none' || state.diagnostics.length === 0) {
    return null;
  }
  return (
    <Section
      title="Diagnostics"
      level={4}
      contentSlot={
        <PropertyList
          contentSlot={state.diagnostics.map(({ param, text }) => (
            <PropertyRow key={param.id} label={param.label} value={text} />
          ))}
        />
      }
    />
  );
}

/**
 * What the connected device says about itself, and the reboot buttons.
 *
 * @param props - The client, its HELLO answer and handlers.
 * @returns The card.
 */
export function DeviceCard({
  client,
  hello,
  diagnostics,
  onRefresh,
  isBusy,
  deviceFiles,
  configHash,
}: DeviceCardProps) {
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
          {hello.kind === 'ok' ? <HelloFacts info={hello.info} configHash={configHash} /> : null}
          {hello.kind === 'ok' ? <DiagnosticsFacts state={diagnostics} /> : null}
          {hello.kind === 'ok' && hello.info.nodeName !== null && deviceFiles !== null ? (
            <MatchingFiles nodeName={hello.info.nodeName} files={deviceFiles} configHash={configHash} />
          ) : null}
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
