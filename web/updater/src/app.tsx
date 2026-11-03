import { lazy, Suspense, useEffect, useLayoutEffect, useRef, useState } from 'react';
import { CardLayout } from '@/components/card-layout';
import { EmptyState } from '@/components/empty-state';
import { PageLayout } from '@/components/page-layout';
import { Alert } from '@/components/ui/alert';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { Plug, Settings } from '@/components/ui/icons';
import { SegmentedButton, SegmentedGroup } from '@/components/ui/segmented';
import { type DeviceListing, fetchBuilderInfo, listDevices } from './builder/api.ts';
import { BuilderView, type OpenRequest } from './builder/builder-view.tsx';
import { devicesWithNodeName } from './builder/node-match.ts';
import { useRequest } from './builder/use-request.ts';
import { ConfigCard } from './device/config-card.tsx';
import { ConnectionCard, type TransportMode } from './device/connection-card.tsx';
import { DeviceCard, type DeviceFiles } from './device/device-card.tsx';
import { FeedbackCard } from './device/feedback-card.tsx';
import { FlashCard, type ResumeOffer } from './device/flash-card.tsx';
import { LogCard } from './device/log-card.tsx';
import type { PickedImage } from './device/picked-image.ts';
import { isWindows } from './device/platform.ts';
import { offersConfig } from './device/use-config.ts';
import { useConnection } from './device/use-connection.ts';
import { useDeviceLog } from './device/use-device-log.ts';
import { offersFeedback } from './device/use-feedback.ts';
import { useHello } from './device/use-hello.ts';
import { usePendingUpload } from './device/use-pending-upload.ts';
import { useSerial } from './device/use-serial.ts';
import { useSearchParam } from './navigation/use-search-param.ts';
import type { DeviceLink } from './protocol/flash.ts';
import type { PickedBytes } from './serial/esp-manifest.ts';
import { isMaudioBootloaderName } from './vendor/maudio.ts';

/** The first-install card, loaded on its own: only ESP users need it. */
const InstallCard = lazy(() => import('./device/install-card.tsx').then((m) => ({ default: m.InstallCard })));

/** The site's changelog: the builder's version links to it. */
const CHANGELOG_URL = 'https://cubicecho.github.io/gemifi/changelog/';

/** Without a builder there is no device list to ask for. */
const noDevices = async (): Promise<DeviceListing> => ({ devices: [], userDir: '' });

/** Read once: Windows changes how a Code 61 is put into update mode (the Firmware card). */
const isOnWindows = isWindows();

/** The non-affiliation notice (issue #2), also in the README and the site footer. */
const disclaimer = (
  <p className="max-w-3xl text-foreground/60 text-xs">
    {
      'gemifi is an independent project, not affiliated with or endorsed by M-Audio/inMusic, Behringer, or any other hardware maker. Product names are trademarks of their owners and are used only to identify compatible hardware. Flashing gemifi replaces the stock firmware and may void your warranty.'
    }
  </p>
);

/** The page's views: Devices only when a builder serves it; Update and Config always. */
const AppView = { Devices: 'devices', Update: 'update', Config: 'config' } as const;
type AppView = (typeof AppView)[keyof typeof AppView];

/**
 * @param param - The `view` search param.
 * @param hasBuilder - Whether a builder serves the page (only then is there a Devices view).
 * @returns The view it names; with no param, Devices under a builder and Update otherwise.
 */
