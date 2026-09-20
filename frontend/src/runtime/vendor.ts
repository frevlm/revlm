/**
 * Vendor JS entry point.
 *
 * Replaces the old `<script>` tags in index.html / preview-head.html with a real
 * module import so Vite bundles Bootstrap instead of two HTML head lists being
 * hand-synced. Importing `bootstrap` registers its declarative `data-bs-*`
 * data-api (dropdowns, collapses, etc.) as a side effect, same as the old
 * bootstrap.bundle.min.js did.
 *
 * Chart.js is deliberately absent: it is imported by `src/ui/LineChart.tsx`, so
 * it loads with the page chunks that draw charts rather than on every page.
 */
import * as bootstrap from 'bootstrap';

// Transitional bridge: `window.bootstrap` is read by src/components/modal.ts and
// src/pages/admin/ChannelsPage.tsx, which still drive modals through the old UMD
// global. It goes away with the controlled `Modal` primitive.
declare global {
  interface Window {
    bootstrap: typeof bootstrap;
  }
}

window.bootstrap = bootstrap;
