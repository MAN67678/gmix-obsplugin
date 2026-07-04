# gmix-obsplugin

osu! realtime frameblending / motion blur for streamers, delivered as a
**native OBS Studio plugin** (zero-copy GPU delivery, no virtual camera, no
kernel module). See platform subfolders for build/setup instructions:

- [`linux-x86_64/`](linux-x86_64/README.md) — Linux (Vulkan capture layer +
  OBS plugin via dma-buf). Working end-to-end.
- [`WIN32/`](WIN32/README.md) — Windows port. Phase 1 (osu!stable/OpenGL ->
  D3D11 gmix/OBS pipeline) scaffolded, not yet build-verified or run; see
  `WIN32/etc/DEV_NOTES.md` for status. osu!lazer/native-D3D11 (phase 2) not
  started.
