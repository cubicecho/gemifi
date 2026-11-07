import { type Ref, useCallback, useEffect, useRef, useState } from 'react';
import { CardLayout } from '@/components/card-layout';
import { EmptyState } from '@/components/empty-state';
import { LoadError, RowSkeleton } from '@/components/load-state';
import { SidebarLayout } from '@/components/split-layout';
import { Alert } from '@/components/ui/alert';
import { FileText } from '@/components/ui/icons';
import { UnsavedChangesDialog, useUnsavedChangesGuard } from '@/components/unsaved-changes-guard';
import type { PickedImage } from '../device/picked-image.ts';
import { useSearchParam } from '../navigation/use-search-param.ts';
import type { PickedBytes } from '../serial/esp-manifest.ts';
import {
  type BuilderDevice,
  type BuilderInfo,
  type DeviceListing,
  deleteDevice,
  listPresets,
  PLATFORM_TOOLCHAIN,
  readDeviceFile,
} from './api.ts';
import { BuildLogCard } from './build-log-card.tsx';
import { DeviceEditorCard } from './device-editor-card.tsx';
import { DeviceListCard } from './device-list-card.tsx';
import { ImagesCard } from './images-card.tsx';
import { NewDeviceDialog } from './new-device-dialog.tsx';
import { deviceTitle, platformLabel, presetNameOf } from './platforms.ts';
import { type BuildState, useBuild } from './use-build.ts';
import { type RequestState, useRequest } from './use-request.ts';
import { YamlReferenceCard } from './yaml-reference-card.tsx';

/** Where the list and the open device stack (the layout's `stackBelow="lg"`), so opening one scrolls to it. */
const STACKED_QUERY = '(width < 64rem)';

/** Asks the builder to open a device file; a new `nonce` asks again for the same file. */
export interface OpenRequest {
  id: string;
  nonce: number;
}

/** Props of {@link BuilderView}. */
export interface BuilderViewProps {
  info: BuilderInfo;
  /** The device files, asked for by the page so the Update view can match the connected device too. */
  devices: RequestState<DeviceListing>;
  /** The node name the connected device reports in HELLO, or null. */
  connectedNodeName: string | null;
  /** The Update view's "open this device's file", or null. */
  openRequest: OpenRequest | null;
  /** An upload or install is running on the Update view. */
  isBusy: boolean;
  /** The first-install card is on the page: this browser has Web Serial. */
  canInstall: boolean;
  onFlash: (image: PickedImage) => void;
  onInstall: (files: PickedBytes[]) => void;
}

/**
 * @param device - A device file.
 * @param info - What the builder has installed.
 * @returns Whether its platform is known and the builder lacks the toolchain for it.
 */
function isToolchainMissingFor(device: BuilderDevice, info: BuilderInfo): boolean {
  return device.platform !== null && info.toolchains[PLATFORM_TOOLCHAIN[device.platform]] === false;
}

/**
 * @param device - The device to build.
 * @param info - What the builder has installed.
 * @param build - The last build.
 * @returns Why the device cannot be built now, or null when it can.
 */
function buildBlockerOf(device: BuilderDevice, info: BuilderInfo, build: BuildState): string | null {
  if (build.kind === 'running' && build.deviceId !== device.id) {
    return 'Another device is building. Wait for it to finish.';
  }
  if (device.platform === null) {
    return 'The file names no platform this builder knows. Set platform: and save first.';
  }
  if (isToolchainMissingFor(device, info)) {
    return `This builder has no toolchain for ${platformLabel(device.platform)}.`;
  }
  return null;
}

/** Props of {@link DeviceDetail}. */
interface DeviceDetailProps extends Omit<BuilderViewProps, 'info' | 'devices' | 'connectedNodeName' | 'openRequest'> {
  device: BuilderDevice;
  buildBlocker: string | null;
  isToolchainMissing: boolean;
  build: BuildState;
  onBuild: () => void;
  onSaved: () => void;
  onUnsavedCheckChange: (check: (() => boolean) | null) => void;
  onCopyPreset: () => void;
  ref?: Ref<HTMLDivElement> | undefined;
}

/** The open device: its editor, its last build and its images. */
function DeviceDetail(props: DeviceDetailProps) {
  const { device, buildBlocker, isToolchainMissing, build, onBuild, onSaved, onUnsavedCheckChange, onCopyPreset } =
    props;
  const file = useRequest(() => readDeviceFile(device.id), device.id);
  const buildHere = build.kind !== 'idle' && build.deviceId === device.id ? build : null;

  let editor = (
    <CardLayout title={deviceTitle(device)} iconSlot={<FileText />} contentSlot={<RowSkeleton rows={6} />} />
  );
  if (file.isError) {
    editor = <LoadError error={file.error} onRetry={file.refetch} what="this device file" />;
  } else if (file.data !== undefined) {
    editor = (
      <DeviceEditorCard
        key={device.id}
        device={device}
        savedText={file.data}
        buildBlocker={buildBlocker}
        isBuilding={buildHere?.kind === 'running'}
        onSaved={onSaved}
        onBuild={onBuild}
        onUnsavedCheckChange={onUnsavedCheckChange}
        onCopyPreset={onCopyPreset}
      />
    );
  }

  return (
    // No scroll anchoring here: when the editor replaces its placeholder, the browser would otherwise
    // scroll down by the difference to keep the cards under it still.
    <div ref={props.ref} className="flex min-w-0 scroll-mt-4 flex-col gap-4 [overflow-anchor:none]">
      {isToolchainMissing ? (
        <Alert
          variant="warning"
          title={`No ${platformLabel(device.platform)} toolchain here`}
          description="This builder was started without the toolchain this device needs, so it can be edited and validated but not built. Use the builder image that includes it, or install it where the builder runs."
        />
      ) : null}
      {editor}
      {buildHere === null ? null : <BuildLogCard build={buildHere} />}
      <ImagesCard
        device={device}
        isBusy={props.isBusy}
        canInstall={props.canInstall}
        onFlash={props.onFlash}
        onInstall={props.onInstall}
      />
      <YamlReferenceCard />
    </div>
  );
}

