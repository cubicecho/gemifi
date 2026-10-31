import { describe, expect, it } from 'vitest';
import { parseManifest, planInstall, resolveParts, selectBuild } from './esp-manifest.ts';

const MANIFEST = {
  name: 'gemifi faders',
  version: '1.2.3',
  builds: [
    {
      chipFamily: 'ESP32-S3',
      parts: [
        { path: 'build/bootloader.bin', offset: 0 },
        { path: 'partitions.bin', offset: 0x8000 },
        { path: 'firmware.bin', offset: '0x10000' },
      ],
    },
    { chipFamily: 'ESP8266', parts: [{ path: 'esp8266.bin', offset: 0 }] },
  ],
};

const bytes = (text: string) => new TextEncoder().encode(text);

describe('ESP manifests', () => {
  it('parses the ESP Web Tools format, with hex string offsets', () => {
    const result = parseManifest(JSON.stringify(MANIFEST));
    expect(result.ok).toBe(true);
    if (result.ok) {
      expect(result.manifest.builds[0]?.parts.map((p) => p.offset)).toEqual([0, 0x8000, 0x10000]);
      expect(result.manifest.name).toBe('gemifi faders');
    }
  });

  it('refuses what is not a manifest', () => {
    expect(parseManifest('{').ok).toBe(false);
    expect(parseManifest('{"name":"x"}').ok).toBe(false);
    expect(parseManifest('{"builds":[]}').ok).toBe(false);
    expect(parseManifest('{"builds":[{"chipFamily":"ESP32","parts":[{"path":"a.bin"}]}]}').ok).toBe(false);
    expect(parseManifest('{"builds":[{"chipFamily":"ESP32","parts":[{"path":"a.bin","offset":-1}]}]}').ok).toBe(false);
  });

  it('picks the build for the chip esptool found', () => {
    const result = parseManifest(JSON.stringify(MANIFEST));
    if (result.ok === false) {
      throw new Error(result.problem);
    }
    expect(selectBuild(result.manifest, 'ESP32-S3')?.parts).toHaveLength(3);
    expect(selectBuild(result.manifest, 'esp32s3')?.chipFamily).toBe('ESP32-S3');
    expect(selectBuild(result.manifest, 'ESP8266')?.parts).toHaveLength(1);
    expect(selectBuild(result.manifest, 'ESP32')).toBeNull();
  });

  it('matches parts to picked files by name and lists what is missing', () => {
    const result = parseManifest(JSON.stringify(MANIFEST));
    if (result.ok === false) {
      throw new Error(result.problem);
    }
    const build = result.manifest.builds[0];
    if (build === undefined) {
      throw new Error('no build');
    }
    const files = [
      { name: 'firmware.bin', bytes: bytes('app') },
      { name: 'bootloader.bin', bytes: bytes('boot') },
    ];
    expect(resolveParts(build, files)).toEqual({ ok: false, missing: ['partitions.bin'] });
    const all = resolveParts(build, [...files, { name: 'partitions.bin', bytes: bytes('pt') }]);
    expect(all.ok && all.parts.map((p) => [p.name, p.address])).toEqual([
      ['bootloader.bin', 0],
      ['partitions.bin', 0x8000],
      ['firmware.bin', 0x10000],
    ]);
  });

  it('plans a merged image at 0x0, a manifest with its files, or says what is wrong', () => {
    const merged = planInstall([{ name: 'factory.bin', bytes: bytes('x') }]);
    expect(merged.kind === 'merged' && merged.part.address).toBe(0);
    const withManifest = planInstall([
      { name: 'manifest.json', bytes: bytes(JSON.stringify(MANIFEST)) },
      { name: 'firmware.bin', bytes: bytes('app') },
    ]);
    expect(withManifest.kind).toBe('manifest');
    expect(withManifest.kind === 'manifest' && withManifest.files.map((f) => f.name)).toEqual(['firmware.bin']);
    expect(planInstall([]).kind).toBe('invalid');
    expect(
      planInstall([
        { name: 'a.bin', bytes: bytes('a') },
        { name: 'b.bin', bytes: bytes('b') },
      ]).kind,
    ).toBe('invalid');
    expect(planInstall([{ name: 'manifest.json', bytes: bytes('nope') }]).kind).toBe('invalid');
    expect(planInstall([{ name: 'empty.bin', bytes: new Uint8Array(0) }]).kind).toBe('invalid');
  });
});
