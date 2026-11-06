import { describe, expect, it } from 'vitest';
import { nodeNameOfImage } from '../device/picked-image.ts';
import type { BuilderDevice } from './api.ts';
import { ConfigMatch, configMatch, devicesWithNodeName, formatConfigHash } from './node-match.ts';

/**
 * @param id - `presets/x.yaml` or `mine/x.yaml`.
 * @param nodeName - Its `name:`.
 * @param hashes - Its config hash now and at its last build.
 * @returns A listed device file.
 */
function device(
  id: string,
  nodeName: string,
  hashes: { now: number | null; built: number | null } = { now: null, built: null },
): BuilderDevice {
  const [root, file] = id.split('/') as ['presets' | 'mine', string];
  return {
    id,
    root,
    file,
    name: file,
    nodeName,
    platform: null,
    description: '',
    images: [],
    isStale: false,
    writable: root === 'mine',
    configHash: hashes.now,
    builtConfigHash: hashes.built,
  };
}

describe('configMatch', () => {
  const file = device('mine/keys.yaml', 'keys', { now: 0x1234, built: 0x99 });

  it('compares the running hash with the file now, then with its last build', () => {
    expect(configMatch(file, 0x1234)).toBe(ConfigMatch.Same);
    expect(configMatch(file, 0x99)).toBe(ConfigMatch.LastBuild);
    expect(configMatch(file, 0x5)).toBe(ConfigMatch.Different);
  });

  it('says nothing when either side has no hash', () => {
    expect(configMatch(file, null)).toBe(ConfigMatch.Unknown);
    expect(configMatch(device('mine/old.yaml', 'keys'), 0x1234)).toBe(ConfigMatch.Unknown);
  });

  it('formats a hash as the builder prints it', () => {
    expect(formatConfigHash(0x9754273)).toBe('09754273');
  });
});

describe('devicesWithNodeName', () => {
  const devices = [
    device('presets/keystation-mini32-mk3.yaml', 'keystation-mini32'),
    device('presets/esp32-example.yaml', 'esp32-example'),
    device('mine/keys.yaml', 'keystation-mini32'),
    device('mine/broken.yaml', ''),
  ];

  it('puts the user’s own files before the presets', () => {
    expect(devicesWithNodeName(devices, 'keystation-mini32').map((d) => d.id)).toEqual([
      'mine/keys.yaml',
      'presets/keystation-mini32-mk3.yaml',
    ]);
  });

  it('finds nothing without a node name', () => {
    expect(devicesWithNodeName(devices, null)).toEqual([]);
    expect(devicesWithNodeName(devices, 'other')).toEqual([]);
  });
});

describe('nodeNameOfImage', () => {
  it('reads <node>-<version> image names', () => {
    expect(nodeNameOfImage('keystation-mini32-0.1.5.gfw')).toBe('keystation-mini32');
    expect(nodeNameOfImage('keys-1.2.3-factory.bin')).toBe('keys');
    expect(nodeNameOfImage('keys-1.2.3.bin')).toBe('keys');
  });

  it('says nothing for names without a version', () => {
    expect(nodeNameOfImage('keystation-mini32.gfw')).toBeNull();
    expect(nodeNameOfImage('Firmware 0.1.5.gfw')).toBeNull();
    expect(nodeNameOfImage('FirmwareFile.hex.txt')).toBeNull();
  });
});
