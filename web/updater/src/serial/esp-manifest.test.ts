import { describe, expect, it } from 'vitest';
import { checkPartsChip, imageChipId, parseManifest, planInstall, resolveParts, selectBuild } from './esp-manifest.ts';

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
    { chipFamily: 'ESP32-S2', parts: [{ path: 'esp32s2.bin', offset: 0 }] },
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
    expect(selectBuild(result.manifest, 'ESP32-S2')?.parts).toHaveLength(1);
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

  /**
   * @param chipId - The header's chip_id.
   * @param at - Where the header goes.
   * @param size - The image size.
   * @returns An image: 0xFF, with an ESP image header at `at`.
   */
  const image = (chipId: number, at = 0, size = 0x2000) => {
    const data = new Uint8Array(size).fill(0xff);
    data.set([0xe9, 3, 2, 0x20, 0, 0, 0, 0, 0xee, 0, 0, 0, chipId & 0xff, chipId >> 8], at);
    return data;
  };

  it('reads the chip a merged image or a part is built for', () => {
    expect(imageChipId({ name: 's3.bin', address: 0, data: image(9) })).toBe(9);
    expect(imageChipId({ name: 'esp32.bin', address: 0, data: image(0, 0x1000) })).toBe(0);
    expect(imageChipId({ name: 'c5.bin', address: 0, data: image(23, 0x2000, 0x3000) })).toBe(23);
    expect(imageChipId({ name: 'app.bin', address: 0x10000, data: image(2) })).toBe(2);
    // Only a part at 0x0 is looked into further; a partition table or OTA data has no header.
    expect(imageChipId({ name: 'x.bin', address: 0x10000, data: image(9, 0x1000) })).toBeNull();
    expect(
      imageChipId({ name: 'partitions.bin', address: 0x8000, data: new Uint8Array([0xaa, 0x50, 1, 2]) }),
    ).toBeNull();
    expect(imageChipId({ name: 'short.bin', address: 0, data: new Uint8Array([0xe9, 1]) })).toBeNull();
  });

  it('refuses a build for another chip before anything is written', () => {
    const s3Merged = { name: 'diy-s3-factory.bin', address: 0, data: image(9) };
    expect(checkPartsChip([s3Merged], 0, 'ESP32')).toBe(
      'diy-s3-factory.bin is built for an ESP32-S3, but this chip is an ESP32. Nothing was written: pick the ESP32 build.',
    );
    expect(checkPartsChip([s3Merged], 9, 'ESP32-S3')).toBeNull();
    expect(checkPartsChip([{ name: 'esp32-factory.bin', address: 0, data: image(0, 0x1000) }], 9, 'ESP32-S3')).toMatch(
      /built for an ESP32, but this chip is an ESP32-S3/,
    );
    // A manifest's parts: the header-less ones are skipped, a mislabelled app is caught.
    const parts = [
      { name: 'bootloader.bin', address: 0, data: image(9) },
      { name: 'partitions.bin', address: 0x8000, data: new Uint8Array([0xaa, 0x50]) },
      { name: 'firmware.bin', address: 0x10000, data: image(2) },
    ];
    expect(checkPartsChip(parts, 9, 'ESP32-S3')).toMatch(/^firmware\.bin is built for an ESP32-S2/);
    expect(checkPartsChip([{ name: 'odd.bin', address: 0, data: image(99) }], 9, 'ESP32-S3')).toMatch(/chip ID 99/);
    // Not an ESP image at all, or a chip without IMAGE_CHIP_ID (ESP8266): left to esptool.
    expect(checkPartsChip([{ name: 'x.bin', address: 0, data: bytes('x') }], 0, 'ESP32')).toBeNull();
    expect(checkPartsChip([s3Merged], undefined, 'ESP8266')).toBeNull();
  });
});
