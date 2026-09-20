import path from 'node:path';
import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

export default defineConfig({
  resolve: {
    alias: {
      '@': path.resolve(__dirname, './src'),
    },
  },
  plugins: [react()],
  build: {
    outDir: 'dist',
    rollupOptions: {
      input: { index: path.resolve(__dirname, './index.html') },
      output: {
        // Bootstrap used to load as a separate <script> tag, so it was cached
        // independently of app code. Now that it enters the build graph via
        // src/runtime/vendor.ts, keep it in its own chunk so an app-only change
        // does not invalidate it. Chart.js is intentionally not listed: it is
        // reachable only from src/ui/LineChart.tsx, so leaving it to automatic
        // splitting keeps it out of the eagerly-loaded entry.
        // The `react` chunk holds the React runtime and the libraries pinned to
        // it — router and query cache — which turn over on the same slow clock.
        manualChunks(id: string) {
          if (!id.includes('node_modules')) return;
          if (/[\\/]node_modules[\\/](bootstrap|@popperjs)[\\/]/.test(id)) return 'vendor';
          if (/[\\/]node_modules[\\/](react|react-dom|react-router|react-router-dom|scheduler|@tanstack)[\\/]/.test(id))
            return 'react';
          return;
        },
      },
    },
  },
  server: {
    host: '0.0.0.0',
    proxy: {
      '/api': {
        target: 'http://localhost:8080',
        changeOrigin: true,
      },
      '/v1': {
        target: 'http://localhost:8080',
        changeOrigin: true,
      },
      '/oauth': {
        target: 'http://localhost:8080',
        changeOrigin: true,
      },
      '/auth/callback': {
        target: 'http://localhost:8080',
        changeOrigin: true,
      },
      '/readyz': {
        target: 'http://localhost:8080',
        changeOrigin: true,
      },
    },
  },
});
