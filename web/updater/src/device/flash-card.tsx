import { useRef, useState } from 'react';
import { CardLayout } from '@/components/card-layout';
import { ConfirmButton } from '@/components/confirm-button';
import { DescriptionList, PropertyRow } from '@/components/description-list';
import { Alert } from '@/components/ui/alert';
import { Button } from '@/components/ui/button';
import { FilePicker } from '@/components/ui/file-picker';
import { Upload } from '@/components/ui/icons';
import { Progress } from '@/components/ui/progress';
import { boardLabel } from '../protocol/boards.ts';
import {
  type DeviceLink,
  FLASH_PHASE_LABELS,
  FlashError,
  type FlashProgress,
  flashFirmware,
} from '../protocol/flash.ts';
import { hex, type ImageResult, validateImage } from '../protocol/image.ts';
import { formatVersion, type HelloInfo } from '../protocol/messages.ts';

/** Percent, for the progress bar's words. */
const PERCENT = 100;

/** Where an upload is. */
type RunState =
  | { kind: 'idle' }
  | { kind: 'running'; progress: FlashProgress }
  | { kind: 'done'; info: HelloInfo }
  | { kind: 'failed'; title: string; message: string };

/** A picked file and what checking it found. */
interface PickedImage {
  name: string;
  bytes: Uint8Array;
}

/** Props of {@link FlashCard}. */
export interface FlashCardProps {
  link: DeviceLink | null;
  /** The connected device's HELLO, to check the image's board against. */
  device: HelloInfo | null;
  onRunningChange: (isRunning: boolean) => void;
}

/**
 * The checked image's header as rows.
 *
 * @param props - The file name and the check result.
 * @returns The list, or the problems.
 */
function ImageFacts({ name, result }: { name: string; result: ImageResult }) {
  const header = result.ok ? result.image.header : result.header;
  return (
    <>
      {header === null ? null : (
        <DescriptionList
          contentSlot={[
            <PropertyRow key="file" label="File" value={name} valueClassName="font-mono break-all" />,
            <PropertyRow key="board" label="Board" value={boardLabel(header.boardId)} />,
            <PropertyRow key="fw" label="Version" value={formatVersion(header.fwVersion)} valueClassName="font-mono" />,
            <PropertyRow key="size" label="Size" value={`${header.imageSize} bytes`} />,
            <PropertyRow key="crc" label="CRC-32" value={hex(header.crc32)} valueClassName="font-mono" />,
          ]}
        />
      )}
      {result.ok ? null : (
        <Alert
          variant="destructive"
          title="This image cannot be flashed"
          description={
            <ul className="list-disc pl-4">
              {result.problems.map((problem) => (
                <li key={problem}>{problem}</li>
              ))}
            </ul>
          }
        />
      )}
    </>
  );
}

/**
 * Picks a `.gfw`, checks it before anything is sent, and uploads it.
 *
 * @param props - The device link, the device's HELLO and a busy callback.
 * @returns The card.
 */
export function FlashCard({ link, device, onRunningChange }: FlashCardProps) {
  const [picked, setPicked] = useState<PickedImage | null>(null);
  const [run, setRun] = useState<RunState>({ kind: 'idle' });
  const controller = useRef<AbortController | null>(null);
  const result = picked === null ? null : validateImage(picked.bytes, device?.boardId);
  const isRunning = run.kind === 'running';

  const start = () => {
    if (result?.ok !== true || link === null) {
      return;
    }
    const abort = new AbortController();
    controller.current = abort;
    onRunningChange(true);
    setRun({
      kind: 'running',
      progress: { phase: 'checking', sentBytes: 0, totalBytes: result.image.header.imageSize },
    });
    flashFirmware({
      image: result.image,
      link,
      signal: abort.signal,
      onProgress: (progress) => setRun({ kind: 'running', progress }),
    })
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
      .finally(() => onRunningChange(false));
  };

  const progress = run.kind === 'running' ? run.progress : null;
  const isFlashable = result?.ok === true && link !== null && device !== null;
  return (
    <CardLayout
      title="Firmware"
      iconSlot={<Upload />}
      description="The image is checked here first: header, board and CRC. Nothing is sent until it passes."
      contentSlot={
        <>
          <FilePicker
            label="Choose a .gfw image"
            hint="Drop a .gfw file here, or click to choose one"
            accept=".gfw"
            read="bytes"
            onPickMany={([file]) => {
              if (file?.bytes) {
                setPicked({ name: file.name, bytes: file.bytes });
                setRun({ kind: 'idle' });
              }
            }}
          />
          {picked === null || result === null ? null : <ImageFacts name={picked.name} result={result} />}
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
            description="The firmware on the device is erased first. If the upload stops part way, the device stays in the bootloader until an image is flashed."
            confirmLabel="Flash"
            onConfirm={start}
          />
        )
      }
    />
  );
}
