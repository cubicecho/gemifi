import { useState } from 'react';
import { InputField, SelectField, useAppForm } from '@/components/app-form';
import { CardLayout } from '@/components/card-layout';
import { DialogLayout } from '@/components/dialog-layout';
import { List } from '@/components/list';
import { ListItem } from '@/components/list-item';
import { Alert } from '@/components/ui/alert';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { ConfirmDialog } from '@/components/ui/confirm-dialog';
import { FormElement } from '@/components/ui/form-element';
import { ChevronDown, ChevronUp, Ellipsis, Pencil, Plus, Tag, Trash2, Undo2 } from '@/components/ui/icons';
import { Menu, MenuContent, MenuItem, MenuTrigger } from '@/components/ui/menu';
import { isPresetEmpty, isPresetStored, type PresetInfo, presetNameProblem, presetSlots } from '../protocol/presets.ts';
import { PRESET_NAME_MAX } from '../protocol/wire.ts';
import type { PresetStart, PresetsController } from './use-presets.ts';

/**
 * @param error - Anything thrown.
 * @returns Its message.
 */
function messageOf(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

/**
 * @param value - A name as typed.
 * @returns What is wrong with it, or undefined.
 */
function nameProblem(value: string): string | undefined {
  return presetNameProblem(value.trim()) ?? undefined;
}

/** The name field's hint, shared by both dialogs. */
const NAME_HINT = `Up to ${PRESET_NAME_MAX} letters, digits or symbols (plain ASCII). A device with a display shows it when the preset is chosen.`;

/** Props of the dialogs. */
interface DialogProps {
  controller: PresetsController;
  onOpenChange: (open: boolean) => void;
}

/**
 * Names a slot. Mounted each time it opens, so it starts from the slot's name.
 *
 * @param props - The controller and the slot.
 * @returns The dialog.
 */
function RenameDialog({ controller, preset, onOpenChange }: DialogProps & { preset: PresetInfo }) {
  const [failure, setFailure] = useState<string | null>(null);
  const form = useAppForm({
    defaultValues: { name: preset.name },
    onSubmit: async ({ value }) => {
      setFailure(null);
      try {
        await controller.rename(preset.slot, value.name.trim());
        onOpenChange(false);
      } catch (error) {
        setFailure(messageOf(error));
      }
    },
  });
  return (
    <DialogLayout
      open
      onOpenChange={onOpenChange}
      title={`Rename preset ${preset.slot}`}
      description="The name is stored on the device straight away."
      hasUnsavedChanges={() => form.state.isDefaultValue === false}
      contentSlot={
        <FormElement onSubmit={() => form.handleSubmit()}>
          <InputField
            form={form}
            name="name"
            label="Name"
            description={`${NAME_HINT} Leave it empty to go back to the firmware's name.`}
            autoCapitalize="none"
            autoCorrect={false}
            validators={{ onChange: ({ value }) => nameProblem(value) }}
          />
          {failure === null ? null : (
            <Alert variant="destructive" title="The name was not stored" description={failure} />
          )}
        </FormElement>
      }
      footerActionsSlot={(close) => (
        <>
          <Button variant="outline" onClick={close} content="Cancel" />
          <form.AppForm>
            <form.SubmitButton requireChanges content="Rename" pendingLabel="Renaming…" />
          </form.AppForm>
        </>
      )}
    />
  );
}

/** What New preset can start from, in the reader's words. */
const START_OPTIONS: ReadonlyArray<{ value: PresetStart; label: string }> = [
  { value: 'current', label: 'A copy of the current settings' },
  { value: 'defaults', label: "The firmware's defaults" },
];

/**
 * Makes a named preset in the next free slot, and makes it the active one.
 *
 * @param props - The controller, the slot it goes into, the active one and whether it has unsaved edits.
 * @returns The dialog.
 */
function NewPresetDialog({
  controller,
  slot,
  active,
  isDirty,
  onOpenChange,
}: DialogProps & { slot: number; active: PresetInfo; isDirty: boolean }) {
  const [failure, setFailure] = useState<string | null>(null);
  const form = useAppForm({
    defaultValues: { name: '', from: 'current' as PresetStart },
    onSubmit: async ({ value }) => {
      setFailure(null);
      try {
        await controller.create({ slot, name: value.name.trim(), from: value.from });
        onOpenChange(false);
      } catch (error) {
        setFailure(messageOf(error));
      }
    },
  });
  return (
    <DialogLayout
      open
      onOpenChange={onOpenChange}
      title="New preset"
      description={`It goes into slot ${slot}, after the others, and becomes the active preset.`}
      hasUnsavedChanges={() => form.state.isDefaultValue === false}
      contentSlot={
        <FormElement onSubmit={() => form.handleSubmit()}>
          <InputField
            form={form}
            name="name"
            label="Name"
            required
            description={NAME_HINT}
            autoCapitalize="none"
            autoCorrect={false}
            validators={{
              onChange: ({ value }) => (value.trim() === '' ? 'Give the preset a name.' : nameProblem(value)),
            }}
          />
          <SelectField form={form} name="from" label="Start from" required options={START_OPTIONS} />
          <form.Subscribe selector={(s) => s.values.from}>
            {(from) =>
              from === 'defaults' && isDirty ? (
                <Alert
                  variant="warning"
                  title={`The unsaved edits to "${active.name}" are dropped`}
                  description="Starting from the defaults switches presets. Save to device first to keep them, or start from a copy of the current settings."
                />
              ) : null
            }
          </form.Subscribe>
          {failure === null ? null : (
            <Alert variant="destructive" title="The preset was not made" description={failure} />
          )}
        </FormElement>
      }
      footerActionsSlot={(close) => (
        <>
          <Button variant="outline" onClick={close} content="Cancel" />
          <form.AppForm>
            <form.SubmitButton content="Make preset" pendingLabel="Making…" />
          </form.AppForm>
        </>
      )}
    />
  );
}

/** Props of {@link PresetsCard}. */
export interface PresetsCardProps {
  controller: PresetsController;
  /** Another action of the editor is running. */
  disabled: boolean;
}

/** An open dialog, and the slot it is about. */
type OpenDialog =
  | { kind: 'new' }
  | { kind: 'rename'; slot: number }
  | { kind: 'reset'; slot: number }
  | { kind: 'delete'; slot: number }
  | { kind: 'move'; slot: number; to: number }
  | { kind: 'switch'; slot: number };

/**
 * @param inUse - The presets in use, in slot order.
 * @param deleted - The slot being deleted.
 * @param active - The active slot.
 * @returns What happens to the active preset, as a sentence, or '' when it just moves.
 */
function handOver(inUse: PresetInfo[], deleted: number, active: number): string {
  if (deleted !== active) {
    return '';
  }
  const index = inUse.findIndex((info) => info.slot === deleted);
  const next = inUse[index + 1] ?? inUse[index - 1];
  return next === undefined ? '' : ` "${next.name}" becomes the active preset.`;
}

/**
 * The device's presets as a list (§4.5): choose one to make it active (asking first when the
 * active one has unsaved edits), make a new one in the next free slot, rename, reset or delete
 * any of them. Deleting moves the later presets up, so the free slots are always last.
 *
 * @param props - The presets controller.
 * @returns The card, or nothing on a device without presets.
 */
export function PresetsCard({ controller, disabled }: PresetsCardProps) {
  const { state, action } = controller;
  const [dialog, setDialog] = useState<OpenDialog | null>(null);
  if (state.kind === 'off') {
    return null;
  }
  const isBusy = disabled || action !== null;
  const presets = state.kind === 'ready' ? state.presets : [];
  const activeSlot = state.kind === 'ready' ? state.active : 0;
  const { inUse, free, canDelete } = presetSlots(presets, activeSlot);
  const active = presets.find((info) => info.slot === activeSlot) ?? presets[0];
  const isDirty = state.kind === 'ready' && state.dirty;
  const nextFree = free[0];
  const close = () => setDialog(null);
  const choose = (slot: number) => {
    if (slot === active?.slot) {
      return;
    }
    if (isDirty) {
      setDialog({ kind: 'switch', slot });
      return;
    }
    controller.select(slot);
  };
  const about = (slot: number) => presets.find((info) => info.slot === slot);
  const target = dialog !== null && dialog.kind !== 'new' ? about(dialog.slot) : undefined;
  /** Moves a preset one place, asking first when the active one's unsaved edits would go. */
  const shift = (slot: number, to: number) => {
    if (isDirty) {
      setDialog({ kind: 'move', slot, to });
      return;
    }
    controller.move(slot, to);
  };
  const rows = inUse.map((info, position) => {
    const above = inUse[position - 1];
    const below = inUse[position + 1];
    const isActive = info.slot === active?.slot;
    const badges = [
      isActive ? (
        <Badge key="active" variant="info-soft">
          Active
        </Badge>
      ) : null,
      isActive && isDirty ? (
        <Badge key="dirty" variant="warning-soft">
          Unsaved edits
        </Badge>
      ) : null,
      // Before PRESET_DELETE, the empty slots are listed too: copies of slot 1.
      canDelete === false && isPresetEmpty(info) ? (
        <Badge key="empty" variant="secondary">
          Empty
        </Badge>
      ) : null,
    ];
    const canReset = isPresetStored(info) || (isActive && isDirty);
    return (
      <ListItem
        key={info.slot}
        leadingSlot={<span className="w-5 text-right font-mono text-foreground/60 text-sm">{info.slot}</span>}
        title={info.name}
        meta={<span className="flex flex-wrap gap-1">{badges}</span>}
        selected={isActive}
        onClick={isBusy ? undefined : () => choose(info.slot)}
        actionSlot={
          <Menu>
            <MenuTrigger asChild>
              <Button
                size="icon-sm"
                variant="outline"
                aria-label={`More for ${info.name}`}
                iconSlot={<Ellipsis />}
                disabled={isBusy}
              />
            </MenuTrigger>
            <MenuContent align="end">
              <MenuItem
                iconSlot={<Pencil />}
                label="Rename…"
                onSelect={() => setDialog({ kind: 'rename', slot: info.slot })}
              />
              {canDelete ? (
                <>
                  <MenuItem
                    iconSlot={<ChevronUp />}
                    label="Move up"
                    disabled={above === undefined}
                    onSelect={() => (above === undefined ? null : shift(info.slot, above.slot))}
                  />
                  <MenuItem
                    iconSlot={<ChevronDown />}
                    label="Move down"
                    disabled={below === undefined}
                    onSelect={() => (below === undefined ? null : shift(info.slot, below.slot))}
                  />
                </>
              ) : null}
              <MenuItem
                iconSlot={<Undo2 />}
                label="Reset…"
                disabled={canReset === false}
                onSelect={() => setDialog({ kind: 'reset', slot: info.slot })}
              />
              {canDelete ? (
                <MenuItem
                  iconSlot={<Trash2 />}
                  label="Delete…"
                  destructive
                  disabled={inUse.length < 2}
                  onSelect={() => setDialog({ kind: 'delete', slot: info.slot })}
                />
              ) : null}
            </MenuContent>
          </Menu>
        }
      />
    );
  });
  const slotsNote =
    state.kind !== 'ready' || canDelete === false
      ? null
      : nextFree === undefined
        ? `All ${presets.length} slots are in use. Delete a preset to make room for a new one.`
        : `${inUse.length} of ${presets.length} slots in use.`;
  return (
    <CardLayout
      title="Presets"
      iconSlot={<Tag />}
      description="Each preset has its own name and its own copy of the settings below. Choose one to make it active; the device switches at once, and its preset buttons step through these."
      actionSlot={
        <Button
          size="sm"
          variant="info"
          iconSlot={<Plus />}
          content="New preset"
          disabled={isBusy || active === undefined || nextFree === undefined}
          hint={
            state.kind === 'ready' && nextFree === undefined
              ? canDelete
                ? 'Every slot is in use: delete a preset first.'
                : 'Every slot holds a preset.'
              : undefined
          }
          loading={action === 'create'}
          loadingLabel="Making…"
          onClick={() => setDialog({ kind: 'new' })}
        />
      }
      loading={state.kind === 'loading'}
      contentSlot={
        <>
          {state.kind === 'error' ? (
            <Alert
              variant="destructive"
              title="The presets could not be read"
              description={state.message}
              actionSlot={<Button size="sm" variant="outline" content="Try again" onClick={controller.reload} />}
            />
          ) : (
            <List label="Presets" contentSlot={rows} />
          )}
          {slotsNote === null ? null : <p className="text-foreground/60 text-sm">{slotsNote}</p>}
          {controller.actionError === null ? null : (
            <Alert variant="destructive" title="That did not work" description={controller.actionError} />
          )}
          {dialog?.kind === 'new' && active !== undefined && nextFree !== undefined ? (
            <NewPresetDialog
              controller={controller}
              slot={nextFree.slot}
              active={active}
              isDirty={isDirty}
              onOpenChange={(open) => (open ? null : close())}
            />
          ) : null}
          {dialog?.kind === 'rename' && target !== undefined ? (
            <RenameDialog controller={controller} preset={target} onOpenChange={(open) => (open ? null : close())} />
          ) : null}
          <ConfirmDialog
            open={dialog?.kind === 'reset' && target !== undefined}
            onOpenChange={(open) => (open ? null : close())}
            title={`Reset "${target?.name ?? ''}"?`}
            description={
              target?.slot === active?.slot
                ? "Its stored changes and its name are dropped, so it holds the firmware's settings for this slot again. Unsaved edits go too. The other presets are not touched."
                : "Its stored changes and its name are dropped, so it holds the firmware's settings for this slot again. The other presets are not touched."
            }
            confirmLabel="Reset preset"
            onConfirm={() => {
              if (target !== undefined) {
                controller.reset(target.slot);
              }
              close();
            }}
          />
          <ConfirmDialog
            open={dialog?.kind === 'delete' && target !== undefined}
            onOpenChange={(open) => (open ? null : close())}
            title={`Delete "${target?.name ?? ''}"?`}
            description={`Its settings and name are gone, and the presets after it move up one slot.${handOver(inUse, target?.slot ?? 0, activeSlot)}${isDirty ? ` The unsaved edits to "${active?.name ?? ''}" are dropped.` : ''}`}
            confirmLabel="Delete preset"
            onConfirm={() => {
              if (target !== undefined) {
                controller.remove(target.slot);
              }
              close();
            }}
          />
          <ConfirmDialog
            open={dialog?.kind === 'move' && target !== undefined}
            onOpenChange={(open) => (open ? null : close())}
            title={`Move "${target?.name ?? ''}"?`}
            description={`Moving presets reloads "${active?.name ?? ''}", so its unsaved edits are dropped. Save to device first to keep them.`}
            confirmLabel="Move preset"
            onConfirm={() => {
              if (dialog?.kind === 'move') {
                controller.move(dialog.slot, dialog.to);
              }
              close();
            }}
          />
          <ConfirmDialog
            open={dialog?.kind === 'switch' && target !== undefined}
            onOpenChange={(open) => (open ? null : close())}
            title={`Switch to "${target?.name ?? ''}"?`}
            description={`The unsaved edits to "${active?.name ?? ''}" are dropped. Save to device first to keep them.`}
            confirmLabel="Switch preset"
            onConfirm={() => {
              if (target !== undefined) {
                controller.select(target.slot);
              }
              close();
            }}
          />
        </>
      }
    />
  );
}
