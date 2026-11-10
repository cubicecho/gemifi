import { type KeyboardEvent, useEffect, useRef, useState } from 'react';
import { TextareaField, useAppForm } from '@/components/app-form';
import { CardLayout } from '@/components/card-layout';
import { List } from '@/components/list';
import { ListItem } from '@/components/list-item';
import { Alert } from '@/components/ui/alert';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { FormElement } from '@/components/ui/form-element';
import { CircleAlert, CircleCheck, FilePen, TriangleAlert } from '@/components/ui/icons';
import { type BuilderDevice, type Diagnostic, saveDevice, type ValidationResult, validateDevice } from './api.ts';
import { deviceTitle } from './platforms.ts';
import { ValidationFacts } from './validation-facts.tsx';
import { INDENT, insertIndent, lineRange } from './yaml-text.ts';

/** Rows the YAML box shows before it scrolls. */
const EDITOR_ROWS = 22;
/** How far down the box a line jumped to sits: a third of the way, so the lines above it show. */
const JUMP_VIEW_FRACTION = 3;

/** Where a check is. */
type CheckState =
  | { kind: 'idle' }
  | { kind: 'checking' }
  | { kind: 'done'; result: ValidationResult }
  | { kind: 'failed'; message: string };

/** What a diagnostic's level looks like. */
const LEVEL_LOOKS = {
  error: { label: 'Error', variant: 'destructive-soft', iconSlot: <CircleAlert className="size-4 text-negative" /> },
  warning: { label: 'Warning', variant: 'warning-soft', iconSlot: <TriangleAlert className="size-4 text-warning" /> },
} as const satisfies Record<Diagnostic['level'], { label: string; variant: string; iconSlot: unknown }>;

/** How the builder words a DMA allocation clash (allocate_dma): "needs DMA1 channel 4 (USART1_TX), which …". */
const DMA_CLASH = /needs DMA1 channel \d+/;

/**
 * @param diag - A diagnostic.
 * @returns Whether it is a DMA channel clash, so it can say so at a glance.
 */
function isDmaClash(diag: Diagnostic): boolean {
  return diag.level === 'error' && DMA_CLASH.test(diag.msg);
}

/** Props of {@link DeviceEditorCard}. */
export interface DeviceEditorCardProps {
  device: BuilderDevice;
  /** The file as it was opened, which the form starts from. */
  savedText: string;
  /** Why the device cannot be built here, or null when it can. */
  buildBlocker: string | null;
  isBuilding: boolean;
  /** Called after a save, so the device list is asked again. */
  onSaved: () => void;
  /** Starts a build of the saved file. */
  onBuild: () => void;
  /** Handed the question "are there edits that are not saved?" while the form is open, and null after. */
  onUnsavedCheckChange: (check: (() => boolean) | null) => void;
  /** Opens the new-device form on this preset, for a device that cannot be written. */
  onCopyPreset: () => void;
}

/**
 * Selects a line in the box and scrolls it into view.
 *
 * @param box - The YAML textarea.
 * @param line - The 1-based line.
 */
function jumpToLine(box: HTMLTextAreaElement, line: number): void {
  const { start, end } = lineRange(box.value, line);
  box.focus();
  box.setSelectionRange(start, end);
  const lineHeight = Number.parseFloat(getComputedStyle(box).lineHeight);
  if (Number.isFinite(lineHeight)) {
    box.scrollTop = Math.max(0, (line - 1) * lineHeight - box.clientHeight / JUMP_VIEW_FRACTION);
  }
}

/**
 * @param diag - One finding.
 * @returns Where it is: `Line 12` in this file, `presets/x.yaml:12` in another, or null.
 */
function placeOf(diag: Diagnostic): string | null {
  if (diag.file !== null) {
    return diag.line === null ? diag.file : `${diag.file}:${diag.line}`;
  }
  return diag.line === null ? null : `Line ${diag.line}`;
}

/**
 * The validator's findings, each a button that jumps to its line.
 *
 * @param props - The check's result and the jump.
 * @returns The summary and the list.
 */
