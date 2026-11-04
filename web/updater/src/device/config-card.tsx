import { type ReactElement, useState } from 'react';
import { CardLayout } from '@/components/card-layout';
import { Combobox } from '@/components/combobox';
import { ConfirmButton } from '@/components/confirm-button';
import { type ControlProps, FormField } from '@/components/form-field';
import { Section } from '@/components/section';
import { Alert } from '@/components/ui/alert';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { DownloadButton } from '@/components/ui/download-button';
import { FilePickerButton } from '@/components/ui/file-picker';
import { RefreshCw, Settings } from '@/components/ui/icons';
import { Input } from '@/components/ui/input';
import { Switch } from '@/components/ui/switch';
import { Table, TableBody, TableCaption, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table';
import type { SysexClient } from '../protocol/client.ts';
import { type ParamValues, valueKey } from '../protocol/config.ts';
import type { HelloInfo } from '../protocol/messages.ts';
import { offersPresets } from '../protocol/presets.ts';
import {
  isPreset,
  isReadOnly,
  isVolatile,
  layoutSchema,
  noteName,
  type ParamSpec,
  type SchemaItem,
  type SchemaSection,
} from '../protocol/schema.ts';
import { ParamType, PresetParam } from '../protocol/wire.ts';
import { PresetControls } from './preset-controls.tsx';
import { type ConfigController, type ImportReport, useConfig } from './use-config.ts';
import { type PresetsController, usePresets } from './use-presets.ts';

/** Props of {@link ConfigCard}. */
export interface ConfigCardProps {
  client: SysexClient;
  /** HELLO from the device, which offers config. */
  device: HelloInfo;
}

/** What every control is handed. */
interface ControlBase {
  param: ParamSpec;
  index: number;
  value: number;
  /** FormField's wiring: id and aria props. */
  wired: ControlProps;
  /** The name, for a control in a table that has no visible label of its own. */
  ariaLabel: string | undefined;
  config: ConfigController;
}

/** A whole number, optionally signed, and nothing else. */
const INTEGER_PATTERN = /^-?\d+$/;

/**
 * @param text - What was typed.
 * @returns The integer, or null when the text is not one.
 */
function parseInteger(text: string): number | null {
  const trimmed = text.trim();
  return INTEGER_PATTERN.test(trimmed) ? Number(trimmed) : null;
}

/**
 * An int or note param: a number box that sends each whole number typed. It holds its own text,
 * so a half-typed value is not overwritten, and takes the device's value whenever that changes.
 *
 * @param props - The param, its value and the wiring.
 * @returns The input.
 */
function NumberControl({ param, index, value, wired, ariaLabel, config }: ControlBase) {
  const [draft, setDraft] = useState(String(value));
  const [shown, setShown] = useState(value);
  if (value !== shown) {
    setShown(value);
    setDraft(String(value));
  }
  const typed = parseInteger(draft);
  const note = param.type === ParamType.Note && typed !== null ? noteName(typed) : '';
  const suffix = [note, param.unit ?? ''].filter((part) => part !== '').join(' ');
  return (
    <Input
      {...wired}
      aria-label={ariaLabel}
      type="number"
      inputMode="numeric"
      signed={param.min < 0}
      min={param.min}
      max={param.max}
      step={1}
      value={draft}
      disabled={isReadOnly(param)}
      suffixText={suffix === '' ? undefined : suffix}
      onChangeText={(text) => {
        setDraft(text);
        const next = parseInteger(text);
        if (next === null) {
          config.setFieldError(param, index, `Enter a whole number from ${param.min} to ${param.max}.`);
        } else {
          config.setValue(param, index, next);
        }
      }}
    />
  );
}

/**
 * The control for one value, by the param's type.
 *
 * @param props - The param, its value and the wiring.
 * @returns The control.
 */
function ParamControl(props: ControlBase) {
  const { param, index, value, wired, ariaLabel, config } = props;
  if (param.type === ParamType.Bool) {
    return (
      <Switch
        {...wired}
        aria-label={ariaLabel}
        checked={value !== 0}
        disabled={isReadOnly(param)}
        onCheckedChange={(on) => config.setValue(param, index, on ? 1 : 0)}
      />
    );
  }
  if (param.type === ParamType.Enum) {
    const options = (param.options ?? []).map((label, i) => ({ value: String(param.min + i), label }));
    const isListed = options.some((option) => option.value === String(value));
    return (
      <Combobox
        {...wired}
        aria-label={ariaLabel}
        options={isListed ? options : [...options, { value: String(value), label: `${value} (not in the list)` }]}
        value={String(value)}
        disabled={isReadOnly(param)}
        onValueChange={(next) => config.setValue(param, index, Number(next))}
      />
    );
  }
  return <NumberControl {...props} />;
}

/**
 * @param param - A param.
 * @returns A line on what it is, if there is one to say.
 */
function paramNote(param: ParamSpec): string | undefined {
  if (isReadOnly(param)) {
    return 'Read-only.';
  }
  if (isVolatile(param)) {
    return 'Live only: not stored by Save to device.';
  }
  if (isPreset(param)) {
    return 'Per preset.';
  }
  return undefined;
}

/**
 * A plain param as a labelled field.
 *
 * @param props - The param, the values and the controller.
 * @returns The field.
 */
function ParamField({ param, values, config }: { param: ParamSpec; values: ParamValues; config: ConfigController }) {
  const key = valueKey(param.id, 0);
  const value = values.get(key) ?? param.defaults[0] ?? 0;
  return (
    <FormField
      label={param.label}
      orientation={param.type === ParamType.Bool ? 'horizontal' : 'vertical'}
      description={paramNote(param)}
      error={config.fieldErrors.get(key)}
      controlSlot={(wired) => (
        <ParamControl param={param} index={0} value={value} wired={wired} ariaLabel={undefined} config={config} />
      )}
    />
  );
}

/**
 * @param param - A column's param.
 * @returns Its heading: the label, the unit and whether it is stored.
 */
function columnTitle(param: ParamSpec): string {
  const unit = param.unit === null ? '' : ` (${param.unit})`;
  const note = isReadOnly(param)
    ? ', read-only'
    : isVolatile(param)
      ? ', live only'
      : isPreset(param)
        ? ', per preset'
        : '';
  return `${param.label}${unit}${note}`;
}

/**
 * Array params that share rows, as one table: a row per index, a column per param.
 *
 * @param props - The table item, the values and the controller.
 * @returns The table.
 */
function ParamTable({
  table,
  title,
  values,
  config,
}: {
  table: Extract<SchemaItem, { kind: 'table' }>;
  title: string;
  values: ParamValues;
  config: ConfigController;
}) {
  const rows = Array.from({ length: table.count }, (_, index) => ({
    index,
    rowLabel: table.indexLabels?.[index] ?? String(index + 1),
    rowKey: `row-${index}`,
  }));
  return (
    <div className="max-h-[32rem] overflow-y-auto">
      <Table>
        <TableCaption className="sr-only">{`${title}: ${table.params.map((param) => param.label).join(', ')}`}</TableCaption>
        <TableHeader>
          <TableRow>
            <TableHead>{table.indexLabels === null ? '#' : ''}</TableHead>
            {table.params.map((param) => (
              <TableHead key={param.id}>{columnTitle(param)}</TableHead>
            ))}
          </TableRow>
        </TableHeader>
        <TableBody>
          {rows.map(({ index, rowLabel, rowKey }) => (
            <TableRow key={rowKey}>
              <TableHead className="whitespace-nowrap">{rowLabel}</TableHead>
              {table.params.map((param) => {
                const key = valueKey(param.id, index);
                const value = values.get(key) ?? param.defaults[index] ?? 0;
                return (
                  <TableCell key={param.id} className="min-w-32 align-top">
                    <FormField
                      error={config.fieldErrors.get(key)}
                      controlSlot={(wired) => (
                        <ParamControl
                          param={param}
                          index={index}
                          value={value}
                          wired={wired}
                          ariaLabel={`${param.label}, ${rowLabel}`}
                          config={config}
                        />
                      )}
                    />
                  </TableCell>
                );
              })}
            </TableRow>
          ))}
        </TableBody>
      </Table>
    </div>
  );
}

/**
 * One group: its plain params in a grid, its tables full width, in schema order.
 *
 * @param props - The section, the values, the controller, and what goes first (the preset picker).
 * @returns The section.
 */
function GroupSection({
  section,
  values,
  config,
  leadSlot,
}: {
  section: SchemaSection;
  values: ParamValues;
  config: ConfigController;
  leadSlot?: ReactElement | undefined;
}) {
  const blocks: ReactElement[] = leadSlot === undefined ? [] : [leadSlot];
  let fields: ParamSpec[] = [];
  const flushFields = () => {
    if (fields.length > 0) {
      const first = fields[0]?.id ?? 0;
      blocks.push(
        <div key={`fields-${first}`} className="grid gap-4 sm:grid-cols-2">
          {fields.map((param) => (
            <ParamField key={param.id} param={param} values={values} config={config} />
          ))}
        </div>,
      );
      fields = [];
    }
  };
  for (const item of section.items) {
    if (item.kind === 'param') {
      fields.push(item.param);
      continue;
    }
    flushFields();
    blocks.push(
      <ParamTable
        key={`table-${item.params[0]?.id ?? 0}`}
        table={item}
        title={section.title}
        values={values}
        config={config}
      />,
    );
  }
  flushFields();
  return <Section title={section.title} level={3} contentClassName="gap-4" contentSlot={blocks} />;
}

/** The preset params the picker stands for: the active slot and its dirty flag (§4.5). */
const PICKER_PARAMS: ReadonlySet<number> = new Set([PresetParam.Active, PresetParam.Dirty]);

/**
 * @param section - A schema section.
 * @returns Whether it holds `preset.active`, so it is the presets group.
 */
function isPresetSection(section: SchemaSection): boolean {
  return section.items.some((item) => item.kind === 'param' && item.param.id === PresetParam.Active);
}

/**
 * The schema's sections, with the presets group first and the params the picker stands for left
 * out of it, when the device has presets.
 *
 * @param sections - From `layoutSchema`.
 * @param hasPresets - The device has presets.
 * @returns The sections to draw, and which one gets the picker.
 */
function arrangeSections(
  sections: SchemaSection[],
  hasPresets: boolean,
): Array<{ section: SchemaSection; isPresets: boolean }> {
  if (hasPresets === false) {
    return sections.map((section) => ({ section, isPresets: false }));
  }
  const presets = sections.filter(isPresetSection).map((section) => ({
    section: {
      ...section,
      items: section.items.filter((item) => item.kind !== 'param' || PICKER_PARAMS.has(item.param.id) === false),
    },
    isPresets: true,
  }));
  const rest = sections
    .filter((section) => isPresetSection(section) === false)
    .map((section) => ({ section, isPresets: false }));
  return [...presets, ...rest];
}

/**
 * @param presets - The presets controller.
 * @returns Whether the active preset has unsaved edits.
 */
function isPresetDirty(presets: PresetsController): boolean {
  return presets.state.kind === 'ready' && presets.state.dirty;
}

/**
 * What an import did, as an alert.
 *
 * @param props - The report.
 * @returns The alert.
 */
function ImportSummary({ report }: { report: ImportReport }) {
  const lines = [
    report.boardWarning,
    report.unknownNames.length > 0
      ? `Skipped names this device does not have: ${report.unknownNames.join(', ')}.`
      : null,
    report.readOnlyNames.length > 0 ? `Skipped read-only: ${report.readOnlyNames.join(', ')}.` : null,
    ...report.problems,
  ].filter((line): line is string => line !== null);
  const isClean = report.problems.length === 0;
  return (
    <Alert
      variant={isClean ? 'info' : 'warning'}
      title={`Imported ${report.applied} value${report.applied === 1 ? '' : 's'}${isClean ? '' : ', with problems'}`}
      description={
        lines.length === 0 ? (
          'Save to device to keep them after a power cycle.'
        ) : (
          <ul className="list-disc pl-4">
            {lines.map((line) => (
              <li key={line}>{line}</li>
            ))}
          </ul>
        )
      }
    />
  );
}

/**
 * The device's settings, drawn from the schema it sends: every edit is sent at once (live), and
 * Save to device stores them.
 *
 * @param props - The client and the device's HELLO.
 * @returns The card.
 */
export function ConfigCard({ client, device }: ConfigCardProps) {
  const config = useConfig(client, device);
  const hasPresets = offersPresets(device);
  const presets = usePresets(client, hasPresets, config.refreshValues);
  const { state, action } = config;
  const isReady = state.kind === 'ready';
  const isBusy = action !== null || presets.action !== null;
  const hasUnsaved = config.isDirty || isPresetDirty(presets);
  return (
    <CardLayout
      title="Config"
      iconSlot={<Settings />}
      description={
        hasPresets
          ? 'Changes take effect on the device as you make them. Save to device keeps them after a power cycle: the per-preset ones in the active preset.'
          : 'Changes take effect on the device as you make them. Save to device keeps them after a power cycle.'
      }
      actionSlot={
        <>
          {hasUnsaved ? <Badge variant="warning-soft">Unsaved changes</Badge> : null}
          <Button
            size="icon-sm"
            variant="outline"
            aria-label="Read the config again"
            iconSlot={<RefreshCw />}
            disabled={isBusy}
            onClick={() => {
              config.reload();
              presets.reload();
            }}
          />
        </>
      }
      loading={state.kind === 'loading'}
      contentSlot={
        <>
          {state.kind === 'error' ? (
            <Alert variant="destructive" title="The config could not be read" description={state.message} />
          ) : null}
          {state.kind === 'ready'
            ? arrangeSections(layoutSchema(state.schema), hasPresets).map(({ section, isPresets }) => (
                <GroupSection
                  key={section.title}
                  section={section}
                  values={state.values}
                  config={config}
                  leadSlot={
                    isPresets ? (
                      <PresetControls key="preset-controls" controller={presets} disabled={action !== null} />
                    ) : undefined
                  }
                />
              ))
            : null}
          {config.importReport === null ? null : <ImportSummary report={config.importReport} />}
          {config.actionError === null ? null : (
            <Alert variant="destructive" title="That did not work" description={config.actionError} />
          )}
        </>
      }
      footerActionsSlot={
        <>
          <ConfirmButton
            content="Reset to defaults"
            disabled={isReady === false || isBusy}
            loading={action === 'reset'}
            loadingLabel="Resetting…"
            title="Reset every setting to its default?"
            description={
              hasPresets
                ? 'Every value on the device goes back to its firmware default, and every stored preset and preset name is erased. Export JSON first to keep a copy of the active preset.'
                : 'Every value on the device goes back to its firmware default and the stored profile is erased. Export JSON first to keep a copy.'
            }
            confirmLabel="Reset"
            onConfirm={() => {
              config.reset();
              presets.reload();
            }}
          />
          <FilePickerButton
            variant="outline"
            size="sm"
            label="Import JSON"
            accept=".json,application/json"
            onPick={(text) => config.importText(text)}
          />
          <DownloadButton
            size="sm"
            label="Export JSON"
            filename={`gemifi-config-${device.boardId}.json`}
            mimeType="application/json"
            disabled={isReady === false}
            source={() => config.exportJson() ?? ''}
          />
          <Button
            variant="positive"
            content="Save to device"
            disabled={isReady === false || (isBusy && action !== 'save')}
            loading={action === 'save'}
            loadingLabel="Saving…"
            onClick={config.save}
          />
        </>
      }
    />
  );
}
