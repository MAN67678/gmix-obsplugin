#!/usr/bin/env bash
# ============================================================================
#  GMix Linux setup  (OBS-plugin, v1.0.0)
#
#  GMix runs INSIDE OBS as a plugin (obs-gmix-source) and osu!'s capture is
#  enabled via PERSISTENT Flatpak overrides (`flatpak override --user`), not
#  a per-launch wrapper -- once this script has run, capture is active every
#  time you launch osu! normally (app menu, `flatpak run`, Steam, whatever),
#  no special command needed. Re-running this script is safe/idempotent
#  (e.g. after rebuilding gmix, to refresh the installed layer).
#
#  This script does NOT launch osu! itself -- launch it however you normally
#  do, then add the "GMix Motion Blur" source in OBS (once; it persists in
#  your scene).
#
#  Capture is the implicit Vulkan layer -- there is no DLL injection.
# ============================================================================
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ini="$here/gmix_config.ini"

# Locate the gmix binary (only needed for --install-layer): next to this
# script (release), else ../build (dev tree).
if   [[ -x "$here/gmix" ]];          then GMIX="$here/gmix"
elif [[ -x "$here/../build/gmix" ]]; then GMIX="$here/../build/gmix"
else echo "error: gmix binary not found near $here" >&2; exit 1; fi

[[ -f "$ini" ]] || { echo "error: config not found: $ini" >&2; exit 1; }

# --- tiny INI reader: ini_get <section> <key> -------------------------------
ini_get() {
  sed -n "/^[[:space:]]*\[$1\][[:space:]]*$/,/^[[:space:]]*\[/p" "$ini" \
    | sed 's/[;#].*//' \
    | awk -F= -v k="$2" '{
        key=$1; sub(/^[[:space:]]+/,"",key); sub(/[[:space:]]+$/,"",key)
        if (key==k) { val=$2; sub(/^[[:space:]]+/,"",val); sub(/[[:space:]]+$/,"",val); print val; exit }
      }'
}

target="$(ini_get capture target)";   : "${target:=osu!}"
flatpak_app_id="$(ini_get capture flatpak_app_id)"
layer_dir="$HOME/.local/share/vulkan/implicit_layer.d"
socket_dir="$HOME/.cache/gmix"

echo "  ===================================================================="
echo "    GMix - real-time motion blur for osu!   (v1.0.0, OBS plugin)"
echo "  ===================================================================="

# 1) Make sure the implicit capture layer is registered (idempotent refresh).
echo "  [1/2] installing/refreshing the GMix capture layer ..."
"$GMIX" --install-layer || echo "        (layer install skipped/failed - continuing)"

# 2) Flatpak-sandboxed target: grant PERSISTENT sandbox access to the layer +
# IPC-socket dirs, and PERSISTENT env vars enabling capture. Two sandbox
# quirks, both confirmed by testing against this exact app:
#  - Flatpak apps do not see the host's real XDG_DATA_HOME by default (it's
#    remapped to ~/.var/app/<id>/data even when general $HOME access is
#    granted), so VK_LAYER_PATH must point the sandboxed Vulkan loader at
#    the layer JSON explicitly.
#  - VK_LAYER_PATH alone is NOT reliably honored for IMPLICIT layers by
#    modern Vulkan loaders inside a sandbox (the same issue that broke
#    MangoHud/vkBasalt in Steam's pressure-vessel -- see
#    https://github.com/ValveSoftware/steam-runtime/issues/662). The fix is
#    to ALSO prepend the layer's parent dir to XDG_DATA_DIRS, a standard
#    (trusted) loader search path the sandbox restriction doesn't block.
# `flatpak override` persists all of this forever -- this is a one-time
# setup, not something re-applied per launch.
if [[ -n "$flatpak_app_id" ]]; then
  if ! flatpak info "$flatpak_app_id" >/dev/null 2>&1; then
    echo "  [2/2] ERROR: flatpak app '$flatpak_app_id' is not installed" >&2
    exit 1
  fi
  # Query the sandbox's own baseline XDG_DATA_DIRS rather than hardcoding it
  # (varies by runtime/extensions) and prepend $HOME/.local/share to it.
  base_data_dirs="$(flatpak run --command=sh "$flatpak_app_id" -c 'echo "$XDG_DATA_DIRS"' 2>/dev/null || true)"
  : "${base_data_dirs:=/app/share:/usr/share:/usr/share/runtime/share:/run/host/user-share:/run/host/share}"
  echo "  [2/2] granting '$flatpak_app_id' persistent capture access (flatpak override) ..."
  flatpak override --user \
    --filesystem="$layer_dir:ro" \
    --filesystem="$socket_dir" \
    --filesystem="$HOME/.config/gmix" \
    --env=ENABLE_GMIX=1 \
    --env="GMIX_TARGET_PROCESS=$target" \
    --env="VK_LAYER_PATH=$layer_dir" \
    --env="XDG_DATA_DIRS=$HOME/.local/share:$base_data_dirs" \
    "$flatpak_app_id"
  echo "        done -- launch '$flatpak_app_id' normally from now on, capture is always on."
else
  echo "  [2/2] no 'flatpak_app_id' set in $ini -- set it (e.g. sh.ppy.osu) to have"
  echo "        this script grant it persistent capture access, or export these"
  echo "        yourself before launching a non-Flatpak build:"
  echo "          ENABLE_GMIX=1 GMIX_TARGET_PROCESS='$target' <your osu! binary>"
fi

echo ""
echo "  Done. Add the 'GMix Motion Blur' source in OBS (once) to see the blur."
