import { lazy, Suspense, useLayoutEffect, useRef, useState } from 'react';
import { PageLayout } from '@/components/page-layout';
import { Alert } from '@/components/ui/alert';
import { Badge } from '@/components/ui/badge';
import { SegmentedButton, SegmentedGroup } from '@/components/ui/segmented';
import { fetchBuilderInfo } from './builder/api.ts';
import { BuilderView } from './builder/builder-view.tsx';
import { useRequest } from './builder/use-request.ts';
import { ConfigCard } from './device/config-card.tsx';
import { ConnectionCard, type TransportMode } from './device/connection-card.tsx';
import { DeviceCard } from './device/device-card.tsx';
import { FlashCard } from './device/flash-card.tsx';
import { LogCard } from './device/log-card.tsx';
import type { PickedImage } from './device/picked-image.ts';
import { offersConfig } from './device/use-config.ts';
import { useConnection } from './device/use-connection.ts';
import { useDeviceLog } from './device/use-device-log.ts';
import { useHello } from './device/use-hello.ts';
import { useSerial } from './device/use-serial.ts';
import { useSearchParam } from './navigation/use-search-param.ts';
import type { DeviceLink } from './protocol/flash.ts';
import type { PickedBytes } from './serial/esp-manifest.ts';

/** The first-install card, loaded on its own: only ESP users need it. */
const InstallCard = lazy(() => import('./device/install-card.tsx').then((m) => ({ default: m.InstallCard })));

/** The page's two views, when a builder serves it. */
const AppView = { Devices: 'devices', Update: 'update' } as const;
type AppView = (typeof AppView)[keyof typeof AppView];

/**
 * @param param - The `view` search param.
 * @returns The view it names; the device list unless it says Update.
 */
function viewOf(param: string | null): AppView {
  return param === AppView.Update ? AppView.Update : AppView.Devices;
}

/** Shown when the page is not a secure context, where the browser hides MIDI and serial ports. */
const insecureAlert = (
  <Alert
    variant="warning"
    title="Web MIDI and Web Serial need HTTPS or localhost"
    description="This page was opened over plain HTTP, so the browser offers no MIDI devices or serial ports and nothing can be flashed from here. Open it over HTTPS, or as http://localhost (an SSH tunnel such as ssh -L 6055:localhost:6055 <host> does it). Editing and building still work, and the images can be downloaded."
  />
);

/**
 * The page: connection (USB MIDI or serial), device facts, firmware upload, the first install on
 * ESP chips, config and the log. Served by a gemifi builder, it also has the Devices view, where
 * device files are edited and built and their images handed to the Update view.
 *
 * @returns The page.
 */
export function App() {
  const builder = useRequest(fetchBuilderInfo, 'info');
  const builderInfo = builder.data ?? null;
  const [viewParam, setViewParam] = useSearchParam('view');
  const view = viewOf(viewParam);
  const [flashImage, setFlashImage] = useState<PickedImage | null>(null);
  const [installFiles, setInstallFiles] = useState<PickedBytes[]>([]);
  const body = useRef<HTMLDivElement | null>(null);
  // The views share one scrolling body: each opens at its top rather than where the other was.
  // biome-ignore lint/correctness/useExhaustiveDependencies: `view` is the trigger.
  useLayoutEffect(() => {
    body.current?.scrollTo({ top: 0 });
  }, [view]);
  const [mode, setMode] = useState<TransportMode>('midi');
  const midi = useConnection();
  const serial = useSerial();
  const isMidi = mode === 'midi';
  const client = isMidi ? midi.snapshot.client : serial.snapshot.client;
  const { state: hello, refresh } = useHello(client);
  const log = useDeviceLog(client);
  const [isFlashing, setIsFlashing] = useState(false);
  const [isInstalling, setIsInstalling] = useState(false);
  const manager = midi.access.kind === 'ready' ? midi.access.manager : null;
  const link: DeviceLink | null = isMidi ? manager : serial.snapshot.port === null ? null : serial.link;
  const isConnected = isMidi ? midi.snapshot.selected !== null : serial.snapshot.port !== null;
  const info = hello.kind === 'ok' ? hello.info : null;
  const isBusy = isFlashing || isInstalling;
  const canInstall = serial.access.kind !== 'unavailable';
  const isSecure = window.isSecureContext;
  const showView = (next: AppView) => setViewParam(next === AppView.Devices ? null : next);

  const updateCards = (
    <>
      <ConnectionCard
        mode={mode}
        onModeChange={setMode}
        access={midi.access}
        snapshot={midi.snapshot}
        onConnect={midi.connect}
        onSelect={(id) => manager?.select(id)}
        serialAccess={serial.access}
        serialSnapshot={serial.snapshot}
        onSerialConnect={serial.connect}
        onSerialDisconnect={serial.disconnect}
        isBusy={isBusy}
      />
      <div className="grid min-w-0 gap-4 lg:grid-cols-2">
        <DeviceCard client={client} hello={hello} onRefresh={refresh} isBusy={isBusy} />
        <FlashCard
          link={link}
          image={flashImage}
          onImageChange={setFlashImage}
          device={info}
          onRunningChange={setIsFlashing}
          onFinished={refresh}
        />
      </div>
      {client !== null && info !== null && offersConfig(info) && isBusy === false ? (
        <ConfigCard client={client} device={info} />
      ) : null}
      <LogCard lines={log.lines} rawLines={log.rawLines} onClear={log.clear} />
      {canInstall ? (
        <Suspense fallback={null}>
          <InstallCard
            serialLink={serial.link}
            files={installFiles}
            onFilesChange={setInstallFiles}
            isBusy={isFlashing}
            onRunningChange={setIsInstalling}
          />
        </Suspense>
      ) : null}
    </>
  );

  if (builderInfo === null) {
    return (
      <PageLayout
        title="gemifi updater"
        description="Device info, config, debug log and firmware upload, over Web MIDI or Web Serial."
        actionSlot={
          <Badge variant={isConnected ? 'positive-soft' : 'secondary'}>
            {isConnected ? 'Connected' : 'Not connected'}
          </Badge>
        }
        contentSlot={
          <>
            {isSecure ? null : insecureAlert}
            {updateCards}
          </>
        }
      />
    );
  }

  // Both views stay mounted, so an upload keeps running and the editor keeps its edits across a switch.
  return (
    <PageLayout
      title="gemifi builder"
      contentRef={body}
      description="Define a device, build its firmware, and flash it over Web MIDI or Web Serial."
      headerContentSlot={
        <SegmentedGroup aria-label="View" value={view} onValueChange={showView}>
          <SegmentedButton value={AppView.Devices}>Devices</SegmentedButton>
          <SegmentedButton value={AppView.Update}>Update</SegmentedButton>
        </SegmentedGroup>
      }
      actionSlot={
        <Badge variant={isConnected ? 'positive-soft' : 'secondary'}>
          {isConnected ? 'Connected' : 'Not connected'}
        </Badge>
      }
      contentSlot={
        <>
          {isSecure ? null : insecureAlert}
          <div hidden={view !== AppView.Devices} className="min-w-0">
            <BuilderView
              info={builderInfo}
              isBusy={isBusy}
              canInstall={canInstall}
              onFlash={(image) => {
                setFlashImage(image);
                showView(AppView.Update);
              }}
              onInstall={(files) => {
                setInstallFiles(files);
                showView(AppView.Update);
              }}
            />
          </div>
          <div hidden={view !== AppView.Update} className="flex min-w-0 flex-col gap-4">
            {updateCards}
          </div>
        </>
      }
    />
  );
}
