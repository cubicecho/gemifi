import { useId, useState } from 'react';
import { CardLayout } from '@/components/card-layout';
import { Combobox } from '@/components/combobox';
import { ConfirmButton } from '@/components/confirm-button';
import { FormField } from '@/components/form-field';
import { PropertyList, PropertyRow } from '@/components/property-list';
import { Alert } from '@/components/ui/alert';
import { CodeBlock } from '@/components/ui/code';
import { FilePicker, FilePickerButton } from '@/components/ui/file-picker';
import type { PickedFile } from '@/components/ui/file-picker-base';
import { Download } from '@/components/ui/icons';
import { Progress } from '@/components/ui/progress';
import { SwitchField } from '@/components/ui/switch-field';
import { hex } from '../protocol/image.ts';
import {
  DEFAULT_INSTALL_BAUD,
  INSTALL_BAUD_RATES,
  type InstallProgress,
  type InstallResult,
  installFirmware,
} from '../serial/esp-install.ts';
import { type PickedBytes, planInstall } from '../serial/esp-manifest.ts';
import type { SerialLink } from '../serial/serial-link.ts';

/** Log lines kept on screen. */
const MAX_LOG_LINES = 200;
/** Hex digits for a flash address. */
const ADDRESS_DIGITS = 6;

/** What each stage is called. */
const STAGE_LABELS: Readonly<Record<InstallProgress['stage'], string>> = Object.freeze({
  connecting: 'Connecting to the ROM bootloader',
  erasing: 'Erasing the flash',
  writing: 'Writing',
  resetting: 'Restarting the chip',
  done: 'Done',
});

/** Where an install is. */
type RunState =
  | { kind: 'idle' }
  | { kind: 'running'; progress: InstallProgress }
  | { kind: 'done'; result: InstallResult }
  | { kind: 'failed'; message: string };

/** Props of {@link InstallCard}. */
export interface InstallCardProps {
  /** The page's serial link, closed first if it holds the port chosen here. */
  serialLink: SerialLink;
  /** The files to install: picked here, or the image handed over by the builder. */
  files: readonly PickedBytes[];
  onFilesChange: (files: PickedBytes[]) => void;
  /** Another upload is running. */
  isBusy: boolean;
  onRunningChange: (isRunning: boolean) => void;
}

/**
 * What was picked, as rows.
 *
 * @param props - The picked files.
 * @returns The rows, or what is wrong.
 */
function PlanFacts({ files }: { files: readonly PickedBytes[] }) {
  const plan = planInstall(files);
  if (plan.kind === 'invalid') {
    return <Alert variant="destructive" title="These files cannot be installed" description={plan.problem} />;
  }
  if (plan.kind === 'merged') {
    return (
      <PropertyList
        contentSlot={[
          <PropertyRow key="file" label="File" value={plan.part.name} valueClassName="font-mono break-all" />,
          <PropertyRow key="at" label="Written at" value="0x0 (a merged factory image)" />,
          <PropertyRow key="size" label="Size" value={`${plan.part.data.length} bytes`} />,
        ]}
      />
    );
  }
  const { manifest } = plan;
  return (
    <PropertyList
      contentSlot={[
        <PropertyRow
          key="name"
          label="Manifest"
          value={manifest.version === '' ? manifest.name : `${manifest.name} ${manifest.version}`}
        />,
        <PropertyRow key="chips" label="Chips" value={manifest.builds.map((b) => b.chipFamily).join(', ')} />,
        ...manifest.builds.map((build) => (
          <PropertyRow
            key={build.chipFamily}
            label={build.chipFamily}
            valueClassName="font-mono break-all"
            value={build.parts.map((p) => `${hex(p.offset, ADDRESS_DIGITS)} ${p.path}`).join(', ')}
          />
        )),
        <PropertyRow
          key="files"
          label="Files picked"
          valueClassName="font-mono break-all"
          value={plan.files.length === 0 ? 'none yet' : plan.files.map((f) => f.name).join(', ')}
        />,
      ]}
    />
  );
}

/**
 * The first install on an ESP chip, through its ROM bootloader with esptool-js over Web Serial.
 * After this the app takes updates itself, from the Firmware card.
 *
 * @param props - The serial link, the files and the busy state.
 * @returns The card.
 */
