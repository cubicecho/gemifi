import { useRef, useState } from 'react';
import { CardLayout } from '@/components/card-layout';
import { ConfirmButton } from '@/components/confirm-button';
import { FormField } from '@/components/form-field';
import { PropertyList, PropertyRow } from '@/components/property-list';
import { Alert } from '@/components/ui/alert';
import { Button } from '@/components/ui/button';
import { CodeBlock } from '@/components/ui/code';
import { DownloadButton, downloadBlob } from '@/components/ui/download-button';
import { FilePicker, FilePickerButton } from '@/components/ui/file-picker';
import type { PickedFile } from '@/components/ui/file-picker-base';
import { Upload } from '@/components/ui/icons';
import { Input } from '@/components/ui/input';
import { Progress } from '@/components/ui/progress';
import { SwitchField } from '@/components/ui/switch-field';
import { type AbImageResult, isAbDevice, prepareAbImage } from '../protocol/ab-image.ts';
import {
  type BackupDoc,
  type BackupProgress,
  backupFileName,
  backupSummary,
  backupText,
  type RestoreReport,
  readBackup,
  restoreBackup,
  stalePresets,
} from '../protocol/backup.ts';
import { type Board, boardFromLayout, boardLabel, CODE61_BOARD_ID, usesVendorUpdate } from '../protocol/boards.ts';
import type { SysexClient } from '../protocol/client.ts';
import { getSchema } from '../protocol/config.ts';
import {
  type DeviceLink,
  FLASH_PHASE_LABELS,
  FlashError,
  type FlashProgress,
  flashAbFirmware,
  flashFirmware,
  reboot,
} from '../protocol/flash.ts';
import { hex, type ImageResult, validateImage } from '../protocol/image.ts';
import { formatVersion, type HelloInfo } from '../protocol/messages.ts';
import { offersPresets, type PresetInfo, readPresets, resetPreset } from '../protocol/presets.ts';
import { PRESET_ALL, RebootMode, Role } from '../protocol/wire.ts';
import {
  buildUpdateStream,
  type ProbePort,
  parseStockUpdateFile,
  VENDOR_PHASE_LABELS,
  VendorPhase,
  type VendorProgress,
  vendorUpdate,
} from '../vendor/maudio.ts';
import { type PickedImage, pickedImageOf } from './picked-image.ts';
import { offersConfig } from './use-config.ts';

/** Percent, for the progress bar's words. */
const PERCENT = 100;

/** Where an upload is. */
type RunState =
  | { kind: 'idle' }
  /** Reading the settings and presets into a backup before the upload. */
  | { kind: 'backup'; progress: BackupProgress }
  | { kind: 'running'; progress: FlashProgress }
  | { kind: 'vendor'; progress: VendorProgress }
  | { kind: 'done'; info: HelloInfo }
  | { kind: 'vendor-done'; portName: string }
  /** The stock app accepted the update request on Windows: the user closes Chrome before the power cycle. */
  | { kind: 'vendor-restart' }
  | { kind: 'failed'; title: string; message: string };

/** An upload stored before the keyboard vanished, offered again ({@link FlashCardProps.resume}). */
export interface ResumeOffer {
  image: PickedImage;
  /** A port named like M-Audio's bootloader ("Code updater") is connected. */
  isBootloaderPresent: boolean;
  /** Web MIDI access has been granted. */
  isMidiReady: boolean;
}

/** The backup taken before the last upload. */
interface SavedBackup {
  doc: BackupDoc;
  fileName: string;
}

/** What happens to the settings once the new firmware is up. */
type AfterState =
  | { kind: 'none' }
  /** Stored edits from another presets revision: keep them, or use the firmware's presets (§4.5). */
  | { kind: 'stale'; slots: PresetInfo[] }
  | { kind: 'kept' }
  | { kind: 'reset' }
  | { kind: 'working'; what: 'reset' | 'restore'; slots: PresetInfo[] }
  | { kind: 'restored'; report: RestoreReport }
  | { kind: 'failed'; message: string };

/**
 * @param error - Anything thrown.
 * @returns Its message.
 */
