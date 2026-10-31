import { describe, expect, it } from 'vitest';
import { planInstall } from '../serial/esp-manifest.ts';
import { IMAGE_USES, ImageKind, imageKind } from './images.ts';

describe('built image kinds', () => {
  it('knows the NUC1xx files', () => {
    expect(imageKind('nuvoton-nuc1xx', 'keystation.gfw')).toBe(ImageKind.Gfw);
    expect(imageKind('nuvoton-nuc1xx', 'keystation.bin')).toBe(ImageKind.Raw);
  });

  it('knows the ESP32 files', () => {
    expect(imageKind('esp32s3', 'faders.bin')).toBe(ImageKind.OtaApp);
    expect(imageKind('esp32s3', 'faders-factory.bin')).toBe(ImageKind.Factory);
    expect(imageKind('esp32', 'faders-bootloader.bin')).toBe(ImageKind.Part);
    expect(imageKind('esp32s2', 'faders-partitions.bin')).toBe(ImageKind.Part);
  });

  it('uses the ESP8266 image for both the update and the first install', () => {
    const kind = imageKind('esp8266', 'pedal.bin');
    expect(kind).toBe(ImageKind.Esp8266App);
    expect(IMAGE_USES[ImageKind.Esp8266App]).toMatchObject({ canFlash: true, canInstall: true });
  });

  it('ignores files it does not know', () => {
    expect(imageKind('esp32', 'faders.elf')).toBeNull();
  });

  it('hands the first-install card one image it writes at 0x0', () => {
    const plan = planInstall([{ name: 'pedal.bin', bytes: new Uint8Array([0xe9, 0, 0, 0]) }]);
    expect(plan).toMatchObject({ kind: 'merged', part: { name: 'pedal.bin', address: 0 } });
  });
});
