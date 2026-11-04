/**
 * The Device YAML examples beside the editor, from `yaml-reference.json`. The builder's tests
 * (TestYamlReference) validate each example and check the settings it names against the builder's
 * PARAMS table, so this file and the builder can't drift apart unnoticed.
 */

import reference from './yaml-reference.json' with { type: 'json' };

/** One feature's example. */
export interface YamlReferenceEntry {
  id: string;
  title: string;
  /** One or two sentences on what it is. */
  summary: string;
  /** What to know before using it. */
  notes: string[];
  /** The live settings (schema param names) it adds or uses, shown in the Config card. */
  params: string[];
  /** A YAML fragment that validates on an stm32f1 device. */
  yaml: string;
}

/** Every example, in the order the JSON lists them. */
export const YAML_REFERENCE: readonly YamlReferenceEntry[] = Object.freeze(reference.entries);
