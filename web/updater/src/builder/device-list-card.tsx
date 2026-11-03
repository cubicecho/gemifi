import { CardLayout } from '@/components/card-layout';
import { ConfirmButton } from '@/components/confirm-button';
import { EmptyState } from '@/components/empty-state';
import { List } from '@/components/list';
import { ListItem } from '@/components/list-item';
import { QueryState } from '@/components/query-state';
import { Section } from '@/components/section';
import { Alert } from '@/components/ui/alert';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { Library, Plus, Trash2 } from '@/components/ui/icons';
import { type BuilderDevice, type DeviceListing, ROOT_MINE, ROOT_PRESETS } from './api.ts';
import { deviceTitle, platformLabel } from './platforms.ts';
import type { RequestState } from './use-request.ts';

/** Props of {@link DeviceListCard}. */
export interface DeviceListCardProps {
  devices: RequestState<DeviceListing>;
  /** The device open in the editor. */
  selectedId: string | null;
  onSelect: (device: BuilderDevice) => void;
  /** Opens the new-device form. */
  onNew: () => void;
  onDelete: (device: BuilderDevice) => void;
  /** What went wrong with the last delete, or null. */
  deleteFailure: string | null;
  /** The node name the connected device reports in HELLO, or null: its files get a Connected badge. */
  connectedNodeName: string | null;
}

/**
 * @param device - A device file.
 * @returns Its build badge: built, out of date (edited since), or not built.
 */
function buildLook(device: BuilderDevice) {
  if (device.images.length === 0) {
    return { label: 'Not built', variant: 'secondary' } as const;
  }
  return device.isStale
    ? ({ label: 'Out of date', variant: 'warning-soft' } as const)
    : ({ label: 'Built', variant: 'positive-soft' } as const);
}

/**
 * One device file as a row: its name, its file, its platform and whether it has been built.
 *
 * @param props - The device, whether it is open, and the handlers.
 * @returns The row.
 */
function DeviceRow(props: {
  device: BuilderDevice;
  isSelected: boolean;
  isConnected: boolean;
  onSelect: () => void;
  onDelete: () => void;
}) {
  const { device, isSelected, isConnected, onSelect, onDelete } = props;
  const title = deviceTitle(device);
  const build = buildLook(device);
  const canDelete = device.root === ROOT_MINE && device.writable;
  return (
    <ListItem
      title={title}
      description={
        <span className="flex flex-wrap items-center gap-1.5">
          <span className="font-mono text-foreground/60 text-xs">{device.file}</span>
          <Badge variant="outline">{platformLabel(device.platform)}</Badge>
          <Badge variant={build.variant}>{build.label}</Badge>
          {isConnected ? <Badge variant="info-soft">Connected</Badge> : null}
        </span>
      }
      selected={isSelected}
      onClick={onSelect}
      actionSlot={
        canDelete ? (
          <ConfirmButton
            aria-label={`Delete ${title}`}
            size="icon-sm"
            variant="outline"
            iconSlot={<Trash2 />}
            title={`Delete ${title}?`}
            description={`${device.file} is removed from your devices folder. Its built images are deleted with it.`}
            confirmLabel="Delete device"
            onConfirm={onDelete}
          />
        ) : undefined
      }
    />
  );
}

/** The device files, the user's own first, with New device in the header. */
export function DeviceListCard(props: DeviceListCardProps) {
  const { devices, selectedId, onSelect, onNew, onDelete, deleteFailure, connectedNodeName } = props;
  const all = devices.data?.devices ?? [];
  const mine = all.filter((device) => device.root === ROOT_MINE);
  const presets = all.filter((device) => device.root === ROOT_PRESETS);
  const rowsOf = (list: BuilderDevice[]) =>
    list.map((device) => (
      <DeviceRow
        key={device.id}
        device={device}
        isSelected={device.id === selectedId}
        isConnected={connectedNodeName !== null && device.nodeName === connectedNodeName}
        onSelect={() => onSelect(device)}
        onDelete={() => onDelete(device)}
      />
    ));

  return (
    <CardLayout
      title="Devices"
      iconSlot={<Library />}
      description="Your device files, and the presets a new one starts from."
      actionSlot={<Button variant="info" size="sm" iconSlot={<Plus />} content="New device" onClick={onNew} />}
      contentSlot={
        <>
          <QueryState query={devices} what="the device list" count={all.length} />
          {deleteFailure === null ? null : (
            <Alert variant="destructive" title="The device was not deleted" description={deleteFailure} />
          )}
          {devices.data === undefined ? null : (
            <>
              <Section
                title="My devices"
                level={3}
                contentSlot={
                  mine.length === 0 ? (
                    <EmptyState compact title="None yet. Start one from a preset with New device." />
                  ) : (
                    <List label="My devices" contentSlot={rowsOf(mine)} />
                  )
                }
              />
              {presets.length === 0 ? null : (
                <Section
                  title="Presets"
                  level={3}
                  contentSlot={<List label="Presets" contentSlot={rowsOf(presets)} />}
                />
              )}
            </>
          )}
        </>
      }
    />
  );
}