function viewOf(param: string | null, hasBuilder: boolean): AppView {
  if (param === AppView.Update || param === AppView.Config) {
    return param;
  }
  return hasBuilder ? AppView.Devices : AppView.Update;
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
 * The page. Update: connection (USB MIDI or serial), device facts, firmware upload, the first install
 * on ESP chips and the log. Config: the connected device's settings and host feedback tests. Served by
 * a gemifi builder, it also has the Devices view, where device files are edited and built and their
 * images handed to the Update view.
 *
 * @returns The page.
 */
export function App() {
  const builder = useRequest(fetchBuilderInfo, 'info');
  const builderInfo = builder.data ?? null;
  const hasBuilder = builderInfo !== null;
  const devices = useRequest(hasBuilder ? listDevices : noDevices, hasBuilder ? 'devices' : 'none');
  const [openRequest, setOpenRequest] = useState<OpenRequest | null>(null);
  const [viewParam, setViewParam] = useSearchParam('view');
  const view = viewOf(viewParam, hasBuilder);
  const [flashImage, setFlashImage] = useState<PickedImage | null>(null);
  const pendingUpload = usePendingUpload();
  const { pending } = pendingUpload;
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
  const defaultView = hasBuilder ? AppView.Devices : AppView.Update;
  const showView = (next: AppView) => setViewParam(next === defaultView ? null : next);
  const nodeName = info?.nodeName ?? null;
  // An upload stored before the Code 61 went into update mode: offered once its bootloader is here.
  const bootloaderPair = midi.snapshot.pairs.find((pair) => isMaudioBootloaderName(pair.name)) ?? null;
  const resume: ResumeOffer | null =
    pending === null
      ? null
      : { image: pending.image, isBootloaderPresent: bootloaderPair !== null, isMidiReady: manager !== null };
  if (resume?.isBootloaderPresent === true && flashImage === null) {
    setFlashImage(resume.image);
  }
  const bootloaderId = bootloaderPair?.id ?? null;
  const isNothingSelected = midi.snapshot.selected === null;
  const hasPending = pending !== null;
  // The vendor flow probes the selected port: select the bootloader when nothing else is.
  useEffect(() => {
    const isIdle = isFlashing === false && isInstalling === false;
    if (hasPending && isIdle && isMidi && isNothingSelected && bootloaderId !== null) {
      manager?.select(bootloaderId);
    }
  }, [hasPending, isFlashing, isInstalling, isMidi, isNothingSelected, bootloaderId, manager]);
  /** A new image replaces the stored one; so does none. */
  const changeFlashImage = (image: PickedImage | null) => {
    setFlashImage(image);
    if (pending !== null && image !== pending.image) {
      pendingUpload.clear();
    }
  };
  const dismissResume = () => {
    pendingUpload.clear();
    if (flashImage !== null && flashImage === pending?.image) {
      setFlashImage(null);
    }
  };
  const deviceFiles: DeviceFiles | null = hasBuilder
    ? {
        matches: devicesWithNodeName(devices.data?.devices ?? [], nodeName),
        onOpen: (device) => {
          showView(AppView.Devices);
          setOpenRequest((last) => ({ id: device.id, nonce: (last?.nonce ?? 0) + 1 }));
        },
      }
    : null;

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
        <DeviceCard client={client} hello={hello} onRefresh={refresh} isBusy={isBusy} deviceFiles={deviceFiles} />
        <FlashCard
          link={link}
          image={flashImage}
          onImageChange={changeFlashImage}
          device={info}
          probePorts={isMidi && manager !== null ? manager.probePorts : null}
          onRunningChange={setIsFlashing}
          onFinished={refresh}
          resume={resume}
          onDismissResume={dismissResume}
          onBeforeVanish={pendingUpload.save}
          onUploaded={pendingUpload.clear}
          isWindows={isOnWindows}
        />
      </div>
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

  const hasConfig = client !== null && info !== null && offersConfig(info);
  const hasFeedback = client !== null && offersFeedback(info);
  let configEmpty: { title: string; description: string } | null = null;
  if (client === null || info === null) {
    configEmpty = {
      title: 'No device connected',
      description: 'Connect a device on the Update tab. Its settings and the host feedback tests show up here.',
    };
  } else if (isBusy) {
    configEmpty = {
      title: 'Firmware upload in progress',
      description: 'The settings come back once the upload has finished and the device has restarted.',
    };
  } else if (hasConfig === false && hasFeedback === false) {
    configEmpty = {
      title: 'Nothing to configure',
      description: 'This firmware has no settings and no host feedback (a bootloader or a test app).',
    };
  }
  const configCards =
    configEmpty === null ? (
      <>
        {hasConfig && client !== null && info !== null ? <ConfigCard client={client} device={info} /> : null}
        {hasFeedback && client !== null ? <FeedbackCard client={client} /> : null}
      </>
    ) : (
      <CardLayout
        contentSlot={
          <EmptyState
            iconSlot={client === null ? <Plug /> : <Settings />}
            title={configEmpty.title}
            description={configEmpty.description}
          />
        }
      />
    );

  const viewSwitch = (
    <SegmentedGroup aria-label="View" value={view} onValueChange={showView}>
      {hasBuilder ? <SegmentedButton value={AppView.Devices}>Devices</SegmentedButton> : null}
      <SegmentedButton value={AppView.Update}>Update</SegmentedButton>
      <SegmentedButton value={AppView.Config}>Config</SegmentedButton>
    </SegmentedGroup>
  );
  // Update and Config stay mounted, so an upload keeps running and the log keeps filling across a switch.
  const deviceViews = (
    <>
      <div hidden={view !== AppView.Update} className="flex min-w-0 flex-col gap-4">
        {updateCards}
      </div>
      <div hidden={view !== AppView.Config} className="flex min-w-0 flex-col gap-4">
        {configCards}
      </div>
    </>
  );

  if (builderInfo === null) {
    return (
      <PageLayout
        title="gemifi updater"
        contentRef={body}
        description="Device info, config, debug log and firmware upload, over Web MIDI or Web Serial."
        headerContentSlot={viewSwitch}
        actionSlot={
          <Badge variant={isConnected ? 'positive-soft' : 'secondary'}>
            {isConnected ? 'Connected' : 'Not connected'}
          </Badge>
        }
        contentSlot={
          <>
            {isSecure ? null : insecureAlert}
            {deviceViews}
            {disclaimer}
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
      headerContentSlot={viewSwitch}
      actionSlot={
        <>
          {builderInfo.version === '' ? null : (
            <Button
              variant="outline"
              size="sm"
              linkSlot={<a href={CHANGELOG_URL} target="_blank" rel="noopener" />}
              content={`v${builderInfo.version}`}
              hint="What's new in this version"
            />
          )}
          <Badge variant={isConnected ? 'positive-soft' : 'secondary'}>
            {isConnected ? 'Connected' : 'Not connected'}
          </Badge>
        </>
      }
      contentSlot={
        <>
          {isSecure ? null : insecureAlert}
          <div hidden={view !== AppView.Devices} className="min-w-0">
            <BuilderView
              info={builderInfo}
              devices={devices}
              connectedNodeName={nodeName}
              openRequest={openRequest}
              isBusy={isBusy}
              canInstall={canInstall}
              onFlash={(image) => {
                changeFlashImage(image);
                showView(AppView.Update);
              }}
              onInstall={(files) => {
                setInstallFiles(files);
                showView(AppView.Update);
              }}
            />
          </div>
          {deviceViews}
          {disclaimer}
        </>
      }
    />
  );
}