export function InstallCard({ serialLink, files, onFilesChange, isBusy, onRunningChange }: InstallCardProps) {
  const eraseId = useId();
  const [baud, setBaud] = useState(String(DEFAULT_INSTALL_BAUD));
  const [eraseAll, setEraseAll] = useState(false);
  const [run, setRun] = useState<RunState>({ kind: 'idle' });
  // New files, picked here or handed over by the builder, clear the last install's outcome.
  const [shownFiles, setShownFiles] = useState(files);
  if (shownFiles !== files) {
    setShownFiles(files);
    setRun({ kind: 'idle' });
  }
  const [log, setLog] = useState<string[]>([]);
  const isRunning = run.kind === 'running';
  const isReady = files.length > 0 && planInstall(files).kind !== 'invalid';

  const start = async () => {
    let port: SerialPort;
    try {
      port = await navigator.serial.requestPort();
    } catch {
      return; // the chooser was closed
    }
    onRunningChange(true);
    setLog([]);
    try {
      if (serialLink.getSnapshot().port === port) {
        await serialLink.close();
      }
      const result = await installFirmware({
        port,
        files,
        baudRate: Number(baud),
        eraseAll,
        onProgress: (progress) => setRun({ kind: 'running', progress }),
        onLog: (line) => setLog((lines) => [...lines, line].slice(-MAX_LOG_LINES)),
      });
      setRun({ kind: 'done', result });
    } catch (error) {
      setRun({ kind: 'failed', message: error instanceof Error ? error.message : String(error) });
    } finally {
      onRunningChange(false);
    }
  };

  const progress = run.kind === 'running' ? run.progress : null;
  // The drop zone until files are chosen; after that a small button, so the plan leads.
  const pickOptions = {
    accept: '.bin,.json',
    read: 'bytes',
    multiple: true,
    onPickMany: (picked: PickedFile[]) => {
      onFilesChange(picked.flatMap((file) => (file.bytes ? [{ name: file.name, bytes: file.bytes }] : [])));
    },
  } as const;
  return (
    <CardLayout
      title="Install firmware (first flash)"
      iconSlot={<Download />}
      description="For an ESP32 that has no gemifi firmware yet, or to recover one. It goes through the chip's ROM bootloader (like ESPHome and WLED); later updates go through the Firmware card. Chrome or Edge only."
      contentSlot={
        <>
          {files.length === 0 ? (
            <FilePicker
              label="Choose a merged .bin, or a manifest.json and its files"
              hint="A factory image is written at 0x0; a manifest (ESP Web Tools format) says where each file goes"
              {...pickOptions}
            />
          ) : (
            <FilePickerButton
              variant="outline"
              size="sm"
              label="Switch files"
              className="self-start"
              {...pickOptions}
            />
          )}
          {files.length === 0 ? null : <PlanFacts files={files} />}
          <FormField
            label="Flashing speed"
            description="Drop to 115200 if a cheap USB cable or adapter fails at the higher speeds."
            controlSlot={(wired) => (
              <Combobox
                {...wired}
                disabled={isRunning}
                options={INSTALL_BAUD_RATES.map((rate) => ({ value: String(rate), label: `${rate} baud` }))}
                value={baud}
                onValueChange={setBaud}
              />
            )}
          />
          <SwitchField
            id={eraseId}
            label="Erase the whole flash first"
            description="Clears settings and partitions left by other firmware. Slower."
            checked={eraseAll}
            disabled={isRunning}
            onCheckedChange={setEraseAll}
          />
          {progress === null ? null : (
            <>
              <p className="text-sm">
                {STAGE_LABELS[progress.stage]}
                {progress.stage === 'writing' && progress.partCount > 1
                  ? ` part ${progress.partIndex + 1} of ${progress.partCount}`
                  : ''}
                …
              </p>
              {progress.stage === 'writing' && progress.total > 0 ? (
                <Progress
                  value={progress.written}
                  max={progress.total}
                  label="Install"
                  valueLabel={`${progress.written} of ${progress.total} bytes`}
                />
              ) : null}
            </>
          )}
          {run.kind === 'done' ? (
            <Alert
              variant="info"
              title="Installed"
              description={`${run.result.chip}: wrote ${run.result.parts.length} file${run.result.parts.length === 1 ? '' : 's'} and restarted it. Connect over USB MIDI or Serial to check it.`}
            />
          ) : null}
          {run.kind === 'failed' ? (
            <Alert variant="destructive" title="The install failed" description={run.message} />
          ) : null}
          {log.length === 0 ? null : <CodeBlock title="esptool" content={log.join('\n')} wrap maxHeight="sm" follow />}
        </>
      }
      footerActionsSlot={
        <ConfirmButton
          variant="positive"
          content="Install"
          loading={isRunning}
          disabled={isReady === false || isBusy}
          hint={isReady ? undefined : 'Choose a merged image, or a manifest and its files'}
          title="Install this firmware?"
          description="The browser asks for the board's serial port. Everything at the addresses written is replaced. If the chip does not enter its bootloader by itself, hold BOOT (IO0) while pressing RESET, then try again."
          confirmLabel="Choose port and install"
          onConfirm={() => void start()}
        />
      }
    />
  );
}
