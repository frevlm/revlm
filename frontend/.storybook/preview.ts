import type { Preview } from '@storybook/react-vite';
import { mswLoader } from 'msw-storybook-addon/csf3';

import '../src/runtime/vendor';
import '../src/index.css';
import { handlers } from '../src/storybook/handlers';
import { withQuery } from '../src/storybook/decorators';

const preview: Preview = {
  parameters: {
    layout: 'fullscreen',
    controls: {
      matchers: {
        color: /(background|color)$/i,
        date: /Date$/i,
      },
    },
  },
  decorators: [withQuery()],
  loaders: [mswLoader()],
  beforeEach({ msw }) {
    msw.use(...handlers);
  },
};

export default preview;
