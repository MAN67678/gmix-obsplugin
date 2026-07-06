# gmix-obsplugin

osu! realtime frameblending / motion blur for streamers, delivered as a
**native OBS Studio plugin** (zero-copy GPU delivery, no virtual camera, no
kernel module). See platform subfolders for build/setup instructions:

- [`linux-x86_64/`](linux-x86_64/README.md) — Linux (Vulkan capture layer +
  OBS plugin via dma-buf). Working end-to-end.
- [`WIN32/`](WIN32/README.md) — Windows port. **Not coming soon** — blocked
  on the developer's own hardware/driver setup: no Windows machine with a
  suitable GPU/driver combination to build and test a capture layer against
  any of Windows' relevant graphics APIs (osu! can run on Vulkan, D3D11, or
  OpenGL there), not a decision against Windows itself. GMix is
  Linux-exclusive for now.
