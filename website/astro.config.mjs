import sitemap from '@astrojs/sitemap';
import { defineConfig } from 'astro/config';

export default defineConfig({
  site: 'https://stillmic.robin.build',
  output: 'static',
  build: { format: 'file' },
  trailingSlash: 'never',
  integrations: [sitemap()],
});
