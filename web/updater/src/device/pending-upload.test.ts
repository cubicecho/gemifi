import { describe, expect, it } from 'vitest';
import {
  clearPendingUpload,
  loadPendingUpload,
  PENDING_MAX_AGE_MS,
  type PendingStore,
  savePendingUpload,
} from './pending-upload.ts';
import { pickedImageOf } from './picked-image.ts';

/** A store in memory, as IndexedDB keeps it (structured clone aside). */
function memoryStore(): PendingStore & { value: unknown } {
  const store = {
    value: undefined as unknown,
    get: async () => store.value,
    put: async (value: unknown) => {
      store.value = value;
    },
    remove: async () => {
      store.value = undefined;
    },
  };
  return store;
}

/** A store that refuses everything, as a private window can. */
const failingStore: PendingStore = {
  get: () => Promise.reject(new Error('SecurityError')),
  put: () => Promise.reject(new Error('QuotaExceededError')),
  remove: () => Promise.reject(new Error('SecurityError')),
};

const NOW = 1_700_000_000_000;
const bytes = Uint8Array.from({ length: 300 }, (_, i) => i & 0xff);

describe('pending upload', () => {
  it('round-trips a stock file and a built .gfw with its source', async () => {
    const store = memoryStore();
    const stock = pickedImageOf('FirmwareFile_code61_v1.2.2.hex.txt', bytes);
    expect(await savePendingUpload(store, stock, NOW)).toBe(true);
    expect(await loadPendingUpload(store, NOW + 1000)).toEqual({ image: stock, savedAt: NOW });

    const built = pickedImageOf('keys-1.2.3.gfw', bytes, 'Built from Keys', 'keys');
    await savePendingUpload(store, built, NOW);
    const loaded = await loadPendingUpload(store, NOW);
    expect(loaded?.image).toEqual(built);
    expect(loaded?.image.isGfw).toBe(true);
    expect(loaded?.image.source).toBe('Built from Keys');
  });

  it('forgets an upload older than an hour', async () => {
    const store = memoryStore();
    await savePendingUpload(store, pickedImageOf('a.gfw', bytes), NOW);
    expect(await loadPendingUpload(store, NOW + PENDING_MAX_AGE_MS)).not.toBeNull();
    expect(await loadPendingUpload(store, NOW + PENDING_MAX_AGE_MS + 1)).toBeNull();
    expect(store.value).toBeUndefined();
  });

  it('forgets what is not an upload', async () => {
    const store = memoryStore();
    store.value = { name: 'x', bytes: 'not bytes', isGfw: true, savedAt: NOW };
    expect(await loadPendingUpload(store, NOW)).toBeNull();
    expect(store.value).toBeUndefined();
  });

  it('clears', async () => {
    const store = memoryStore();
    await savePendingUpload(store, pickedImageOf('a.gfw', bytes), NOW);
    await clearPendingUpload(store);
    expect(await loadPendingUpload(store, NOW)).toBeNull();
  });

  it('never throws when storage fails or is missing', async () => {
    const image = pickedImageOf('a.gfw', bytes);
    await expect(savePendingUpload(failingStore, image, NOW)).resolves.toBe(false);
    await expect(loadPendingUpload(failingStore, NOW)).resolves.toBeNull();
    await expect(clearPendingUpload(failingStore)).resolves.toBeUndefined();
    await expect(savePendingUpload(null, image, NOW)).resolves.toBe(false);
    await expect(loadPendingUpload(null, NOW)).resolves.toBeNull();
    await expect(clearPendingUpload(null)).resolves.toBeUndefined();
  });
});
