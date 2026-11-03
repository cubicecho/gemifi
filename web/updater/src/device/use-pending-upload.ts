import { useCallback, useEffect, useRef, useState } from 'react';
import {
  clearPendingUpload,
  defaultPendingStore,
  loadPendingUpload,
  type PendingStore,
  type PendingUpload,
  savePendingUpload,
} from './pending-upload.ts';
import type { PickedImage } from './picked-image.ts';

/** The page's store, made once. */
const pageStore = defaultPendingStore();

/**
 * The upload that waits for the Code 61 to come back in M-Audio's update mode: read once when the
 * page loads, stored before the keyboard vanishes, forgotten once sent, replaced or dismissed.
 *
 * @param store - Where it is kept; the page's IndexedDB by default.
 * @returns The pending upload, `save` and `clear`.
 */
export function usePendingUpload(store: PendingStore | null = pageStore): {
  pending: PendingUpload | null;
  save: (image: PickedImage) => Promise<void>;
  clear: () => void;
} {
  const [pending, setPending] = useState<PendingUpload | null>(null);
  /** Set by `save` and `clear`, so a load that finishes after either does not undo it. */
  const isTouched = useRef(false);

  useEffect(() => {
    let isCurrent = true;
    void loadPendingUpload(store).then((loaded) => {
      if (isCurrent && isTouched.current === false && loaded !== null) {
        setPending(loaded);
      }
    });
    return () => {
      isCurrent = false;
    };
  }, [store]);

  const save = useCallback(
    async (image: PickedImage) => {
      isTouched.current = true;
      const savedAt = Date.now();
      setPending({ image, savedAt });
      await savePendingUpload(store, image, savedAt);
    },
    [store],
  );

  const clear = useCallback(() => {
    isTouched.current = true;
    setPending(null);
    void clearPendingUpload(store);
  }, [store]);

  return { pending, save, clear };
}
