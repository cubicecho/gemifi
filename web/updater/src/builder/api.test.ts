import { afterEach, describe, expect, it, vi } from 'vitest';
import {
  BuilderError,
  createDevice,
  fetchBuilderInfo,
  fetchImage,
  imageUrl,
  listDevices,
  listPresets,
  parseBuildExit,
  readDeviceFile,
  runBuild,
  saveDevice,
  validateDevice,
} from './api.ts';

const json = (body: unknown, status = 200) =>
  new Response(JSON.stringify(body), { status, headers: { 'Content-Type': 'application/json' } });

const DEVICE = {
  id: 'mine/keys.yaml',
  root: 'mine',
  file: 'keys.yaml',
  name: 'Keys',
  platform: 'esp32s3',
  description: 'Two octaves',
  images: ['keys.bin', 'keys-factory.bin'],
  writable: true,
};

/**
 * @param responses - What each call answers, in order.
 * @returns The mock, installed as `fetch`.
 */
function mockFetch(...responses: (Response | Error)[]) {
  const mock = vi.fn(async () => {
    const next = responses.shift();
    if (next === undefined || next instanceof Error) {
      throw next ?? new Error('no more responses');
    }
    return next;
  });
  vi.stubGlobal('fetch', mock);
  return mock;
}

afterEach(() => {
  vi.unstubAllGlobals();
});

describe('builder detection', () => {
  it('reads the info of a gemifi builder', async () => {
    mockFetch(
      json({ name: 'gemifi builder', version: '0.3', user_dir: '/data', toolchains: { 'nuvoton-nuc1xx': true } }),
    );
    expect(await fetchBuilderInfo()).toEqual({
      name: 'gemifi builder',
      version: '0.3',
      userDir: '/data',
      toolchains: { 'nuvoton-nuc1xx': true, stm32f1: false, esp: false },
    });
  });

  it('is null on a static host that 404s', async () => {
    mockFetch(new Response('Not found', { status: 404 }));
    expect(await fetchBuilderInfo()).toBeNull();
  });

  it('is null when the host answers with its index.html', async () => {
    mockFetch(new Response('<!doctype html><html></html>', { status: 200, headers: { 'Content-Type': 'text/html' } }));
    expect(await fetchBuilderInfo()).toBeNull();
  });

  it('is null for JSON from something else', async () => {
    mockFetch(json({ name: 'another app', user_dir: '/x' }));
    expect(await fetchBuilderInfo()).toBeNull();
  });

  it('is null when the network fails', async () => {
    mockFetch(new TypeError('Failed to fetch'));
    expect(await fetchBuilderInfo()).toBeNull();
  });
});

