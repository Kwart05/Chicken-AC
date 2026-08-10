// Chicken AC — Climate Controller Service Worker
// Strategy: cache-first for app shell; network-only for /api/* and /ws.
// Stale sensor data or silent no-ops on control actions are actively harmful,
// so API and WebSocket traffic is NEVER served from cache.

const CACHE_NAME = 'chickenac-v1';

// Resources to pre-cache on install (app shell)
const PRECACHE_URLS = [
  '/',
  '/index.html',
  '/manifest.json',
  '/icons/icon-192.png',
  '/icons/icon-512.png',
  '/icons/icon-512-maskable.png',
  'https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;600;700&family=IBM+Plex+Sans:wght@400;500&display=swap',
  'https://cdn.jsdelivr.net/npm/chart.js@4/dist/chart.umd.min.js',
];

// ── Install: pre-cache app shell ────────────────────────────────────────────
self.addEventListener('install', (event) => {
  event.waitUntil(
    caches.open(CACHE_NAME).then((cache) => {
      // Use individual adds so one failure doesn't block the rest
      return Promise.allSettled(
        PRECACHE_URLS.map((url) =>
          cache.add(url).catch((err) =>
            console.warn('[SW] Failed to precache:', url, err)
          )
        )
      );
    }).then(() => self.skipWaiting())
  );
});

// ── Activate: purge old caches ──────────────────────────────────────────────
self.addEventListener('activate', (event) => {
  event.waitUntil(
    caches.keys().then((keys) =>
      Promise.all(
        keys
          .filter((key) => key !== CACHE_NAME)
          .map((key) => caches.delete(key))
      )
    ).then(() => self.clients.claim())
  );
});

// ── Fetch: route decisions ──────────────────────────────────────────────────
self.addEventListener('fetch', (event) => {
  const url = new URL(event.request.url);

  // NEVER cache API calls or WebSocket upgrades — always go to network
  if (
    url.pathname.startsWith('/api/') ||
    url.pathname === '/ws' ||
    event.request.headers.get('upgrade') === 'websocket'
  ) {
    // Network-only: do not call event.respondWith() so the browser handles it
    return;
  }

  // Cache-first for everything else (app shell, fonts, Chart.js CDN)
  event.respondWith(
    caches.match(event.request).then((cached) => {
      if (cached) return cached;

      // Not in cache — fetch from network and store for next time
      return fetch(event.request)
        .then((response) => {
          // Only cache valid same-origin or CORS responses
          if (
            response &&
            response.status === 200 &&
            (response.type === 'basic' || response.type === 'cors')
          ) {
            const responseToCache = response.clone();
            caches.open(CACHE_NAME).then((cache) => {
              cache.put(event.request, responseToCache);
            });
          }
          return response;
        })
        .catch(() => {
          // Offline fallback: serve index.html for navigation requests
          if (event.request.mode === 'navigate') {
            return caches.match('/index.html');
          }
          return new Response('Offline', { status: 503 });
        });
    })
  );
});
