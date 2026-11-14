/**
 * First install on an ESP chip through its ROM bootloader, with esptool-js over Web Serial (the
 * way ESPHome and WLED install). After this the app takes updates itself over SysEx (A/B slots),
 * so this is only for the first flash and for recovery. esptool-js is loaded when an install
 * starts, so the rest of the page does not carry it.
 */

import {
  checkPartsChip,
  type FlashPart,
  type PickedBytes,
  planInstall,
  resolveParts,
  selectBuild,
} from './esp-manifest.ts';

/** The ROM bootloader's own speed; esptool switches to {@link InstallOptions.baudRate} once its stub runs. */
const ROM_BAUD = 115_200;

/** The default flashing speed: works with every common USB-UART bridge. */
export const DEFAULT_INSTALL_BAUD = 460_800;
/** The fastest offered; some bridges and cables fail at it. */
const FAST_INSTALL_BAUD = 921_600;
/** Flashing speeds offered. */
export const INSTALL_BAUD_RATES = [ROM_BAUD, DEFAULT_INSTALL_BAUD, FAST_INSTALL_BAUD] as const;

/** Where an install is. */
export type InstallStage = 'connecting' | 'erasing' | 'writing' | 'resetting' | 'done';

/** A progress report. */
export interface InstallProgress {
  stage: InstallStage;
  /** Which part is being written, from 0. */
  partIndex: number;
  partCount: number;
  /** Bytes of this part written. */
  written: number;
  /** Bytes of this part. */
  total: number;
}

/** What {@link installFirmware} needs. */
export interface InstallOptions {
  /** A port the browser handed over, not open. */
  port: SerialPort;
  /** The picked files: one merged `.bin`, or a `manifest.json` and its parts. */
  files: readonly PickedBytes[];
  baudRate: number;
  /** Erase the whole flash first (clears old settings and partitions). */
  eraseAll: boolean;
  onProgress: (progress: InstallProgress) => void;
  /** esptool's own log lines. */
  onLog: (line: string) => void;
}

/** What was installed. */
export interface InstallResult {
  /** esptool's description, e.g. `ESP32-S3 (QFN56) (revision v0.2)`. */
  chip: string;
  parts: FlashPart[];
}

/**
 * Connects to the ROM bootloader, finds the chip, writes the parts and hard-resets it.
 *
 * @param options - The port, the files, the speed and the callbacks.
 * @returns The chip and what was written.
 */
export async function installFirmware(options: InstallOptions): Promise<InstallResult> {
  const plan = planInstall(options.files);
  if (plan.kind === 'invalid') {
    throw new Error(plan.problem);
  }
  const { ESPLoader, Transport } = await import('esptool-js');
  const transport = new Transport(options.port, false);
  let line = '';
  const terminal = {
    clean: () => undefined,
    write: (data: string) => {
      line += data;
    },
    writeLine: (data: string) => {
      options.onLog(line + data);
      line = '';
    },
  };
  const report = (stage: InstallStage, partIndex = 0, partCount = 0, written = 0, total = 0) =>
    options.onProgress({ stage, partIndex, partCount, written, total });

  try {
    report('connecting');
    const loader = new ESPLoader({ transport, baudrate: options.baudRate, romBaudrate: ROM_BAUD, terminal });
    const chip = await loader.main();
    const family = loader.chip.CHIP_NAME;
    let parts: FlashPart[];
    if (plan.kind === 'merged') {
      parts = [plan.part];
    } else {
      const build = selectBuild(plan.manifest, family);
      if (build === null) {
        const offered = plan.manifest.builds.map((b) => b.chipFamily).join(', ');
        throw new Error(`The chip is an ${family}, and the manifest only has builds for ${offered}.`);
      }
      const resolved = resolveParts(build, plan.files);
      if (resolved.ok === false) {
        throw new Error(`Pick these files with the manifest too: ${resolved.missing.join(', ')}.`);
      }
      parts = resolved.parts;
    }
    // Before anything is erased: a build for another chip would only fail once it boots.
    const wrongChip = checkPartsChip(parts, loader.chip.IMAGE_CHIP_ID, family);
    if (wrongChip !== null) {
      throw new Error(wrongChip);
    }
    if (options.eraseAll) {
      report('erasing');
      await loader.eraseFlash();
    }
    report('writing', 0, parts.length, 0, parts[0]?.data.length ?? 0);
    await loader.writeFlash({
      fileArray: parts.map((part) => ({ data: part.data, address: part.address })),
      flashMode: 'keep',
      flashFreq: 'keep',
      flashSize: 'keep',
      eraseAll: false,
      compress: true,
      reportProgress: (fileIndex, written, total) => report('writing', fileIndex, parts.length, written, total),
    });
    report('resetting', parts.length, parts.length);
    await loader.after('hard_reset');
    report('done', parts.length, parts.length);
    return { chip, parts };
  } finally {
    await transport.disconnect().catch(() => undefined);
  }
}
