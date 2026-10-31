import { useState } from 'react';
import { CardLayout } from '@/components/card-layout';
import { EmptyState } from '@/components/empty-state';
import { List } from '@/components/list';
import { ListItem } from '@/components/list-item';
import { Alert } from '@/components/ui/alert';
import { Button } from '@/components/ui/button';
import { DownloadButton } from '@/components/ui/download-button';
import { Download, Upload } from '@/components/ui/icons';
import type { PickedImage } from '../device/picked-image.ts';
import { pickedImageOf } from '../device/picked-image.ts';
import type { PickedBytes } from '../serial/esp-manifest.ts';
import { type BuilderDevice, fetchImage, imageUrl } from './api.ts';
import { IMAGE_USES, imageKind } from './images.ts';
import { deviceTitle } from './platforms.ts';

/** Which card an image is being handed to. */
type HandOff = 'flash' | 'install';

/** Props of {@link ImagesCard}. */
export interface ImagesCardProps {
  device: BuilderDevice;
  /** An upload or install is running, so nothing new is handed over. */
  isBusy: boolean;
  /** The first-install card is on the page: this browser has Web Serial. */
  canInstall: boolean;
  /** Hands an image to the Firmware card. */
  onFlash: (image: PickedImage) => void;
  /** Hands an image to the first-install card. */
  onInstall: (files: PickedBytes[]) => void;
}

/** What each hand-off failing says. */
const HAND_OFF_FAILURES = Object.freeze({
  flash: 'The image could not be sent to the Firmware card',
  install: 'The image could not be sent to the first-install card',
}) satisfies Readonly<Record<HandOff, string>>;

/** The last build's files: each downloads, and the ones a card takes are handed to it. */
export function ImagesCard({ device, isBusy, canInstall, onFlash, onInstall }: ImagesCardProps) {
  const [fetching, setFetching] = useState<string | null>(null);
  const [failure, setFailure] = useState<{ handOff: HandOff; message: string } | null>(null);
  const title = deviceTitle(device);
  const platform = device.platform;
  const isStale = device.isStale;
  let handOffHint: string | undefined;
  if (isBusy) {
    handOffHint = 'Wait for the upload or install to finish';
  } else if (isStale) {
    handOffHint = 'Build again first: this image is from before your last edit';
  }
  const isHandOffBlocked = isBusy || isStale || fetching !== null;

  const handOver = (file: string, handOff: HandOff) => {
    setFetching(`${handOff}:${file}`);
    setFailure(null);
    fetchImage(device.id, file)
      .then((bytes) => {
        if (handOff === 'flash') {
          onFlash(pickedImageOf(file, bytes, `Built from ${title}`));
        } else {
          onInstall([{ name: file, bytes }]);
        }
      })
      .catch((error: unknown) =>
        setFailure({ handOff, message: error instanceof Error ? error.message : String(error) }),
      )
      .finally(() => setFetching(null));
  };

  const rows = device.images.map((file) => {
    const kind = platform === null ? null : imageKind(platform, file);
    const use = kind === null ? null : IMAGE_USES[kind];
    const shouldOfferFlash = use?.canFlash === true;
    const shouldOfferInstall = use?.canInstall === true && canInstall;
    return (
      <ListItem
        key={file}
        title={<span className="break-all font-mono text-sm">{file}</span>}
        description={
          <span className="flex flex-col gap-2">
            <span className="text-foreground/60 text-sm">{use?.description ?? 'Build output'}</span>
            {shouldOfferFlash || shouldOfferInstall ? (
              <span className="flex flex-wrap gap-2">
                {shouldOfferFlash ? (
                  <Button
                    variant="outline"
                    size="sm"
                    iconSlot={<Upload />}
                    content="Flash to connected device"
                    loading={fetching === `flash:${file}`}
                    disabled={isHandOffBlocked}
                    hint={handOffHint}
                    onClick={() => handOver(file, 'flash')}
                  />
                ) : null}
                {shouldOfferInstall ? (
                  <Button
                    variant="outline"
                    size="sm"
                    iconSlot={<Download />}
                    content="First install (serial)"
                    loading={fetching === `install:${file}`}
                    disabled={isHandOffBlocked}
                    hint={handOffHint}
                    onClick={() => handOver(file, 'install')}
                  />
                ) : null}
              </span>
            ) : null}
          </span>
        }
        actionSlot={<DownloadButton label={`Download ${file}`} filename={file} href={imageUrl(device.id, file)} />}
      />
    );
  });

  return (
    <CardLayout
      title="Built images"
      iconSlot={<Download />}
      description="From the last build. Flashing opens the Update view with the image ready to check and send."
      contentSlot={
        <>
          {rows.length > 0 && isStale ? (
            <Alert
              variant="warning"
              title="Out of date"
              description="The YAML has changed since these were built, so they don't have your edits. Press Save & build to make new ones. You can still download these."
            />
          ) : null}
          {rows.length === 0 ? (
            <EmptyState compact title="Not built yet. Press Save & build to make the images." />
          ) : (
            <List label="Built images" contentSlot={rows} />
          )}
          {failure === null ? null : (
            <Alert variant="destructive" title={HAND_OFF_FAILURES[failure.handOff]} description={failure.message} />
          )}
        </>
      }
    />
  );
}
