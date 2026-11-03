/**
 * An upload to the Code 61 that has to outlive the page. M-Audio's bootloader is reached by
 * switching the keyboard off and on, or by a gemifi app restarting into it; on Windows, Chrome can
 * stop responding while a MIDI device vanishes, and the way out is to close it. The image is
 * stored in IndexedDB first (it is ~200 KB of bytes, too much for localStorage), so the reopened
 * page can offer to finish.
 *
 * Storage may be missing or refuse (a private window, blocked site data): every call here catches
 * that and the page carries on without it.
 */

import type { PickedImage } from './picked-image.ts';

/** How long a stored upload is offered, an hour: after that it is more likely stale than wanted. */
export const PENDING_MAX_AGE_MS = 3_600_000;

/** An upload waiting for the keyboard to come back in update mode. */
export interface PendingUpload {
  image: PickedImage;
  /** When it was stored, in ms since the epoch. */
  savedAt: number;
}

/** Where one pending upload is kept: IndexedDB in the page, a fake in tests. */
export interface PendingStore {
  /** @returns The stored value, or undefined when there is none. */
  get(): Promise<unknown>;
  /** @param value - Replaces what is stored. */
  put(value: unknown): Promise<void>;
  /** Removes what is stored. */
  remove(): Promise<void>;
}

const DB_NAME = 'gemifi-updater';
const DB_VERSION = 1;
const STORE_NAME = 'pending-upload';
const KEY = 'upload';

/**
 * @param request - An IndexedDB request.
 * @returns Its result, or its error as a rejection.
 */
function settle<T>(request: IDBRequest<T>): Promise<T> {
  return new Promise((resolve, reject) => {
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error ?? new Error('IndexedDB request failed.'));
  });
}

/**
 * A {@link PendingStore} in IndexedDB: one object store, one key. The database is opened for each
 * call and closed after it, so nothing stays open while the page waits.
 *
 * @param factory - `indexedDB`.
 * @returns The store.
 */
export function indexedDbPendingStore(factory: IDBFactory): PendingStore {
  const run = async <T>(mode: IDBTransactionMode, act: (store: IDBObjectStore) => IDBRequest<T>): Promise<T> => {
    const open = factory.open(DB_NAME, DB_VERSION);
    open.onupgradeneeded = () => {
      open.result.createObjectStore(STORE_NAME);
    };
    const db = await settle(open);
    try {
      const transaction = db.transaction(STORE_NAME, mode);
      const result = await settle(act(transaction.objectStore(STORE_NAME)));
      await new Promise<void>((resolve, reject) => {
        transaction.oncomplete = () => resolve();
        transaction.onerror = () => reject(transaction.error ?? new Error('IndexedDB transaction failed.'));
        transaction.onabort = () => reject(transaction.error ?? new Error('IndexedDB transaction aborted.'));
      });
      return result;
    } finally {
      db.close();
    }
  };
  return {
    get: () => run('readonly', (store) => store.get(KEY)),
    put: async (value) => {
      await run('readwrite', (store) => store.put(value, KEY));
    },
    remove: async () => {
      await run('readwrite', (store) => store.delete(KEY));
    },
  };
}

/**
 * @returns The page's store, or null where the browser has no IndexedDB or refuses it.
 */
export function defaultPendingStore(): PendingStore | null {
  try {
    const factory = globalThis.indexedDB;
    return factory === undefined ? null : indexedDbPendingStore(factory);
  } catch {
    return null;
  }
}

/** What is stored: the image's fields, flat, and the time. */
interface StoredUpload {
  name: string;
  bytes: Uint8Array;
  isGfw: boolean;
  source: string | null;
  nodeName: string | null;
  savedAt: number;
}

/**
 * @param value - What came out of the store.
 * @returns It as a pending upload, or null when it is not one.
 */
function pendingOf(value: unknown): PendingUpload | null {
  if (typeof value !== 'object' || value === null) {
    return null;
  }
  const stored = value as Partial<StoredUpload>;
  const isValid =
    typeof stored.name === 'string' &&
    stored.bytes instanceof Uint8Array &&
    typeof stored.isGfw === 'boolean' &&
    typeof stored.savedAt === 'number' &&
    (typeof stored.source === 'string' || stored.source === null || stored.source === undefined) &&
    (typeof stored.nodeName === 'string' || stored.nodeName === null || stored.nodeName === undefined);
  if (isValid === false) {
    return null;
  }
  const { name, bytes, isGfw, source, nodeName, savedAt } = stored as StoredUpload;
  return {
    image: { name, bytes, isGfw, source: source ?? undefined, nodeName: nodeName ?? null },
    savedAt,
  };
}

/**
 * Stores the image to upload once the keyboard is back in update mode, replacing any other.
 *
 * @param store - Where; null when there is no storage.
 * @param image - The image the user picked.
 * @param now - The time to stamp it with.
 * @returns Whether it was stored.
 */
export async function savePendingUpload(
  store: PendingStore | null,
  image: PickedImage,
  now: number = Date.now(),
): Promise<boolean> {
  if (store === null) {
    return false;
  }
  const stored: StoredUpload = {
    name: image.name,
    bytes: image.bytes,
    isGfw: image.isGfw,
    source: image.source ?? null,
    nodeName: image.nodeName,
    savedAt: now,
  };
  try {
    await store.put(stored);
    return true;
  } catch {
    return false;
  }
}

/**
 * Reads the stored upload. One older than {@link PENDING_MAX_AGE_MS}, or not an upload at all, is
 * removed and not returned.
 *
 * @param store - Where; null when there is no storage.
 * @param now - The time to judge its age by.
 * @returns The upload, or null.
 */
export async function loadPendingUpload(
  store: PendingStore | null,
  now: number = Date.now(),
): Promise<PendingUpload | null> {
  if (store === null) {
    return null;
  }
  try {
    const value = await store.get();
    if (value === undefined) {
      return null;
    }
    const pending = pendingOf(value);
    const isFresh = pending !== null && now - pending.savedAt <= PENDING_MAX_AGE_MS && pending.savedAt <= now;
    if (isFresh) {
      return pending;
    }
    await clearPendingUpload(store);
    return null;
  } catch {
    return null;
  }
}

/**
 * Forgets the stored upload: it was sent, replaced or dismissed.
 *
 * @param store - Where; null when there is no storage.
 */
export async function clearPendingUpload(store: PendingStore | null): Promise<void> {
  try {
    await store?.remove();
  } catch {
    // Nothing to do: an entry that stays expires.
  }
}
