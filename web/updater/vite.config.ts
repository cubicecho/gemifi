import { fileURLToPath } from 'node:url';
import tailwindcss from '@tailwindcss/vite';
import react from '@vitejs/plugin-react';
import { defineConfig } from 'vitest/config';

/** Where `npm run dev` sends builder requests: `python3 tools/builder/gemifi_build.py dashboard` by default. */
const builderUrl = process.env.GEMIFI_BUILDER_URL ?? 'http://127.0.0.1:6055';
/** The builder checks the Host header on loopback, so the proxy sends the builder's own. */
const builderProxy = { target: builderUrl, changeOrigin: true };

export default defineConfig({
  // Relative asset paths, so the build works from any sub-path (GitHub Pages serves it under /<repo>/).
  base: './',
  plugins: [react(), tailwindcss()],
  server: {
    host: '0.0.0.0',
    port: 3000,
    strictPort: true,
    proxy: { '/api': builderProxy, '/download': builderProxy },
  },
  preview: {
    host: '0.0.0.0',
    port: 3000,
    strictPort: true,
    proxy: { '/api': builderProxy, '/download': builderProxy },
  },
  resolve: { alias: { '@': fileURLToPath(new URL('./src', import.meta.url)) } },
  build: {
    // The bundled dependencies' license texts (MIT/ISC/Apache need them shipped), served next to the app.
    license: { fileName: 'THIRD_PARTY_LICENSES.md' },
    rolldownOptions: {
      // React on its own, so the app chunk stays under the 500 kB warning and React caches across releases.
      output: {
        codeSplitting: { groups: [{ name: 'react', test: /node_modules[\\/](react|react-dom|scheduler)[\\/]/ }] },
      },
    },
  },
  test: {
    pool: 'threads',
    environment: 'node',
    include: ['src/**/*.test.ts'],
  },
});