function CheckResult({ result, onJump }: { result: ValidationResult; onJump: (line: number) => void }) {
  const errors = result.diags.filter((diag) => diag.level === 'error').length;
  const warnings = result.diags.length - errors;
  const counts = [
    errors > 0 ? `${errors} error${errors === 1 ? '' : 's'}` : null,
    warnings > 0 ? `${warnings} warning${warnings === 1 ? '' : 's'}` : null,
  ].filter((part) => part !== null);
  return (
    <>
      {result.ok ? (
        <Alert
          variant="info"
          iconSlot={<CircleCheck />}
          title={warnings > 0 ? `Valid, with ${counts.join(' and ')}` : 'Valid'}
          description={result.summary === '' ? undefined : result.summary}
        />
      ) : (
        <Alert
          variant="destructive"
          title={`Not valid: ${counts.join(' and ') || 'the builder gave no reason'}`}
          description="Choose a problem to go to its line."
        />
      )}
      {result.diags.length === 0 ? null : (
        <List
          label="Problems"
          contentSlot={result.diags.map((diag, index) => {
            const look = LEVEL_LOOKS[diag.level];
            const where = [placeOf(diag), diag.path === '' ? null : diag.path]
              .filter((part) => part !== null)
              .join(' · ');
            // A line in a package or an !include file is that file's: it is named, not jumped to.
            const line = diag.file === null ? diag.line : null;
            return (
              <ListItem
                // biome-ignore lint/suspicious/noArrayIndexKey: two diagnostics can say the same thing.
                key={index}
                leadingSlot={look.iconSlot}
                // An element, so the message wraps rather than being cut to one line.
                title={<span className="break-words font-medium text-sm">{diag.msg}</span>}
                description={
                  <span className="flex flex-wrap items-center gap-1.5">
                    <Badge variant={look.variant}>{look.label}</Badge>
                    {isDmaClash(diag) ? <Badge variant="outline">DMA channel</Badge> : null}
                    {where === '' ? null : <span className="font-mono text-foreground/60 text-xs">{where}</span>}
                  </span>
                }
                onClick={line === null ? undefined : () => onJump(line)}
              />
            );
          })}
        />
      )}
      {result.ok ? <ValidationFacts result={result} /> : null}
    </>
  );
}

/**
 * A device file's YAML in an explicit form: Save writes it, Validate checks it, and Save & build
 * saves it and starts a build. Nothing is saved without a press.
 */