/**
 * The builder: the device list beside the open device, where its YAML is edited, validated and
 * built, and its images are handed to the Update view.
 */
export function BuilderView(props: BuilderViewProps) {
  const { info, devices, connectedNodeName, openRequest, isBusy, canInstall, onFlash, onInstall } = props;
  const presets = useRequest(listPresets, 'presets');
  const [selectedId, setSelectedId] = useSearchParam('device');
  // The open editor's own question, asked only when something would throw its edits away.
  const unsavedCheck = useRef<(() => boolean) | null>(null);
  const guard = useUnsavedChangesGuard({ hasUnsavedChanges: () => unsavedCheck.current?.() ?? false });
  const [newFromPreset, setNewFromPreset] = useState<string | null>(null);
  const [deleteFailure, setDeleteFailure] = useState<string | null>(null);
  const { refetch: refetchDevices } = devices;
  const build = useBuild(refetchDevices);
  const selected = devices.data?.devices.find((device) => device.id === selectedId) ?? null;
  const detail = useRef<HTMLDivElement | null>(null);
  const shouldReveal = useRef(false);
  const open = (id: string) =>
    guard.leave(() => {
      shouldReveal.current = true;
      setSelectedId(id);
    });
  // On a phone the device opens under the list, so a choice there scrolls down to it.
  // biome-ignore lint/correctness/useExhaustiveDependencies: the open device's id is the trigger; a new device appears a refetch later.
  useEffect(() => {
    if (shouldReveal.current === false || detail.current === null) {
      return;
    }
    shouldReveal.current = false;
    if (window.matchMedia(STACKED_QUERY).matches) {
      detail.current.scrollIntoView({ block: 'start' });
    }
  }, [selected?.id]);
  // The Update view's request goes through the same unsaved-edits question as a click in the list.
  // biome-ignore lint/correctness/useExhaustiveDependencies: a new request is the trigger, not `open`.
  useEffect(() => {
    if (openRequest !== null) {
      open(openRequest.id);
    }
  }, [openRequest]);
  const onUnsavedCheckChange = useCallback((check: (() => boolean) | null) => {
    unsavedCheck.current = check;
  }, []);

  const remove = (device: BuilderDevice) => {
    setDeleteFailure(null);
    deleteDevice(device.id).then(
      () => {
        if (device.id === selectedId) {
          setSelectedId(null);
        }
        refetchDevices();
      },
      (error: unknown) => setDeleteFailure(error instanceof Error ? error.message : String(error)),
    );
  };

  return (
    <>
      <SidebarLayout
        sidebarPosition="start"
        sidebarWidth="md"
        stackBelow="lg"
        sidebarSlot={
          <DeviceListCard
            devices={devices}
            selectedId={selectedId}
            deleteFailure={deleteFailure}
            connectedNodeName={connectedNodeName}
            onSelect={(device) => open(device.id)}
            onNew={() => setNewFromPreset('')}
            onDelete={remove}
          />
        }
        contentSlot={
          selected === null ? (
            <CardLayout
              contentSlot={
                <EmptyState
                  iconSlot={<FileText />}
                  title={devices.isPending ? 'Loading devices…' : 'Choose a device'}
                  description={`Open one of your devices to edit and build it, or start a new one from a preset. New devices are saved in ${info.userDir}.`}
                />
              }
            />
          ) : (
            <DeviceDetail
              ref={detail}
              device={selected}
              buildBlocker={buildBlockerOf(selected, info, build.state)}
              isToolchainMissing={isToolchainMissingFor(selected, info)}
              build={build.state}
              onBuild={() => build.start(selected.id)}
              onSaved={refetchDevices}
              onUnsavedCheckChange={onUnsavedCheckChange}
              onCopyPreset={() => setNewFromPreset(presetNameOf(selected))}
              isBusy={isBusy}
              canInstall={canInstall}
              onFlash={onFlash}
              onInstall={onInstall}
            />
          )
        }
      />
      <NewDeviceDialog
        open={newFromPreset !== null}
        onOpenChange={(open) => {
          if (open === false) {
            setNewFromPreset(null);
          }
        }}
        presets={presets}
        initialPreset={newFromPreset ?? ''}
        onCreated={(id) => {
          refetchDevices();
          open(id);
        }}
      />
      <UnsavedChangesDialog guard={guard} />
    </>
  );
}
