# gmix-obsplugin

osu! realtime frameblending / motion blur for streamers, delivered as a
**native OBS Studio plugin** (zero-copy GPU delivery, no virtual camera, no
kernel module). See platform subfolders for build/setup instructions:

- [`linux-x86_64/`](linux-x86_64/README.md) — Linux (Vulkan capture layer +
  OBS plugin via dma-buf). Working end-to-end.
- [`WIN32/`](WIN32/README.md) — Windows port. Not started yet.