export function DeviceEditorCard(props: DeviceEditorCardProps) {
  const { device, savedText, buildBlocker, isBuilding, onSaved, onBuild, onUnsavedCheckChange, onCopyPreset } = props;
  const box = useRef<HTMLTextAreaElement | null>(null);
  // After Escape, the next Tab leaves the box instead of indenting, so the keyboard is never trapped.
  const isTabReleased = useRef(false);
  const [check, setCheck] = useState<CheckState>({ kind: 'idle' });
  const [saveFailure, setSaveFailure] = useState<string | null>(null);
  const [isSaving, setIsSaving] = useState(false);
  // What the form counts as saved. The form re-reads `defaultValues` on every render, so after a
  // save it must already be the new text, or the form resets to the file as it was opened.
  const [baselineText, setBaselineText] = useState(savedText);

  const save = async (text: string): Promise<boolean> => {
    setSaveFailure(null);
    setIsSaving(true);
    try {
      await saveDevice(device.id, text);
    } catch (error) {
      setSaveFailure(error instanceof Error ? error.message : String(error));
      return false;
    } finally {
      setIsSaving(false);
    }
    setBaselineText(text);
    form.reset({ text });
    onSaved();
    return true;
  };
  const form = useAppForm({
    defaultValues: { text: baselineText },
    onSubmit: async ({ value }) => {
      await save(value.text);
    },
  });
  useEffect(() => {
    onUnsavedCheckChange(() => form.state.isDefaultValue === false);
    return () => onUnsavedCheckChange(null);
  }, [form, onUnsavedCheckChange]);

  const validate = () => {
    setCheck({ kind: 'checking' });
    validateDevice(form.state.values.text, device.id).then(
      (result) => setCheck({ kind: 'done', result }),
      (error: unknown) => setCheck({ kind: 'failed', message: error instanceof Error ? error.message : String(error) }),
    );
  };

  const saveAndBuild = async () => {
    const needsSave = device.writable && form.state.isDefaultValue === false;
    if (needsSave) {
      const isSaved = await save(form.state.values.text);
      if (isSaved === false) {
        return;
      }
    }
    onBuild();
  };

  const onKeyDown = (event: KeyboardEvent<HTMLTextAreaElement>) => {
    if (event.key === 'Escape') {
      isTabReleased.current = true;
      return;
    }
    const isPlainTab =
      event.key === 'Tab' && event.shiftKey === false && event.altKey === false && event.ctrlKey === false;
    const shouldIndent = isPlainTab && event.metaKey === false && isTabReleased.current === false;
    isTabReleased.current = false;
    if (shouldIndent === false || device.writable === false) {
      return;
    }
    event.preventDefault();
    const target = event.currentTarget;
    // `insertText` keeps the browser's undo history; where it is refused, edit the value directly.
    const isInserted = document.execCommand('insertText', false, INDENT);
    if (isInserted === false) {
      const next = insertIndent(target.value, { start: target.selectionStart, end: target.selectionEnd });
      form.setFieldValue('text', next.text);
      requestAnimationFrame(() => target.setSelectionRange(next.caret, next.caret));
    }
  };

  const title = deviceTitle(device);
  const isWritable = device.writable;
  const canBuild = buildBlocker === null && isBuilding === false && isSaving === false;
  return (
    <CardLayout
      title={title}
      iconSlot={<FilePen />}
      description={`${device.id}${isWritable ? '' : ' (read-only)'}`}
      contentSlot={
        <>
          {isWritable ? null : (
            <Alert
              variant="info"
              title="This preset is read-only"
              description="Start a device of your own from it to change the YAML. You can still validate and build it as it is."
              actionSlot={
                <Button variant="info" size="sm" content="New device from this preset" onClick={onCopyPreset} />
              }
            />
          )}
          <FormElement onSubmit={() => form.handleSubmit()}>
            <TextareaField
              form={form}
              name="text"
              label="Device YAML"
              description={
                isWritable
                  ? 'Tab indents two spaces; press Escape, then Tab, to move on. Nothing is saved until you press Save.'
                  : undefined
              }
              ref={box}
              rows={EDITOR_ROWS}
              readOnly={isWritable === false}
              spellCheck={false}
              autoCapitalize="none"
              autoCorrect="off"
              wrap="off"
              // `className` is the field's root, so the code face reaches the box through it.
              className="[&_textarea]:font-mono [&_textarea]:text-xs [&_textarea]:leading-5"
              onKeyDown={onKeyDown}
            />
          </FormElement>
          {saveFailure === null ? null : (
            <Alert
              variant="destructive"
              title="The file was not saved"
              description={`${saveFailure} Your edits are still here.`}
            />
          )}
          {check.kind === 'failed' ? (
            <Alert variant="destructive" title="The file could not be checked" description={check.message} />
          ) : null}
          {check.kind === 'done' ? (
            <CheckResult
              result={check.result}
              onJump={(line) => {
                if (box.current !== null) {
                  jumpToLine(box.current, line);
                }
              }}
            />
          ) : null}
        </>
      }
      footerSlot={
        <Button
          variant="outline"
          content="Validate"
          loading={check.kind === 'checking'}
          loadingLabel="Validating…"
          onClick={validate}
        />
      }
      footerActionsSlot={
        <>
          {isWritable ? (
            <form.AppForm>
              <form.SubmitButton variant="positive-outline" requireChanges />
            </form.AppForm>
          ) : null}
          <Button
            variant="positive"
            content={isWritable ? 'Save & build' : 'Build'}
            loading={isBuilding}
            loadingLabel="Building…"
            disabled={canBuild === false}
            hint={buildBlocker ?? undefined}
            onClick={() => void saveAndBuild()}
          />
        </>
      }
    />
  );
}
