# AgentSphere Website

Source of [tenbox.ai](https://tenbox.ai) — the marketing site and release manifest host
for AgentSphere. Built with Vue 3 + Vite and deployed as static files.

## Stack

- **Vue 3** (`<script setup>` SFCs) with [`vue-i18n`](https://vue-i18n.intlify.dev/) for
  English / Simplified Chinese localization
- **Vite 7** as build tool and dev server
- **Custom domain** served via `public/CNAME` (→ `tenbox.ai`)

## Project layout

```
website/
├── index.html           # Page shell, favicon, SEO meta
├── vite.config.js       # Injects __APP_VERSION__ and download URLs from public/api/version.json
├── src/
│   ├── main.js          # Vue + i18n bootstrap
│   ├── App.vue          # Root layout
│   ├── components/      # NavBar, HeroSection, FeaturesSection, HowItWorks, FooterSection
│   ├── i18n/            # en-US.json, zh-CN.json, and the i18n plugin setup
│   └── assets/          # Static art bundled by Vite
└── public/
    ├── favicon.png
    ├── CNAME            # GitHub Pages custom domain
    ├── images/          # Screenshots referenced by the root README and the site
    └── api/
        └── version.json # Canonical release manifest consumed by the app and the site
```

## Release manifest (`public/api/version.json`)

The same `version.json` served at [`https://tenbox.ai/api/version.json`](https://tenbox.ai/api/version.json)
is read at build time by `vite.config.js` to inject the current version and per-platform
download URLs into the page, and at runtime by the AgentSphere manager for update checks.
Keep its schema (`latest_version`, `platforms.windows`, `platforms.macos`, `sha256`, ...)
in sync with the manager's expectations — see `scripts/image_manager.py` and the
`http_download` / update-check paths under `src/manager`.

## Develop

```bash
cd website
npm install
npm run dev       # Vite dev server with HMR
```

## Build & preview

```bash
npm run build     # Emits dist/
npm run preview   # Serve the built site locally
```

The generated `dist/` folder is what gets published to the hosting provider.

## Editor tips

Vue 3 tooling recommendations (Volar, TypeScript Vue plugin, etc.) are documented
in the [Vue Scaling-up Guide](https://vuejs.org/guide/scaling-up/tooling.html#ide-support).
