import { useState } from 'react';
import { InputField, SelectField, useAppForm } from '@/components/app-form';
import { DialogLayout } from '@/components/dialog-layout';
import { Alert } from '@/components/ui/alert';
import { Button } from '@/components/ui/button';
import { FormElement } from '@/components/ui/form-element';
import { type BuilderPreset, createDevice } from './api.ts';
import { platformLabel } from './platforms.ts';
import type { RequestState } from './use-request.ts';

/** What the builder accepts as a device name: it becomes `<name>.yaml`. */
const DEVICE_NAME = /^[A-Za-z0-9][A-Za-z0-9._-]*$/;

/** Props of {@link NewDeviceDialog}. */
export interface NewDeviceDialogProps {
  open: boolean;
  onOpenChange: (open: boolean) => void;
  presets: RequestState<BuilderPreset[]>;
  /** The preset chosen when it opens. */
  initialPreset: string;
  /** Called with the new device's id once it is written. */
  onCreated: (id: string) => void;
}

/**
 * The form inside the dialog, mounted each time it opens so it starts from `initialPreset`.
 *
 * @param props - The dialog's props, less its open state.
 * @returns The dialog.
 */
function NewDeviceForm({ onOpenChange, presets, initialPreset, onCreated }: Omit<NewDeviceDialogProps, 'open'>) {
  const [failure, setFailure] = useState<string | null>(null);
  const form = useAppForm({
    defaultValues: { name: '', preset: initialPreset },
    onSubmit: async ({ value }) => {
      setFailure(null);
      try {
        const id = await createDevice(value.name.trim(), value.preset);
        onCreated(id);
        onOpenChange(false);
      } catch (error) {
        setFailure(error instanceof Error ? error.message : String(error));
      }
    },
  });
  const options = (presets.data ?? []).map((preset) => ({
    value: preset.name,
    label: preset.name,
    hint: preset.description === '' ? undefined : preset.description,
    group: platformLabel(preset.platform),
    className: 'font-mono',
  }));

  return (
    <DialogLayout
      open
      onOpenChange={onOpenChange}
      title="New device"
      description="Copies a preset into your devices folder, where you can edit and build it."
      hasUnsavedChanges={() => form.state.isDefaultValue === false}
      contentSlot={
        <FormElement onSubmit={() => form.handleSubmit()}>
          <InputField
            form={form}
            name="name"
            label="Name"
            required
            description="Letters, digits, dots, dashes and underscores. The file is saved as this name with .yaml; in lowercase with dashes it also becomes the YAML's name: (the node name)."
            placeholder="my-pedalboard"
            autoCapitalize="none"
            autoCorrect={false}
            validators={{
              onChange: ({ value }) => {
                const name = value.trim();
                if (name === '') {
                  return 'Give the device a name';
                }
                return DEVICE_NAME.test(name) ? undefined : 'Use letters, digits, dots, dashes and underscores only';
              },
            }}
          />
          <SelectField
            form={form}
            name="preset"
            label="Preset"
            required
            placeholder={presets.isPending ? 'Loading presets…' : 'Choose a preset'}
            options={presets.isError ? [{ note: 'The presets could not be loaded.' }] : options}
            validators={{ onChange: ({ value }) => (value === '' ? 'Choose the preset to start from' : undefined) }}
          />
          {failure === null ? null : (
            <Alert variant="destructive" title="The device was not created" description={failure} />
          )}
        </FormElement>
      }
      footerActionsSlot={(close) => (
        <>
          <Button variant="outline" onClick={close} content="Cancel" />
          <form.AppForm>
            <form.SubmitButton variant="info" content="Create device" pendingLabel="Creating…" />
          </form.AppForm>
        </>
      )}
    />
  );
}

/** Starts a device of the user's own from a preset. */
export function NewDeviceDialog({ open, ...props }: NewDeviceDialogProps) {
  return open ? <NewDeviceForm {...props} /> : null;
}