function messageOf(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

/**
 * @param isCancelled - The reader pressed Cancel.
 * @param error - What reading the backup threw.
 * @returns The run's outcome: nothing was flashed.
 */
function backupFailed(isCancelled: boolean, error: unknown): RunState {
  return isCancelled
    ? {
        kind: 'failed',
        title: 'Upload cancelled',
        message: 'Cancelled while backing up the settings: nothing was flashed.',
      }
    : {
        kind: 'failed',
        title: 'The settings could not be backed up, so nothing was flashed',
        message: `${messageOf(error)} Switch off "Back up settings and presets first" to flash without a backup.`,
      };
}

/** Props of {@link AfterUpload}. */
interface AfterUploadProps {
  after: AfterState;
  backup: SavedBackup | null;
  /** A gemifi app with config is connected, so the backup can be written back. */
  canRestore: boolean;
  onKeep: () => void;
  onUseFirmware: () => void;
  onRestore: () => void;
}

/**
 * What the settings are after the upload, and what can be done about them: keep or drop preset
 * edits from another presets revision, download the backup again, or write it back.
 *
 * @param props - The state, the backup and the actions.
 * @returns The alert, or nothing.
 */
function AfterUpload({ after, backup, canRestore, onKeep, onUseFirmware, onRestore }: AfterUploadProps) {
  const isWorking = after.kind === 'working';
  const restoreButton =
    backup === null || canRestore === false ? null : (
      <ConfirmButton
        size="sm"
        variant="outline"
        content="Restore backup"
        disabled={isWorking}
        loading={isWorking && after.what === 'restore'}
        loadingLabel="Restoring…"
        title="Put the backup back?"
        description={`Every setting and preset in ${backup.fileName} is written to the device and stored. Presets made since the backup are deleted.`}
        confirmLabel="Restore"
        onConfirm={onRestore}
      />
    );
  const isAsking = after.kind === 'stale' || (after.kind === 'working' && after.what === 'reset');
  if (isAsking) {
    const { slots } = after;
    const where = slots.map((slot) => `${slot.slot} ${slot.name}`).join(', ');
    return (
      <Alert
        variant="warning"
        title="The firmware's presets changed"
        description={
          <>
            <p>{`Your stored edits were made on another version of the presets, in: ${where}.`}</p>
            <p>
              {`Keep them on top of the new presets, or use the firmware's presets (drops your edits and names).${backup === null ? '' : ' The backup can bring them back.'}`}
            </p>
          </>
        }
        actionSlot={
          <>
            <Button size="sm" variant="positive" content="Keep my presets" disabled={isWorking} onClick={onKeep} />
            <ConfirmButton
              size="sm"
              variant="outline"
              content="Use the firmware's presets"
              disabled={isWorking}
              loading={isWorking}
              loadingLabel="Resetting…"
              title="Use the firmware's presets?"
              description="Every preset's stored edits and name are dropped. The shared settings stay."
              confirmLabel="Use the firmware's presets"
              onConfirm={onUseFirmware}
            />
          </>
        }
      />
    );
  }
  if (after.kind === 'restored') {
    const isClean = after.report.problems.length === 0;
    return (
      <Alert
        variant={isClean ? 'info' : 'warning'}
        title={`Backup restored: ${after.report.applied} values${isClean ? '' : ', with problems'}`}
        description={
          isClean ? (
            'Everything is stored on the device.'
          ) : (
            <ul className="list-disc pl-4">
              {after.report.problems.map((line) => (
                <li key={line}>{line}</li>
              ))}
            </ul>
          )
        }
      />
    );
  }
  if (after.kind === 'failed') {
    return (
      <Alert variant="destructive" title="That did not work" description={after.message} actionSlot={restoreButton} />
    );
  }
  if (backup === null) {
    return null;
  }
  const outcome =
    after.kind === 'kept'
      ? ' Your preset edits were kept on top of the new presets.'
      : after.kind === 'reset'
        ? " Every preset is the firmware's again."
        : '';
  return (
    <Alert
      variant="info"
      title="Settings backed up"
      description={`${backup.fileName} (${backupSummary(backup.doc)}) went to your downloads before the upload.${outcome} If the new firmware lost your settings, restore it.`}
      actionSlot={
        <>
          <DownloadButton
            size="sm"
            label="Download again"
            filename={backup.fileName}
            mimeType="application/json"
            source={() => backupText(backup.doc)}
          />
          {restoreButton}
        </>
      }
    />
  );
}

/** What the reader is told on Windows, where Chrome can hang when a MIDI device is switched off. */
const windowsRestartSteps = (
  <>
    <p>
      The keyboard will start in update mode. Chrome on Windows can stop responding when a MIDI device is switched off,
      so:
    </p>
    <ol className="list-decimal pl-4">
      <li>Close Chrome completely.</li>
      <li>Switch the keyboard off and on (its display shows 'update').</li>
      <li>Reopen this page — it offers to finish.</li>
    </ol>
  </>
);

/** Props of {@link FlashCard}. */
export interface FlashCardProps {
  link: DeviceLink | null;
  /** The image waiting to be flashed: picked here, or handed over by the builder. */
  image: PickedImage | null;
  onImageChange: (image: PickedImage | null) => void;
  /** The connected device's HELLO, to check the image's board against. */
  device: HelloInfo | null;
  /** Every MIDI port, for M-Audio's bootloader (Code 61); null without Web MIDI. */
  probePorts: (() => readonly ProbePort[]) | null;
  onRunningChange: (isRunning: boolean) => void;
  /** Called after an upload ends, so the page asks HELLO again (a serial link keeps its client). */
  onFinished: () => void;
  /** An upload stored before the keyboard went into update mode, to finish; null when none. */
  resume: ResumeOffer | null;
  /** Forgets {@link FlashCardProps.resume}. */
  onDismissResume: () => void;
  /** Stores the image before the keyboard vanishes from USB, so a reopened page can finish. */
  onBeforeVanish: (image: PickedImage) => Promise<void>;
  /** Called after M-Audio's bootloader took every record. */
  onUploaded: () => void;
  /** Windows: the stock app's update request ends the run, and the user closes Chrome first. */
  isWindows: boolean;
}

/**
 * @param source - Where the image came from, when the builder handed it over.
 * @returns The row saying so, or none for a picked file.
 */
function sourceRow(source: string | undefined) {
  return source === undefined ? [] : [<PropertyRow key="source" label="From" value={source} />];
}

/**
 * @param problems - What is wrong.
 * @returns The alert listing them.
 */
function Problems({ problems }: { problems: readonly string[] }) {
  return (
    <Alert
      variant="destructive"
      title="This image cannot be flashed"
      description={
        <ul className="list-disc pl-4">
          {problems.map((problem) => (
            <li key={problem}>{problem}</li>
          ))}
        </ul>
      }
    />
  );
}

/**
 * The checked `.gfw` header as rows.
 *
 * @param props - The file name and the check result.
 * @returns The list, or the problems.
 */
function GfwFacts(props: { name: string; source: string | undefined; result: ImageResult; extra: string[] }) {
  const { name, source, result, extra } = props;
  const header = result.ok ? result.image.header : result.header;
  const problems = [...(result.ok ? [] : result.problems), ...extra];
  return (
    <>
      {header === null ? null : (
        <PropertyList
          contentSlot={[
            <PropertyRow key="file" label="File" value={name} valueClassName="font-mono break-all" />,
            ...sourceRow(source),
            <PropertyRow key="board" label="Board" value={boardLabel(header.boardId)} />,
            <PropertyRow key="fw" label="Version" value={formatVersion(header.fwVersion)} valueClassName="font-mono" />,
            <PropertyRow key="size" label="Size" value={`${header.imageSize} bytes`} />,
            <PropertyRow key="crc" label="CRC-32" value={hex(header.crc32)} valueClassName="font-mono" />,
          ]}
        />
      )}
      {problems.length === 0 ? null : <Problems problems={problems} />}
    </>
  );
}

/**
 * A chip image's facts: board from HELLO, the version typed, the padded size and the CRC.
 *
 * @param props - The file, the check result, the version field's state and the device.
 * @returns The rows and the version field.
 */
function AbFacts(props: {
  name: string;
  source: string | undefined;
  result: AbImageResult;
  extra: string[];
  version: string;
  onVersionChange: (text: string) => void;
  device: HelloInfo | null;
  isRunning: boolean;
}) {
  const { name, source, result, extra, version, onVersionChange, device, isRunning } = props;
  const problems = [...(result.ok ? [] : result.problems), ...extra];
  const image = result.ok ? result.image : null;
  return (
    <>
      <FormField
        label="Version"
        required
        description="This image's version, as major.minor.patch. It is sent with FW_BEGIN, and the device must report it after the restart."
        controlSlot={
          <Input
            value={version}
            placeholder="1.2.3"
            autoCapitalize="none"
            autoCorrect={false}
            disabled={isRunning}
            onChangeText={onVersionChange}
          />
        }
      />
      <PropertyList
        contentSlot={[
          <PropertyRow key="file" label="File" value={name} valueClassName="font-mono break-all" />,
          ...sourceRow(source),
          <PropertyRow
            key="board"
            label="Board"
            value={device === null ? 'Connect the device' : `${boardLabel(device.boardId)}, from the device`}
          />,
          ...(image === null
            ? []
            : [
                <PropertyRow
                  key="size"
                  label="Size"
                  value={
                    image.size === image.fileBytes
                      ? `${image.size} bytes`
                      : `${image.fileBytes} bytes, padded to ${image.size}`
                  }
                />,
                <PropertyRow key="crc" label="CRC-32" value={hex(image.crc32)} valueClassName="font-mono" />,
              ]),
        ]}
      />
      {result.warnings.map((warning) => (
        <Alert key={warning} variant="warning" title="Check the file" description={warning} />
      ))}
      {problems.length === 0 ? null : <Problems problems={problems} />}
    </>
  );
}

/**
 * A stock M-Audio update file's facts.
 *
 * @param props - The file, its records or problem, and any mismatch.
 * @returns The rows, or the problems.
 */
function StockFacts(props: {
  name: string;
  result: NonNullable<ReturnType<typeof parseStockUpdateFile>>;
  extra: string[];
}) {
  const { name, result, extra } = props;
  const problems = [...(result.ok ? [] : [result.problem]), ...extra];
  return (
    <>
      <PropertyList
        contentSlot={[
          <PropertyRow key="file" label="File" value={name} valueClassName="font-mono break-all" />,
          <PropertyRow key="kind" label="Kind" value="M-Audio update file, sent as it is (restores stock firmware)" />,
          ...(result.ok
            ? [<PropertyRow key="records" label="Records" value={`${result.records.length}, every checksum good`} />]
            : []),
        ]}
      />
      {problems.length === 0 ? null : <Problems problems={problems} />}
    </>
  );
}

/**
 * Picks an image, checks it before anything is sent, and uploads it: a `.gfw` through the gemifi
 * bootloader, or a chip's own `.bin` to an A/B device (ESP32 / ESP8266), whose app takes it itself.
 *
 * @param props - The device link, the image, the device's HELLO and the busy and finished callbacks.
 * @returns The card.
 */
export function FlashCard({
  link,
  image: picked,
  onImageChange,
  device,
  probePorts,
  onRunningChange,
  onFinished,
  resume,
  onDismissResume,
  onBeforeVanish,
  onUploaded,
  isWindows,
}: FlashCardProps) {
  const [version, setVersion] = useState('');
  const [vendorLog, setVendorLog] = useState<string[]>([]);
  const [run, setRun] = useState<RunState>({ kind: 'idle' });
  // A new image, picked here or handed over by the builder, clears the last upload's outcome.
  const [shownImage, setShownImage] = useState(picked);
  if (shownImage !== picked) {
    setShownImage(picked);
    setRun({ kind: 'idle' });
    setVendorLog([]);
  }
  const controller = useRef<AbortController | null>(null);
  const isRunning = run.kind === 'running' || run.kind === 'vendor' || run.kind === 'backup';
  const isAb = device !== null && isAbDevice(device);
  // Backup before flashing (as `gemifi.py flash`): on by default, for an app with settings.
  // A DIY board's layout (<name>.board.json from its build), picked with or after its .gfw.
  const [layout, setLayout] = useState<{ board: Board | null; problem: string | null }>({
    board: null,
    problem: null,
  });
  const [isBackupOn, setIsBackupOn] = useState(true);
  const [backup, setBackup] = useState<SavedBackup | null>(null);
  const [after, setAfter] = useState<AfterState>({ kind: 'none' });
  const canBackUp = device !== null && offersConfig(device);

  /**
   * Reads the settings and presets into a backup and saves it to the downloads, before an upload.
   * Does nothing when switched off or when the device has no settings (a bootloader).
   *
   * @param signal - Stops it.
   */
  const takeBackup = async (signal: AbortSignal): Promise<void> => {
    const client = link?.current() ?? null;
    if (isBackupOn === false || client === null || device === null || offersConfig(device) === false) {
      return;
    }
    setRun({ kind: 'backup', progress: { done: 0, total: 0 } });
    const schema = await getSchema(client, { signal });
    const doc = await readBackup(client, device, schema, {
      signal,
      onProgress: (progress) => setRun({ kind: 'backup', progress }),
    });
    const fileName = backupFileName(device, new Date());
    setBackup({ doc, fileName });
    setAfter({ kind: 'none' });
    await downloadBlob(backupText(doc), fileName, { mimeType: 'application/json' });
  };

  /**
   * After an upload: if a preset's stored edits are from another presets revision, ask whether to
   * keep them (§4.5 "After a firmware upload").
   *
   * @param info - HELLO from the new firmware.
   */
  const checkPresets = async (info: HelloInfo): Promise<void> => {
    const client = link?.current() ?? null;
    if (client === null || offersPresets(info) === false) {
      return;
    }
    const stale = stalePresets(await readPresets(client));
    if (stale.length > 0) {
      setAfter({ kind: 'stale', slots: stale });
    }
  };

  /**
   * Runs a settings action after the upload, with the settings views held off meanwhile.
   *
   * @param what - Which one, for the button's spinner.
   * @param work - The requests.
   */
  const afterAction = (what: 'reset' | 'restore', work: (client: SysexClient) => Promise<AfterState>) => {
    const client = link?.current() ?? null;
    if (client === null) {
      return;
    }
    setAfter((prev) => ({ kind: 'working', what, slots: prev.kind === 'stale' ? prev.slots : [] }));
    onRunningChange(true);
    work(client)
      .then(setAfter, (error: unknown) => setAfter({ kind: 'failed', message: messageOf(error) }))
      .finally(() => {
        onRunningChange(false);
        onFinished();
      });
  };
  const applyFirmwarePresets = () =>
    afterAction('reset', async (client) => {
      await resetPreset(client, PRESET_ALL);
      return { kind: 'reset' };
    });
  const restore = () => {
    const doc = backup?.doc;
    const info = run.kind === 'done' ? run.info : device;
    if (doc === undefined || info === null) {
      return;
    }
    afterAction('restore', async (client) => {
      const report = await restoreBackup(client, info, await getSchema(client), doc);
      return { kind: 'restored', report };
    });
  };

  const layouts = layout.board === null ? [] : [layout.board];
  const gfwResult = picked?.isGfw === true ? validateImage(picked.bytes, device?.boardId, layouts) : null;
  // A stock M-Audio update file (hex text) goes to the Code 61's bootloader as it is.
  const stockResult = picked !== null && picked.isGfw === false ? parseStockUpdateFile(picked.bytes) : null;
  const isVendorGfw = gfwResult?.ok === true && usesVendorUpdate(gfwResult.image.board, device);
  const isVendor = stockResult !== null || isVendorGfw;
  const abResult =
    picked !== null && picked.isGfw === false && stockResult === null
      ? prepareAbImage({ bytes: picked.bytes, boardId: device?.boardId ?? null, versionText: version })
      : null;
  const mismatch: string[] = [];
  if (device !== null && picked !== null) {
    if (picked.isGfw && isAb) {
      mismatch.push(
        'This device takes updates in its own app (A/B slots), not through a gemifi bootloader. Choose its .bin instead.',
      );
    }
    if (picked.isGfw === false && stockResult === null && isAb === false) {
      mismatch.push('This device updates through the gemifi bootloader, which needs a .gfw image.');
    }
    const isOtherBoard = device.boardId !== CODE61_BOARD_ID;
    if (stockResult !== null && isOtherBoard) {
      mismatch.push(`This is an M-Audio Code update file, but the device is ${boardLabel(device.boardId)}.`);
    }
  }
  // A warning, not a block: the board id already guards the upload, and a renamed device is fine.
  const deviceNode = device?.nodeName ?? null;
  const imageNode = picked?.nodeName ?? null;
  const isOtherNode = deviceNode !== null && imageNode !== null && deviceNode !== imageNode;
  if (isVendor && probePorts === null) {
    mismatch.push('The M-Audio bootloader is reached over USB MIDI. Switch the transport to USB MIDI.');
  }

  const startVendor = (ports: () => readonly ProbePort[]) => {
    if (picked === null) {
      return;
    }
    const image = picked;
    let records: Uint8Array[];
    if (stockResult?.ok === true) {
      records = stockResult.records;
    } else if (gfwResult?.ok === true) {
      records = buildUpdateStream(gfwResult.image.bytes);
    } else {
      return;
    }
    const abort = new AbortController();
    controller.current = abort;
    setVendorLog([]);
    const client = link?.current() ?? null;
    // A gemifi app on the Code 61 sets M-Audio's update request on REBOOT(1).
    const isGemifiApp = client !== null && device?.role === Role.App && device.boardId === CODE61_BOARD_ID;
    onRunningChange(true);
    let isBackingUp = true;
    takeBackup(abort.signal)
      .then(() => {
        isBackingUp = false;
        return vendorUpdate({
          records,
          ports,
          signal: abort.signal,
          onProgress: (progress) => setRun({ kind: 'vendor', progress }),
          onLog: (line) => setVendorLog((lines) => [...lines, line]),
          beforeVanish: () => onBeforeVanish(image),
          stopForPowerCycle: isWindows,
          ...(isGemifiApp
            ? {
                request: async () => {
                  await reboot(client, RebootMode.Boot, { signal: abort.signal });
                },
              }
            : {}),
        });
      })
      .then(
        (outcome) => {
          if (outcome.kind === 'done') {
            onUploaded();
            setRun({ kind: 'vendor-done', portName: outcome.portName });
          } else {
            setRun({ kind: 'vendor-restart' });
          }
        },
        (error: unknown) => {
          setRun(
            isBackingUp
              ? backupFailed(abort.signal.aborted, error)
              : {
                  kind: 'failed',
                  title: abort.signal.aborted ? 'Upload cancelled' : 'Upload through the M-Audio bootloader failed',
                  message: messageOf(error),
                },
          );
        },
      )
      .finally(() => {
        onRunningChange(false);
        onFinished();
      });
  };

  const start = () => {
    if (isVendor) {
      if (probePorts !== null) {
        startVendor(probePorts);
      }
      return;
    }
    if (link === null || picked === null) {
      return;
    }
    const abort = new AbortController();
    controller.current = abort;
    const onProgress = (progress: FlashProgress) => setRun({ kind: 'running', progress });
    let begin: () => Promise<HelloInfo>;
    if (gfwResult?.ok === true) {
      const image = gfwResult.image;
      begin = () => flashFirmware({ image, link, signal: abort.signal, onProgress });
    } else if (abResult?.ok === true) {
      const image = abResult.image;
      begin = () => flashAbFirmware({ image, link, signal: abort.signal, onProgress });
    } else {
      return;
    }
    onRunningChange(true);
    let isBackingUp = true;
    takeBackup(abort.signal)
      .then(() => {
        isBackingUp = false;
        return begin();
      })
      .then(
        async (info) => {
          setRun({ kind: 'done', info });
          await checkPresets(info).catch((error: unknown) =>
            setAfter({
              kind: 'failed',
              message: `The presets could not be read after the upload: ${messageOf(error)}`,
            }),
          );
        },
        (error: unknown) => {
          if (isBackingUp) {
            setRun(backupFailed(abort.signal.aborted, error));
            return;
          }
          const phase = error instanceof FlashError ? FLASH_PHASE_LABELS[error.phase].toLowerCase() : null;
          setRun({
            kind: 'failed',
            title: abort.signal.aborted ? 'Upload cancelled' : `Upload failed while ${phase ?? 'flashing'}`,
            message: messageOf(error),
          });
        },
      )
      .finally(() => {
        onRunningChange(false);
        onFinished();
      });
  };

  const progress =
    run.kind === 'running'
      ? { ...run.progress, label: FLASH_PHASE_LABELS[run.progress.phase] }
      : run.kind === 'vendor'
        ? { ...run.progress, label: VENDOR_PHASE_LABELS[run.progress.phase] }
        : null;
  const vendorPhase = run.kind === 'vendor' ? run.progress.phase : null;
  const isAwaitingPowerCycle =
    vendorPhase === VendorPhase.WaitingForBootloader || vendorPhase === VendorPhase.PowerCycle;
  const isRunIdle = isRunning === false;
  // A stored upload: ready once the bootloader is here; until then, what to do (not after the Windows steps).
  const isResumeReady = isRunIdle && resume?.isBootloaderPresent === true && picked === resume.image;
  const isResumeWaiting = isRunIdle && run.kind !== 'vendor-restart' && resume?.isBootloaderPresent === false;
  const dismissResume = <Button size="sm" variant="outline" content="Dismiss" onClick={onDismissResume} />;
  const isChecked = gfwResult?.ok === true || abResult?.ok === true || stockResult?.ok === true;
  const isReachable = isVendor ? probePorts !== null : link !== null && device !== null;
  const isFlashable = isChecked && mismatch.length === 0 && isReachable;
  const confirmText = isVendor
    ? "The keyboard's app is replaced through M-Audio's own bootloader, which stays as it is. The page asks you to turn the keyboard off and on, then finds the bootloader by itself. If the upload stops part way, the keyboard should come up in update mode again: run this again."
    : picked?.isGfw === false
      ? 'The image goes to the slot the device is not running from. The old firmware keeps working until the new one has been checked, and the device then restarts into it.'
      : 'The firmware on the device is erased first. If the upload stops part way, the device stays in the bootloader until an image is flashed.';
  // The drop zone until an image is chosen; after that a small button, so the image's facts lead.
  const pickOptions = {
    accept: '.gfw,.bin,.txt,.json',
    read: 'bytes',
    multiple: true,
    onPickMany: (files: PickedFile[]) => {
      // A .json is a DIY board's layout; anything else is the image.
      const layoutFile = files.find((file) => file.name.toLowerCase().endsWith('.json'));
      const imageFile = files.find((file) => file !== layoutFile);
      if (layoutFile?.bytes) {
        const read = boardFromLayout(new TextDecoder().decode(layoutFile.bytes));
        setLayout(
          typeof read === 'string'
            ? { board: null, problem: `${layoutFile.name}: ${read}` }
            : { board: read, problem: null },
        );
      }
      if (imageFile?.bytes) {
        onImageChange(pickedImageOf(imageFile.name, imageFile.bytes));
      }
    },
  } as const;
  return (
    <CardLayout
      title="Firmware"
      iconSlot={<Upload />}
      description="A .gfw for the Keystation's bootloader or the Code 61 (through M-Audio's bootloader), an M-Audio update file to restore the Code 61's stock firmware, or an ESP app .bin for an ESP32 or ESP8266 build that updates itself. The image is checked here first; nothing is sent until it passes."
      contentSlot={
        <>
          {isResumeReady ? (
            <Alert
              variant="info"
              title="Finish the update"
              description={`The keyboard is in M-Audio's update mode. Upload ${resume.image.name} to finish.`}
              actionSlot={dismissResume}
            />
          ) : null}
          {isResumeWaiting ? (
            <Alert
              variant="info"
              title={`${resume.image.name} is waiting to be uploaded`}
              description={
                resume.isMidiReady
                  ? "Switch the keyboard on; it should show 'update'."
                  : "Connect USB MIDI above, then switch the keyboard on; it should show 'update'."
              }
              actionSlot={dismissResume}
            />
          ) : null}
          {picked === null ? (
            <FilePicker
              label="Choose a .gfw, .bin or M-Audio update file"
              hint="Drop a .gfw (with its .board.json for a DIY board), an ESP app .bin or an M-Audio FirmwareFile here, or click to choose one"
              {...pickOptions}
            />
          ) : (
            <FilePickerButton
              variant="outline"
              size="sm"
              label="Switch image"
              className="self-start"
              {...pickOptions}
            />
          )}
          {layout.problem === null ? null : (
            <Alert variant="warning" title="Not a board layout" description={layout.problem} />
          )}
          {isOtherNode ? (
            <Alert
              variant="warning"
              title="Built for another device"
              description={`This image was built for ${imageNode}, but the connected device is ${deviceNode}. Flashing it gives the device the other file's controls and name. Check that it is the right image.`}
            />
          ) : null}
          {picked !== null && gfwResult !== null ? (
            <GfwFacts name={picked.name} source={picked.source} result={gfwResult} extra={mismatch} />
          ) : null}
          {picked !== null && stockResult !== null ? (
            <StockFacts name={picked.name} result={stockResult} extra={mismatch} />
          ) : null}
          {picked !== null && abResult !== null ? (
            <AbFacts
              name={picked.name}
              source={picked.source}
              result={abResult}
              extra={mismatch}
              version={version}
              onVersionChange={setVersion}
              device={device}
              isRunning={isRunning}
            />
          ) : null}
          {canBackUp && picked !== null ? (
            <SwitchField
              id="backup-before-flash"
              label="Back up settings and presets first"
              description="Saves every preset and setting to a file in your downloads before the upload, as gemifi.py flash does. Import JSON on the Presets tab, or gemifi.py restore, puts it back."
              checked={isBackupOn}
              onCheckedChange={setIsBackupOn}
              disabled={isRunning}
            />
          ) : null}
          {run.kind === 'backup' ? (
            <>
              <p className="text-sm">Backing up the settings and presets…</p>
              <Progress
                value={run.progress.done}
                max={Math.max(1, run.progress.total)}
                label="Backup"
                valueLabel={
                  run.progress.total === 0
                    ? 'Reading the settings'
                    : `${run.progress.done} of ${run.progress.total} presets read`
                }
              />
            </>
          ) : null}
          {progress === null ? null : (
            <>
              <p className="text-sm">{progress.label}…</p>
              <Progress
                value={progress.sentBytes}
                max={progress.totalBytes}
                label="Firmware upload"
                valueLabel={`${progress.sentBytes} of ${progress.totalBytes} bytes, ${Math.round((progress.sentBytes / progress.totalBytes) * PERCENT)}%`}
              />
            </>
          )}
          {run.kind === 'done' ? (
            <Alert
              variant="info"
              title="Firmware updated"
              description={`The device restarted and answers with firmware ${formatVersion(run.info.fwVersion)}.`}
            />
          ) : null}
          {isAwaitingPowerCycle && isWindows === false ? (
            <Alert
              variant="warning"
              title="Turn the keyboard off and on"
              description="The Code 61 enters M-Audio's update mode at power-on. Switch it off, then on again; the page asks for the bootloader every 2 s and carries on by itself. Nothing to do if it is in update mode already."
            />
          ) : null}
          {isAwaitingPowerCycle && isWindows ? (
            <Alert
              variant="warning"
              title="If the browser stops responding"
              description="Chrome on Windows can hang while the keyboard restarts. If it does, close Chrome completely, reopen this page, and it offers to finish the upload."
            />
          ) : null}
          {run.kind === 'vendor-restart' ? (
            <Alert
              variant="warning"
              title="Close Chrome, then turn the keyboard off and on"
              description={windowsRestartSteps}
            />
          ) : null}
          {run.kind === 'vendor-done' ? (
            <Alert
              variant="info"
              title="Firmware sent"
              description={`M-Audio's bootloader on ${run.portName} took every record. If the keyboard does not restart into the new firmware by itself, turn it off and on.`}
            />
          ) : null}
          {vendorLog.length === 0 ? null : (
            <CodeBlock title="M-Audio bootloader" content={vendorLog.join('\n')} wrap maxHeight="sm" follow />
          )}
          {run.kind === 'failed' ? <Alert variant="destructive" title={run.title} description={run.message} /> : null}
          {isRunning ? null : (
            <AfterUpload
              after={after}
              backup={backup}
              canRestore={canBackUp && link?.current() != null}
              onKeep={() => setAfter({ kind: 'kept' })}
              onUseFirmware={applyFirmwarePresets}
              onRestore={restore}
            />
          )}
        </>
      }
      footerActionsSlot={
        isRunning ? (
          <Button variant="outline" content="Cancel" onClick={() => controller.current?.abort()} />
        ) : (
          <ConfirmButton
            variant="positive"
            content="Flash"
            disabled={isFlashable === false}
            hint={isFlashable ? undefined : 'Connect a device and choose an image that passes the checks'}
            title="Flash this firmware?"
            description={confirmText}
            confirmLabel="Flash"
            onConfirm={start}
          />
        )
      }
    />
  );
}
