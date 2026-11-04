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
import { useDemoClient } from './dev/use-demo-client.ts';
import { ConnectionCard, type TransportMode } from './device/connection-card.tsx';
import { DeviceCard, type DeviceFiles } from './device/device-card.tsx';
import { FeedbackCard } from './device/feedback-card.tsx';
import { FlashCard, type ResumeOffer } from './device/flash-card.tsx';
import { LogCard } from './device/log-card.tsx';
import type { PickedImage } from './device/picked-image.ts';
import { isWindows } from './device/platform.ts';
import { hasUnsavedSettings, SettingsSaveBar, SettingsView } from './device/settings-view.tsx';
import { offersConfig, useConfig } from './device/use-config.ts';
import { useConnection } from './device/use-connection.ts';
import { useDeviceLog } from './device/use-device-log.ts';
import { offersFeedback } from './device/use-feedback.ts';
import { useHello } from './device/use-hello.ts';
import { usePendingUpload } from './device/use-pending-upload.ts';
import { usePresets } from './device/use-presets.ts';
import { useSerial } from './device/use-serial.ts';
import { useSearchParam } from './navigation/use-search-param.ts';
import type { DeviceLink } from './protocol/flash.ts';
import { offersPresets } from './protocol/presets.ts';
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

/** The page's views: Devices only when a builder serves it; the rest always. */
const AppView = { Devices: 'devices', Update: 'update', Presets: 'presets', Feedback: 'feedback' } as const;
type AppView = (typeof AppView)[keyof typeof AppView];

/**
 * @param param - The `view` search param.
 * @param hasBuilder - Whether a builder serves the page (only then is there a Devices view).
 * @returns The view it names; with no param, Devices under a builder and Update otherwise.
 */
function viewOf(param: string | null, hasBuilder: boolean): AppView {
  if (param === AppView.Update || param === AppView.Presets || param === AppView.Feedback) {
    return param;
  }
  if (param === 'config') {
    return AppView.Presets; // the old name of the view, in saved links
  }
  return hasBuilder ? AppView.Devices : AppView.Update;
}

/** What a device view says when there is nothing to show. */
interface EmptyNote {
  title: string;
  description: string;
}

/** The empty note with no device: what the views say before anything is connected. */
const NO_DEVICE: EmptyNote = { title: 'No device connected', description: 'Connect a device on the Update tab.' };

/** Shown when the page is not a secure context, where the browser hides MIDI and serial ports. */
const insecureAlert = (
  <Alert
    variant="warning"
    title="Web MIDI and Web Serial need HTTPS or localhost"
    description="This page was opened over plain HTTP, so the browser offers no MIDI devices or serial ports and nothing can be flashed from here. Open it over HTTPS, or as http://localhost (an SSH tunnel such as ssh -L 6055:localhost:6055 <host> does it). Editing and building still work, and the images can be downloaded."
  />
);

