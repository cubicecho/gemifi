import { useRef, useState } from 'react';
import { CardLayout } from '@/components/card-layout';
import { ConfirmButton } from '@/components/confirm-button';
import { DescriptionList, PropertyRow } from '@/components/description-list';
import { FormField } from '@/components/form-field';
import { Alert } from '@/components/ui/alert';
import { Button } from '@/components/ui/button';
import { FilePicker, FilePickerButton } from '@/components/ui/file-picker';
import type { PickedFile } from '@/components/ui/file-picker-base';
import { Upload } from '@/components/ui/icons';
import { Input } from '@/components/ui/input';
import { Progress } from '@/components/ui/progress';
import { type AbImageResult, isAbDevice, prepareAbImage } from '../protocol/ab-image.ts';
import { boardLabel } from '../protocol/boards.ts';
import {
  type DeviceLink,
  FLASH_PHASE_LABELS,
  FlashError,
  type FlashProgress,
  flashAbFirmware,
  flashFirmware,
} from '../protocol/flash.ts';
import { hex, type ImageResult, validateImage } from '../protocol/image.ts';
import { formatVersion, type HelloInfo } from '../protocol/messages.ts';
import { type PickedImage, pickedImageOf } from './picked-image.ts';

/** Percent, for the progress bar's words. */
const PERCENT = 100;

/** Where an upload is. */
type RunState =
  | { kind: 'idle' }
  | { kind: 'running'; progress: FlashProgress }
  | { kind: 'done'; info: HelloInfo }
  | { kind: 'failed'; title: string; message: string };

/** Props of {@link FlashCard}. */
export interface FlashCardProps {
  link: DeviceLink | null;
  /** The image waiting to be flashed: picked here, or handed over by the builder. */
  image: PickedImage | null;
  onImageChange: (image: PickedImage | null) => void;
  /** The connected device's HELLO, to check the image's board against. */
  device: HelloInfo | null;
  onRunningChange: (isRunning: boolean) => void;
  /** Called after an upload ends, so the page asks HELLO again (a serial link keeps its client). */
  onFinished: () => void;
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
        <DescriptionList
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
      <DescriptionList
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
 * Picks an image, checks it before anything is sent, and uploads it: a `.gfw` through the gemifi
 * bootloader, or a chip's own `.bin` to an A/B device (ESP32 / ESP8266), whose app takes it itself.
 *
 * @param props - The device link, the image, the device's HELLO and the busy and finished callbacks.
 * @returns The card.
 */
export function FlashCard({ link, image: picked, onImageChange, device, onRunningChange, onFinished }: FlashCardProps) {
  const [version, setVersion] = useState('');
  const [run, setRun] = useState<RunState>({ kind: 'idle' });
  // A new image, picked here or handed over by the builder, clears the last upload's outcome.
  const [shownImage, setShownImage] = useState(picked);
  if (shownImage !== picked) {
    setShownImage(picked);
    setRun({ kind: 'idle' });
  }
  const controller = useRef<AbortController | null>(null);
  const isRunning = run.kind === 'running';
  const isAb = device !== null && isAbDevice(device);

  const gfwResult = picked?.isGfw === true ? validateImage(picked.bytes, device?.boardId) : null;
  const abResult =
    picked !== null && picked.isGfw === false
      ? prepareAbImage({ bytes: picked.bytes, boardId: device?.boardId ?? null, versionText: version })
      : null;
  const mismatch: string[] = [];
  if (device !== null && picked !== null) {
    if (picked.isGfw && isAb) {
      mismatch.push(
        'This device takes updates in its own app (A/B slots), not through a gemifi bootloader. Choose its .bin instead.',
      );
    }
    if (picked.isGfw === false && isAb === false) {
      mismatch.push('This device updates through the gemifi bootloader, which needs a .gfw image.');
    }
  }

  const start = () => {
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
    begin()
      .then(
        (info) => setRun({ kind: 'done', info }),
        (error: unknown) => {
          const phase = error instanceof FlashError ? FLASH_PHASE_LABELS[error.phase].toLowerCase() : null;
          setRun({
            kind: 'failed',
            title: abort.signal.aborted ? 'Upload cancelled' : `Upload failed while ${phase ?? 'flashing'}`,
            message: error instanceof Error ? error.message : String(error),
          });
        },
      )
      .finally(() => {
        onRunningChange(false);
        onFinished();
      });
  };

  const progress = run.kind === 'running' ? run.progress : null;
  const isChecked = gfwResult?.ok === true || abResult?.ok === true;
  const isFlashable = isChecked && mismatch.length === 0 && link !== null && device !== null;
  const confirmText =
    picked?.isGfw === false
      ? 'The image goes to the slot the device is not running from. The old firmware keeps working until the new one has been checked, and the device then restarts into it.'
      : 'The firmware on the device is erased first. If the upload stops part way, the device stays in the bootloader until an image is flashed.';
  // The drop zone until an image is chosen; after that a small button, so the image's facts lead.
  const pickOptions = {
    accept: '.gfw,.bin',
    read: 'bytes',
    onPickMany: ([file]: PickedFile[]) => {
      if (file?.bytes) {
        onImageChange(pickedImageOf(file.name, file.bytes));
      }
    },
  } as const;
  return (
    <CardLayout
      title="Firmware"
      iconSlot={<Upload />}
      description="A .gfw for the Keystation's bootloader, or an ESP app .bin for an ESP32 or ESP8266 build that updates itself. The image is checked here first; nothing is sent until it passes."
      contentSlot={
        <>
          {picked === null ? (
            <FilePicker
              label="Choose a .gfw or .bin image"
              hint="Drop a .gfw or an ESP app .bin here, or click to choose one"
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
          {picked !== null && gfwResult !== null ? (
            <GfwFacts name={picked.name} source={picked.source} result={gfwResult} extra={mismatch} />
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
          {progress === null ? null : (
            <>
              <p className="text-sm">{FLASH_PHASE_LABELS[progress.phase]}…</p>
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
          {run.kind === 'failed' ? <Alert variant="destructive" title={run.title} description={run.message} /> : null}
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
