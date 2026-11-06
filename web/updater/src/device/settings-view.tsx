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
import { RefreshCw } from '@/components/ui/icons';
import { Input } from '@/components/ui/input';
import { Switch } from '@/components/ui/switch';
import { Table, TableBody, TableCaption, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table';
import { type ParamValues, valueKey } from '../protocol/config.ts';
import type { HelloInfo } from '../protocol/messages.ts';
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
import { LearnCard } from './learn-card.tsx';
import type { LearnSource } from './learn-sources.ts';
import { PresetsCard } from './presets-card.tsx';
import type { ConfigController, ImportReport } from './use-config.ts';
import type { DeviceMap } from './use-device-map.ts';
import { LearnRowsContext, type MidiLearn, useLearnRows } from './use-midi-learn.ts';
import type { PresetsController } from './use-presets.ts';

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
  const note = isReadOnly(param) ? ', read-only' : isVolatile(param) ? ', live only' : '';
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
  const learn = useLearnRows();
  const rows = Array.from({ length: table.count }, (_, index) => ({
    index,
    rowLabel: table.indexLabels?.[index] ?? String(index + 1),
    rowKey: `row-${index}`,
    // "Find a control" finds the row by any of its params (use-midi-learn.ts).
    refs: table.params.map((param) => valueKey(param.id, index)).join(' '),
    isFound:
      learn.found !== null && learn.found.index === index && table.params.some((param) => param.id === learn.found?.id),
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
          {rows.map(({ index, rowLabel, rowKey, refs, isFound }) => (
            <TableRow
              key={rowKey}
              data-param-row={refs}
              className={isFound ? 'bg-selection/60 transition-colors' : 'transition-colors'}
            >
              <TableHead className="whitespace-nowrap">{rowLabel}</TableHead>
              {table.params.map((param) => {
                const key = valueKey(param.id, index);
                const value = values.get(key) ?? param.defaults[index] ?? 0;
                const isTarget = learn.target?.id === param.id && learn.target.index === index;
                return (
                  <TableCell
                    key={param.id}
                    className={isTarget ? 'min-w-32 rounded-md align-top ring-2 ring-primary' : 'min-w-32 align-top'}
                    onFocusCapture={() => learn.onFocusField(param, index)}
                  >
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
 * One group as a card: its plain params in a grid, its tables full width, in schema order.
 *
 * @param props - The section, the values and the controller.
 * @returns The card.
 */
function GroupCard({
  section,
  values,
  config,
}: {
  section: SchemaSection;
  values: ParamValues;
  config: ConfigController;
}) {
  const blocks: ReactElement[] = [];
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
  return <CardLayout title={section.title} contentClassName="gap-4" contentSlot={blocks} />;
}

/** The preset params the Presets card stands for: the active slot, its dirty flag and the count in use (§4.5). */
const PRESETS_CARD_PARAMS: ReadonlySet<number> = new Set([PresetParam.Active, PresetParam.Dirty, PresetParam.Count]);

/**
 * @param item - A section's param or table.
 * @param keep - Which params to keep.
 * @returns The item with only those params, or null when none are left.
 */
function filterItem(item: SchemaItem, keep: (param: ParamSpec) => boolean): SchemaItem | null {
  if (item.kind === 'param') {
    return keep(item.param) ? item : null;
  }
  const params = item.params.filter(keep);
  return params.length === 0 ? null : { ...item, params };
}

/**
 * @param sections - From `layoutSchema`.
 * @param keep - Which params to keep.
 * @returns The sections with only those params, empty ones left out.
 */
function filterSections(sections: SchemaSection[], keep: (param: ParamSpec) => boolean): SchemaSection[] {
  return sections
    .map((section) => ({
      ...section,
      items: section.items.flatMap((item) => {
        const kept = filterItem(item, keep);
        return kept === null ? [] : [kept];
      }),
    }))
    .filter((section) => section.items.length > 0);
}

/**
 * Splits the schema's groups by scope, on a device with presets: what each preset has its own of,
 * and what is the same in every preset. The params the Presets card stands for are left out.
 *
 * @param sections - From `layoutSchema`.
 * @returns The two lists of groups.
 */
export function splitByScope(sections: SchemaSection[]): { perPreset: SchemaSection[]; shared: SchemaSection[] } {
  const shown = (param: ParamSpec) => PRESETS_CARD_PARAMS.has(param.id) === false;
  // What is left of the presets group (the Program Change sent on switching) is named for what it
  // does, so it does not read as a second Presets card.
  const perPreset = filterSections(sections, (param) => shown(param) && isPreset(param)).map((section) =>
    section.items.some((item) => item.kind === 'param' && item.param.id === PresetParam.Program)
      ? { ...section, title: 'On switching to this preset' }
      : section,
  );
  return { perPreset, shared: filterSections(sections, (param) => shown(param) && isPreset(param) === false) };
}

/**
 * @param presets - The presets controller.
 * @returns The active preset's name, once the list is read.
 */
function activeName(presets: PresetsController): string | null {
  const { state } = presets;
  if (state.kind !== 'ready') {
    return null;
  }
  return state.presets.find((info) => info.slot === state.active)?.name ?? null;
}

/**
 * @param config - The config controller.
 * @param presets - The presets controller.
 * @returns Whether anything set on the device is not stored yet.
 */
export function hasUnsavedSettings(config: ConfigController, presets: PresetsController): boolean {
  return config.isDirty || (presets.state.kind === 'ready' && presets.state.dirty);
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
          report.isStored === true ? (
            'Every preset from the backup is stored on the device.'
          ) : (
            'Save to device to keep them after a power cycle.'
          )
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

/** MIDI learn for {@link SettingsView}: the controller, the layout to draw and the inputs. */
export interface SettingsLearning {
  learn: MidiLearn;
  map: DeviceMap;
  sources: readonly LearnSource[];
  /** The device sends EVENTs (caps bit 3). */
  hasEvents: boolean;
}

/** Rows with nothing found or targeted: no MIDI learn. */
const NO_LEARN_ROWS = { found: null, target: null, onFocusField: () => undefined };

/** Props of {@link SettingsView} and {@link SettingsSaveBar}. */
export interface SettingsProps {
  config: ConfigController;
  presets: PresetsController;
  /** HELLO from the device, which offers config. */
  device: HelloInfo;
}

/**
 * Copies of the settings to and from a file, and the factory reset.
 *
 * @param props - The controllers and the device.
 * @returns The card.
 */
function BackupCard({ config, presets, device }: SettingsProps) {
  const { state, action } = config;
  const isReady = state.kind === 'ready';
  const isBusy = action !== null || presets.action !== null;
  const hasPresets = presets.state.kind !== 'off';
  return (
    <CardLayout
      title="Backup and reset"
      description={
        hasPresets
          ? 'Export saves the settings as they are now (the active preset and the shared ones) to a JSON file; Import sets them from one. Import also takes a backup of every preset (the one saved before a firmware upload, or gemifi.py backup) and puts every preset back.'
          : 'Export saves the settings as they are now to a JSON file; Import sets them from one.'
      }
      contentSlot={config.importReport === null ? null : <ImportSummary report={config.importReport} />}
      footerSlot={
        <ConfirmButton
          content="Reset to defaults"
          disabled={isReady === false || isBusy}
          loading={action === 'reset'}
          loadingLabel="Resetting…"
          title="Reset every setting to its default?"
          description={
            hasPresets
              ? 'Every value on the device goes back to its firmware default, and every stored preset and preset name is erased. Export first to keep a copy of the active preset.'
              : 'Every value on the device goes back to its firmware default and the stored settings are erased. Export first to keep a copy.'
          }
          confirmLabel="Reset to defaults"
          onConfirm={() => {
            config.reset();
            presets.reload();
          }}
        />
      }
      footerActionsSlot={
        <>
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
        </>
      }
    />
  );
}

/**
 * The device's presets and settings, drawn from the schema it sends: the Presets card, a card per
 * group (on a device with presets, first what the active preset holds, then what every preset
 * shares), and backup and reset. Every edit takes effect at once; {@link SettingsSaveBar} stores them.
 *
 * @param props - The controllers and the device.
 * @returns The view's blocks.
 */
export function SettingsView({ config, presets, device, learning }: SettingsProps & { learning?: SettingsLearning }) {
  const { state } = config;
  const hasPresets = presets.state.kind !== 'off';
  const isBusy = config.action !== null || presets.action !== null;
  const reloadButton = (
    <Button
      size="sm"
      variant="outline"
      iconSlot={<RefreshCw />}
      content="Read again"
      disabled={isBusy}
      onClick={() => {
        config.reload();
        presets.reload();
      }}
    />
  );
  let groups: ReactElement | null = null;
  if (state.kind === 'loading') {
    groups = <CardLayout title="Settings" loading contentSlot={null} />;
  } else if (state.kind === 'error') {
    groups = (
      <Alert
        variant="destructive"
        title="The settings could not be read"
        description={state.message}
        actionSlot={reloadButton}
      />
    );
  } else if (state.kind === 'ready') {
    const sections = layoutSchema(state.schema);
    const cards = (list: SchemaSection[]) =>
      list.map((section) => <GroupCard key={section.title} section={section} values={state.values} config={config} />);
    if (hasPresets) {
      const { perPreset, shared } = splitByScope(sections);
      const name = activeName(presets);
      groups = (
        <>
          {perPreset.length === 0 ? null : (
            <Section
              title={name === null ? 'In this preset' : `In "${name}"`}
              description="Each preset has its own. Switching presets changes these."
              actionSlot={reloadButton}
              contentClassName="gap-4"
              contentSlot={cards(perPreset)}
            />
          )}
          {shared.length === 0 ? null : (
            <Section
              title="Same in every preset"
              description="Device-wide: switching presets leaves these alone."
              contentClassName="gap-4"
              contentSlot={cards(shared)}
            />
          )}
        </>
      );
    } else {
      groups = (
        <Section title="Settings" actionSlot={reloadButton} contentClassName="gap-4" contentSlot={cards(sections)} />
      );
    }
  }
  return (
    <LearnRowsContext.Provider value={learning?.learn.rows ?? NO_LEARN_ROWS}>
      <PresetsCard controller={presets} disabled={config.action !== null} />
      {learning === undefined || state.kind !== 'ready' ? null : (
        <LearnCard
          learn={learning.learn}
          schema={state.schema}
          map={learning.map}
          sources={learning.sources}
          hasEvents={learning.hasEvents}
        />
      )}
      {groups}
      <BackupCard config={config} presets={presets} device={device} />
    </LearnRowsContext.Provider>
  );
}

/**
 * The bar pinned under the settings: whether everything is stored, and Save to device (COMMIT).
 *
 * @param props - The controllers and the device.
 * @returns The bar.
 */
export function SettingsSaveBar({ config, presets }: SettingsProps) {
  const { state, action } = config;
  const isReady = state.kind === 'ready';
  const isBusy = action !== null || presets.action !== null;
  const hasUnsaved = hasUnsavedSettings(config, presets);
  const name = activeName(presets);
  return (
    <div className="flex flex-col gap-2">
      {config.actionError === null ? null : (
        <Alert variant="destructive" title="That did not work" description={config.actionError} />
      )}
      <div className="flex flex-wrap items-center justify-between gap-2">
        {hasUnsaved ? (
          <Badge variant="warning-soft">{name === null ? 'Unsaved changes' : `Unsaved changes in "${name}"`}</Badge>
        ) : (
          <span className="text-foreground/60 text-sm">
            Changes take effect as you make them. Save to device keeps them after a power cycle.
          </span>
        )}
        <Button
          variant={hasUnsaved ? 'positive' : 'outline'}
          content="Save to device"
          disabled={isReady === false || (isBusy && action !== 'save')}
          loading={action === 'save'}
          loadingLabel="Saving…"
          onClick={config.save}
        />
      </div>
    </div>
  );
}