/**
 * The page. Update: connection (USB MIDI or serial), device facts, firmware upload, the log, and on
 * the serial transport the first install on ESP chips. Presets: the connected device's presets and
 * settings, with the save bar pinned under them. Host feedback: driving its LEDs and displays as a
 * DAW script does. Served by a gemifi builder, it also has the Devices view, where device files are
 * edited and built and their images handed to the Update view.
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
  const demo = useDemoClient();
  const client = demo ?? (isMidi ? midi.snapshot.client : serial.snapshot.client);
  const { state: hello, refresh } = useHello(client);
  const log = useDeviceLog(client);
  const [isFlashing, setIsFlashing] = useState(false);
  const [isInstalling, setIsInstalling] = useState(false);
  const manager = midi.access.kind === 'ready' ? midi.access.manager : null;
  const link: DeviceLink | null = isMidi ? manager : serial.snapshot.port === null ? null : serial.link;
  const isConnected = demo !== null || (isMidi ? midi.snapshot.selected !== null : serial.snapshot.port !== null);
  const info = hello.kind === 'ok' ? hello.info : null;
  const isBusy = isFlashing || isInstalling;
  const canInstall = serial.access.kind !== 'unavailable';
  const isSecure = window.isSecureContext;
  const defaultView = hasBuilder ? AppView.Devices : AppView.Update;
  const showView = (next: AppView) => setViewParam(next === defaultView ? null : next);
  const nodeName = info?.nodeName ?? null;
  const hasConfig = client !== null && info !== null && offersConfig(info);
  const hasFeedback = client !== null && offersFeedback(info);
  // The settings are read while a device with config is connected, whichever view is showing, and
  // dropped during an upload: they are read again once the device is back.
  const settingsClient = hasConfig && isBusy === false ? client : null;
  const config = useConfig(settingsClient, settingsClient === null ? null : info);
  const presets = usePresets(settingsClient, info !== null && offersPresets(info), config.refreshValues);
  const hasUnsaved = settingsClient !== null && hasUnsavedSettings(config, presets);
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
      {/* The first install is an ESP's, over serial: the USB MIDI devices never need it. */}
      {canInstall && mode === 'serial' ? (
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
    </>
  );

  /** Why a device view is empty, or null when it has something to show. */
  const emptyNote = (has: boolean, what: string, none: EmptyNote): EmptyNote | null => {
    if (client === null || info === null) {
      return {
        title: 'No device connected',
        description: `Connect a device on the Update tab. Its ${what} show up here.`,
      };
    }
    if (isBusy) {
      return {
        title: 'Firmware upload in progress',
        description: `The ${what} come back once the upload has finished and the device has restarted.`,
      };
    }
    return has ? null : none;
  };
  const emptyCard = (note: EmptyNote) => (
    <CardLayout
      contentSlot={
        <EmptyState
          iconSlot={client === null ? <Plug /> : <Settings />}
          title={note.title}
          description={note.description}
        />
      }
    />
  );
  const hasPresets = info !== null && offersPresets(info);
  const settingsLabel = info !== null && hasPresets === false ? 'Settings' : 'Presets';
  const settingsEmpty = emptyNote(hasConfig, 'presets and settings', {
    title: 'Nothing to configure',
    description: 'This firmware has no settings (a bootloader or a test app).',
  });
  const feedbackEmpty = emptyNote(hasFeedback, 'components', {
    title: 'No host feedback',
    description: 'This firmware was built without host feedback, so a DAW script cannot drive its LEDs or display.',
  });
  // `emptyNote` is null only with a device connected, so `info` and `client` are there below.
  const settingsCards =
    settingsEmpty !== null || info === null ? (
      emptyCard(settingsEmpty ?? NO_DEVICE)
    ) : (
      <SettingsView config={config} presets={presets} device={info} />
    );
  const feedbackCards =
    feedbackEmpty !== null || client === null ? (
      emptyCard(feedbackEmpty ?? NO_DEVICE)
    ) : (
      <FeedbackCard client={client} />
    );
  const saveBar =
    view === AppView.Presets && settingsEmpty === null && info !== null ? (
      <SettingsSaveBar config={config} presets={presets} device={info} />
    ) : undefined;
  const statusBadges = (
    <>
      {hasUnsaved && view !== AppView.Presets ? <Badge variant="warning-soft">Unsaved settings</Badge> : null}
      <Badge variant={isConnected ? 'positive-soft' : 'secondary'}>
        {isConnected ? (nodeName === null ? 'Connected' : `Connected: ${nodeName}`) : 'Not connected'}
      </Badge>
    </>
  );

  const viewSwitch = (
    <SegmentedGroup aria-label="View" value={view} onValueChange={showView}>
      {hasBuilder ? <SegmentedButton value={AppView.Devices}>Devices</SegmentedButton> : null}
      <SegmentedButton value={AppView.Update}>Update</SegmentedButton>
      <SegmentedButton value={AppView.Presets}>{settingsLabel}</SegmentedButton>
      <SegmentedButton value={AppView.Feedback}>Host feedback</SegmentedButton>
    </SegmentedGroup>
  );
  // The device views stay mounted, so an upload keeps running and the log keeps filling across a switch.
  const deviceViews = (
    <>
      <div hidden={view !== AppView.Update} className="flex min-w-0 flex-col gap-4">
        {updateCards}
      </div>
      <div hidden={view !== AppView.Presets} className="flex min-w-0 flex-col gap-4">
        {settingsCards}
      </div>
      <div hidden={view !== AppView.Feedback} className="flex min-w-0 flex-col gap-4">
        {feedbackCards}
      </div>
    </>
  );

  if (builderInfo === null) {
    return (
      <PageLayout
        title="gemifi updater"
        contentRef={body}
        description="Device info, presets and settings, debug log and firmware upload, over Web MIDI or Web Serial."
        headerContentSlot={viewSwitch}
        actionSlot={statusBadges}
        footerSlot={saveBar}
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
          {statusBadges}
        </>
      }
      footerSlot={saveBar}
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
                setMode('serial');
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
