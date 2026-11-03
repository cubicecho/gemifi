import { describe, expect, it } from 'vitest';
import { isWindows } from './platform.ts';

const CHROME_WINDOWS =
  'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36';
const CHROME_LINUX =
  'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36';
const FIREFOX_MAC = 'Mozilla/5.0 (Macintosh; Intel Mac OS X 14.5; rv:132.0) Gecko/20100101 Firefox/132.0';

describe('isWindows', () => {
  it('reads userAgentData.platform where the browser has it', () => {
    expect(isWindows({ userAgentData: { platform: 'Windows' }, userAgent: CHROME_LINUX })).toBe(true);
    expect(isWindows({ userAgentData: { platform: 'Linux' }, userAgent: CHROME_WINDOWS })).toBe(false);
    expect(isWindows({ userAgentData: { platform: 'macOS' } })).toBe(false);
  });

  it('falls back to the user agent', () => {
    expect(isWindows({ userAgent: CHROME_WINDOWS })).toBe(true);
    expect(isWindows({ userAgentData: { platform: '' }, userAgent: CHROME_WINDOWS })).toBe(true);
    expect(isWindows({ userAgent: CHROME_LINUX })).toBe(false);
    expect(isWindows({ userAgent: FIREFOX_MAC })).toBe(false);
  });

  it('says no outside a browser', () => {
    expect(isWindows(undefined)).toBe(false);
    expect(isWindows({})).toBe(false);
  });
});
