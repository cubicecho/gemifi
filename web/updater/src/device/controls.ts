/**
 * The device's controls by kind and index, as the schema, GET_COMPONENT and EVENT name them: the
 * names the drawing and the tester show, and the settings row an EVENT belongs to.
 */

import type { Component } from '../protocol/feedback.ts';
import type { Schema } from '../protocol/schema.ts';
import { ComponentKind, EventType } from '../protocol/wire.ts';

/** Params whose index labels name each kind's controls, base controls first, then layer twins. */
const NAMING_PARAMS: Readonly<Record<number, { own: string; twins?: string }>> = {
  [ComponentKind.Button]: { own: 'button.action', twins: 'layer.button.action' },
  [ComponentKind.Analog]: { own: 'analog.cc' },
  [ComponentKind.Encoder]: { own: 'encoder.cc', twins: 'layer.encoder.cc' },
  [ComponentKind.Pad]: { own: 'pad.note' },
};

/** A value of the settings editor: a param and an index. */
export interface ParamRef {
  id: number;
  index: number;
}

/**
 * @param schema - The device's schema.
 * @param kind - A component kind.
 * @returns How many controls of that kind the schema has rows for (base controls, not twins).
 */
export function ownCount(schema: Schema, kind: number): number {
  const naming = NAMING_PARAMS[kind];
  return naming === undefined ? 0 : (schema.params.find((param) => param.name === naming.own)?.count ?? 0);
}

/**
 * The settings row an EVENT's control is edited on: `button.action` row i for button i, its
 * layer twins' `layer.button.action` rows after the base buttons, `analog.cc`, `encoder.cc` and
 * `pad.note` for the others. Keys have no row.
 *
 * @param schema - The device's schema.
 * @param type - The EVENT type.
 * @param index - The EVENT index.
 * @returns The first param of the row and the index, or null.
 */
export function rowOfEvent(schema: Schema, type: number, index: number): ParamRef | null {
  const kind = type === EventType.Pad ? ComponentKind.Pad : type;
  const naming = NAMING_PARAMS[kind];
  if (naming === undefined) {
    return null;
  }
  const own = schema.params.find((param) => param.name === naming.own);
  if (own !== undefined && index < own.count) {
    return { id: own.id, index };
  }
  const twins = naming.twins === undefined ? undefined : schema.params.find((param) => param.name === naming.twins);
  const twinIndex = index - (own?.count ?? 0);
  if (twins !== undefined && twinIndex >= 0 && twinIndex < twins.count) {
    return { id: twins.id, index: twinIndex };
  }
  return null;
}

/** What one kind's controls are called. */
const KIND_WORDS: Readonly<Record<number, string>> = {
  [ComponentKind.Button]: 'Button',
  [ComponentKind.KeyMatrix]: 'Key',
  [ComponentKind.Analog]: 'Analog',
  [ComponentKind.Encoder]: 'Encoder',
  [ComponentKind.Display]: 'Display',
  [ComponentKind.Pad]: 'Pad',
};

/**
 * Names the controls: GET_COMPONENT's display names where the device gives them, else the schema's
 * index labels, else the kind and a number.
 *
 * @param schema - The device's schema, or null before it is read.
 * @param components - GET_COMPONENT's descriptions, or null.
 * @returns `(kind, index) => name`.
 */
export function controlNamer(
  schema: Schema | null,
  components: readonly Component[] | null,
): (kind: number, index: number) => string {
  const fromComponents = new Map<string, string>();
  for (const component of components ?? []) {
    if (component.name !== '') {
      fromComponents.set(`${component.kind}:${component.index}`, component.name);
    }
  }
  return (kind, index) => {
    const named = fromComponents.get(`${kind}:${index}`);
    if (named !== undefined) {
      return named;
    }
    const naming = NAMING_PARAMS[kind];
    const param = naming === undefined ? undefined : schema?.params.find((p) => p.name === naming.own);
    const label = param?.indexLabels?.[index];
    return label ?? `${KIND_WORDS[kind] ?? `Kind ${kind}`} ${index + 1}`;
  };
}

/**
 * @param components - GET_COMPONENT's descriptions, or null.
 * @param kind - A kind.
 * @param index - An index.
 * @returns That component, if the device described it.
 */
export function componentOf(
  components: readonly Component[] | null,
  kind: number,
  index: number,
): Component | undefined {
  return components?.find((component) => component.kind === kind && component.index === index);
}