describe('builder requests', () => {
  it('lists devices', async () => {
    mockFetch(json({ devices: [DEVICE], user_dir: '/data' }));
    const listing = await listDevices();
    expect(listing.userDir).toBe('/data');
    expect(listing.devices).toEqual([{ ...DEVICE, nodeName: '', isStale: false }]);
  });

  it('reads the node name a device reports in HELLO', async () => {
    mockFetch(json({ devices: [{ ...DEVICE, node_name: 'keys' }] }));
    const listing = await listDevices();
    expect(listing.devices[0]?.nodeName).toBe('keys');
  });

  it('marks a device whose file changed since its build', async () => {
    mockFetch(json({ devices: [{ ...DEVICE, stale: true }] }));
    const listing = await listDevices();
    expect(listing.devices[0]?.isStale).toBe(true);
  });

  it('keeps a device whose platform it does not know, as null', async () => {
    mockFetch(json({ devices: [{ ...DEVICE, platform: '?' }] }));
    const listing = await listDevices();
    expect(listing.devices[0]?.platform).toBeNull();
  });

  it('refuses a device list with a missing field', async () => {
    const { writable: _, ...withoutWritable } = DEVICE;
    mockFetch(json({ devices: [withoutWritable] }));
    await expect(listDevices()).rejects.toBeInstanceOf(BuilderError);
  });

  it('lists presets', async () => {
    mockFetch(json({ presets: [{ name: 'keystation-mini-32', platform: 'nuvoton-nuc1xx', description: 'K' }] }));
    expect(await listPresets()).toEqual([{ name: 'keystation-mini-32', platform: 'nuvoton-nuc1xx', description: 'K' }]);
  });

  it('reads a file by its encoded id', async () => {
    const mock = mockFetch(json({ id: 'mine/keys.yaml', text: 'name: Keys\n' }));
    expect(await readDeviceFile('mine/keys.yaml')).toBe('name: Keys\n');
    expect(mock).toHaveBeenCalledWith('api/file?id=mine%2Fkeys.yaml', undefined);
  });

  it('posts JSON with its content type', async () => {
    const mock = mockFetch(json({ ok: true }));
    await saveDevice('mine/keys.yaml', 'x: 1\n');
    expect(mock).toHaveBeenCalledWith('api/save', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ id: 'mine/keys.yaml', text: 'x: 1\n' }),
    });
  });

  it('reads validation diagnostics, with a line of 0 or null as unplaced', async () => {
    mockFetch(
      json({
        ok: false,
        summary: '',
        diags: [
          { level: 'error', path: 'controls[0]', line: 7, msg: 'unknown pin' },
          { level: 'warning', path: '', line: 0, msg: 'no name' },
          { level: 'error', path: 'x', line: null, msg: 'bad' },
        ],
      }),
    );
    const result = await validateDevice('text', 'mine/keys.yaml');
    expect(result.ok).toBe(false);
    expect(result.diags.map((d) => d.line)).toEqual([7, null, null]);
    expect(result.diags[1]?.level).toBe('warning');
    expect(result.presets).toBeNull();
    expect(result.dma).toBeNull();
    expect(result.dinMidi).toBeNull();
    expect(result.midiPorts).toEqual([]);
  });

  it('reads the presets, the DMA map, DIN MIDI and the extra ports', async () => {
    mockFetch(
      json({
        ok: true,
        summary: 'Code 61 (stm32f1, board 0x0002), 3 hubs\n  DMA1 channels: 7 USART2_TX (rgb_serial), free: 1',
        diags: [],
        presets: {
          slots: [
            { slot: 1, name: 'Default', program: null, changes: 0, listed: true },
            { slot: 2, name: 'Preset 2', program: null, changes: 0, listed: false },
          ],
          revision: 0,
          given: true,
          storage: { max_slots: 3 },
        },
        dma: {
          channels: [{ channel: 7, request: 'USART2_TX', user: "rgb_serial 'pad_leds'" }, { channel: 'x' }],
          free: [1, 2, 3, 4, 5, 6],
          text: 'DMA1 channels: ...',
        },
        din_midi: {
          mode: 'separate_port',
          midi_in: true,
          thru: false,
          controller_out: true,
          usb_to_din: false,
          dma: false,
        },
        midi_ports: [
          { cable: 1, name: 'Code 61 MIDI', role: 'din' },
          { cable: 2, name: '?', role: 'other' },
        ],
      }),
    );
    const result = await validateDevice('text');
    expect(result.summary).toBe('Code 61 (stm32f1, board 0x0002), 3 hubs');
    expect(result.presets).toEqual({
      slots: [
        { slot: 1, name: 'Default', program: null, changes: 0, listed: true },
        { slot: 2, name: 'Preset 2', program: null, changes: 0, listed: false },
      ],
      given: true,
      maxSlots: 3,
    });
    expect(result.dma).toEqual({
      channels: [{ channel: 7, request: 'USART2_TX', user: "rgb_serial 'pad_leds'" }],
      free: [1, 2, 3, 4, 5, 6],
    });
    expect(result.dinMidi).toEqual({
      mode: 'separate_port',
      midiIn: true,
      thru: false,
      controllerOut: true,
      usbToDin: false,
      dma: false,
    });
    expect(result.midiPorts).toEqual([{ cable: 1, name: 'Code 61 MIDI', role: 'din' }]);
  });

  it('keeps the whole summary from a builder that sends no DMA map', async () => {
    mockFetch(json({ ok: true, summary: 'Keys\n  DMA1 channels: none; free: 1', diags: [] }));
    expect((await validateDevice('text')).summary).toBe('Keys\n  DMA1 channels: none; free: 1');
  });

  it('throws the server error message with its status', async () => {
    mockFetch(json({ error: 'a device named keys already exists' }, 400));
    const error = await createDevice('keys', 'esp32s3-blank').catch((e: unknown) => e);
    expect(error).toBeInstanceOf(BuilderError);
    expect(error).toMatchObject({ message: 'a device named keys already exists', status: 400 });
  });

  it('says the status when the error is not JSON', async () => {
    mockFetch(new Response('oops', { status: 502, statusText: 'Bad Gateway' }));
    await expect(listPresets()).rejects.toThrow('The builder answered 502 Bad Gateway');
  });

  it('says the builder could not be reached on a network failure', async () => {
    mockFetch(new TypeError('Failed to fetch'));
    await expect(listDevices()).rejects.toThrow(/could not be reached/);
  });

  it('fetches image bytes from an encoded URL', async () => {
    const mock = mockFetch(new Response(new Uint8Array([1, 2, 3])));
    expect(await fetchImage('mine/keys.yaml', 'keys.bin')).toEqual(new Uint8Array([1, 2, 3]));
    expect(mock).toHaveBeenCalledWith(imageUrl('mine/keys.yaml', 'keys.bin'), undefined);
    expect(imageUrl('mine/a b.yaml', 'a b.bin')).toBe('api/image?id=mine%2Fa%20b.yaml&file=a%20b.bin');
  });
});

