import { useState } from 'react';
import { InputField, SelectField, useAppForm } from '@/components/app-form';
import { Combobox } from '@/components/combobox';
import { ConfirmButton } from '@/components/confirm-button';
import { DialogLayout } from '@/components/dialog-layout';
import { FormField } from '@/components/form-field';
import { Alert } from '@/components/ui/alert';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { ConfirmDialog } from '@/components/ui/confirm-dialog';
import { FormElement } from '@/components/ui/form-element';
import { Copy, Pencil } from '@/components/ui/icons';
import { isPresetStored, type PresetInfo, presetNameProblem } from '../protocol/presets.ts';
import { PRESET_NAME_MAX } from '../protocol/wire.ts';
import type { PresetsController } from './use-presets.ts';

/**
 * @param info - A slot.
 * @returns How the picker and the dialogs name it, e.g. `2. Drums`.
 */
function presetLabel(info: PresetInfo): string {
  return `${info.slot}. ${info.name}`;
}

/**
 * @param error - Anything thrown.
 * @returns Its message.
 */
function messageOf(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

/** Props of the two dialogs. */
interface PresetDialogProps {
  controller: PresetsController;
  presets: PresetInfo[];
  active: PresetInfo;
  onOpenChange: (open: boolean) => void;
}

/**
 * Renames the active preset. Mounted each time it opens, so it starts from the current name.
 *
 * @param props - The controller, the slots and the active one.
 * @returns The dialog.
 */
function RenameDialog({ controller, active, onOpenChange }: PresetDialogProps) {
  const [failure, setFailure] = useState<string | null>(null);
  const form = useAppForm({
    defaultValues: { name: active.name },
    onSubmit: async ({ value }) => {
      setFailure(null);
      try {
        await controller.rename(active.slot, value.name.trim());
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
      title={`Rename preset ${active.slot}`}
      description="The name is stored on the device at once, and shows on its display when the preset is selected."
      hasUnsavedChanges={() => form.state.isDefaultValue === false}
      contentSlot={
        <FormElement onSubmit={() => form.handleSubmit()}>
          <InputField
            form={form}
            name="name"
            label="Name"
            description={`Up to ${PRESET_NAME_MAX} characters. Leave it empty to go back to the firmware's name.`}
            autoCapitalize="none"
            autoCorrect={false}
            validators={{ onChange: ({ value }) => presetNameProblem(value.trim()) ?? undefined }}
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

/**
 * Stores the live values into a slot ("save as") and makes it active.
 *
 * @param props - The controller, the slots and the active one.
 * @returns The dialog.
 */
function SaveAsDialog({ controller, presets, active, onOpenChange }: PresetDialogProps) {
  const [failure, setFailure] = useState<string | null>(null);
  const form = useAppForm({
    defaultValues: { slot: String(active.slot) },
    onSubmit: async ({ value }) => {
      setFailure(null);
      try {
        await controller.saveAs(Number(value.slot));
        onOpenChange(false);
      } catch (error) {
        setFailure(messageOf(error));
      }
    },
  });
  const options = presets.map((info) => ({
    value: String(info.slot),
    label: presetLabel(info),
    meta:
      info.slot === active.slot ? (
        <Badge key="active" variant="info-soft">
          Active
        </Badge>
      ) : undefined,
  }));
  return (
    <DialogLayout
      open
      onOpenChange={onOpenChange}
      title="Save as preset"
      description={`Stores the current values of "${active.name}" into the preset you choose, and makes it the active one. That preset keeps its own name, and what it held before is replaced.`}
      hasUnsavedChanges={() => form.state.isDefaultValue === false}
      contentSlot={
        <FormElement onSubmit={() => form.handleSubmit()}>
          <SelectField form={form} name="slot" label="Save into" required options={options} />
          {failure === null ? null : (
            <Alert variant="destructive" title="The preset was not saved" description={failure} />
          )}
        </FormElement>
      }
      footerActionsSlot={(close) => (
        <>
          <Button variant="outline" onClick={close} content="Cancel" />
          <form.AppForm>
            <form.SubmitButton content="Save" pendingLabel="Saving…" />
          </form.AppForm>
        </>
      )}
    />
  );
}

/** Props of {@link PresetControls}. */
export interface PresetControlsProps {
  controller: PresetsController;
  /** Another action of the editor is running. */
  disabled: boolean;
}

/**
 * The preset picker and its actions (§4.5): select (asking first when the active preset has
 * unsaved edits), rename, save as and reset. Save to device (COMMIT) stores the active preset.
 *
 * @param props - The presets controller.
 * @returns The controls, or the list's failure.
 */
export function PresetControls({ controller, disabled }: PresetControlsProps) {
  const { state, action } = controller;
  const [pendingSlot, setPendingSlot] = useState<number | null>(null);
  const [dialog, setDialog] = useState<'rename' | 'save-as' | null>(null);
  if (state.kind === 'off') {
    return null;
  }
  if (state.kind === 'error') {
    return (
      <Alert
        variant="destructive"
        title="The presets could not be read"
        description={state.message}
        actionSlot={<Button size="sm" variant="outline" content="Try again" onClick={controller.reload} />}
      />
    );
  }
  if (state.kind === 'loading') {
    return <FormField label="Active preset" loading controlSlot={<span />} />;
  }
  const { presets } = state;
  const active = presets.find((info) => info.slot === state.active) ?? presets[0];
  if (active === undefined) {
    return null;
  }
  const isBusy = disabled || action !== null;
  const pending = presets.find((info) => info.slot === pendingSlot);
  const options = presets.map((info) => {
    const isActive = info.slot === state.active;
    const isDirty = isActive && state.dirty;
    const meta = isDirty ? (
      <Badge key="dirty" variant="warning-soft">
        Unsaved edits
      </Badge>
    ) : isPresetStored(info) ? (
      <Badge key="stored" variant="secondary">
        Changed
      </Badge>
    ) : undefined;
    return { value: String(info.slot), label: presetLabel(info), meta };
  });
  const choose = (slot: number) => {
    if (slot === state.active) {
      return;
    }
    if (state.dirty) {
      setPendingSlot(slot);
      return;
    }
    controller.select(slot);
  };
  const dialogProps = { controller, presets, active, onOpenChange: (open: boolean) => setDialog(open ? dialog : null) };
  return (
    <div className="grid gap-3">
      <FormField
        label="Active preset"
        description={
          state.dirty
            ? 'This preset has unsaved edits: Save to device keeps them, switching drops them.'
            : 'What the controls send. Settings marked "per preset" belong to it; the rest are the same in every preset.'
        }
        controlSlot={(wired) => (
          <Combobox
            {...wired}
            options={options}
            value={String(state.active)}
            disabled={isBusy}
            onValueChange={(next) => choose(Number(next))}
          />
        )}
      />
      <div className="flex flex-wrap gap-2">
        <Button
          size="sm"
          variant="outline"
          iconSlot={<Pencil />}
          content="Rename…"
          disabled={isBusy}
          onClick={() => setDialog('rename')}
        />
        <Button
          size="sm"
          variant="outline"
          iconSlot={<Copy />}
          content="Save as…"
          disabled={isBusy}
          loading={action === 'save-as'}
          loadingLabel="Saving…"
          onClick={() => setDialog('save-as')}
        />
        <ConfirmButton
          size="sm"
          content="Reset preset"
          disabled={isBusy || (isPresetStored(active) === false && state.dirty === false)}
          loading={action === 'reset'}
          loadingLabel="Resetting…"
          title={`Reset "${active.name}"?`}
          description="Its stored changes and its name are dropped, so it is the firmware's preset again. Unsaved edits go too. The other presets are not touched."
          confirmLabel="Reset"
          onConfirm={() => controller.reset(active.slot)}
        />
      </div>
      {controller.actionError === null ? null : (
        <Alert variant="destructive" title="That did not work" description={controller.actionError} />
      )}
      <ConfirmDialog
        open={pending !== undefined}
        onOpenChange={(open) => {
          if (open === false) {
            setPendingSlot(null);
          }
        }}
        title={pending === undefined ? 'Switch preset?' : `Switch to "${pending.name}"?`}
        description={`The unsaved edits to "${active.name}" are dropped. Save to device first to keep them.`}
        confirmLabel="Switch"
        onConfirm={() => {
          if (pending !== undefined) {
            controller.select(pending.slot);
          }
          setPendingSlot(null);
        }}
      />
      {dialog === 'rename' ? <RenameDialog {...dialogProps} /> : null}
      {dialog === 'save-as' ? <SaveAsDialog {...dialogProps} /> : null}
    </div>
  );
}