describe('build log', () => {
  it('finds the exit code on the last line', () => {
    expect(parseBuildExit('compiling\nlinking\n\n[exit 0]\n')).toBe(0);
    expect(parseBuildExit('error: no\n[exit 2]')).toBe(2);
    expect(parseBuildExit('killed\n[exit -9]\n')).toBe(-9);
  });

  it('has no exit code until the stream ends with one', () => {
    expect(parseBuildExit('compiling\n')).toBeNull();
    expect(parseBuildExit('[exit 0]\nmore output\n')).toBeNull();
    expect(parseBuildExit('')).toBeNull();
  });

  it('streams the log as it arrives, across split chunks and characters', async () => {
    const encoded = new TextEncoder().encode('step 1 ✓\nstep 2\n\n[exit 0]\n');
    const cut = 8; // inside the three bytes of ✓
    const body = new ReadableStream<Uint8Array>({
      start(controller) {
        controller.enqueue(encoded.slice(0, cut));
        controller.enqueue(encoded.slice(cut));
        controller.close();
      },
    });
    mockFetch(new Response(body, { headers: { 'Content-Type': 'text/plain' } }));
    const seen: string[] = [];
    const result = await runBuild('mine/keys.yaml', { onLog: (log) => seen.push(log) });
    expect(result.exitCode).toBe(0);
    expect(result.log).toBe('step 1 ✓\nstep 2\n\n[exit 0]\n');
    expect(seen[0]).toBe('step 1 ');
    expect(seen.at(-1)).toBe(result.log);
  });

  it('reports a build that ended without an exit line', async () => {
    mockFetch(new Response('compiling\n'));
    const result = await runBuild('mine/keys.yaml', { onLog: () => undefined });
    expect(result.exitCode).toBeNull();
  });

  it('throws the 409 refusal when a build is already running', async () => {
    mockFetch(json({ error: 'a build is already running' }, 409));
    await expect(runBuild('mine/keys.yaml', { onLog: () => undefined })).rejects.toMatchObject({
      status: 409,
      message: 'a build is already running',
    });
  });
});
