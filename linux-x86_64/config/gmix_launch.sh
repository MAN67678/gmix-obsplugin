#!/usr/bin/env bash
# ============================================================================
#  GMix Linux setup  (OBS-plugin, v1.0.0)
#
#  GMix runs INSIDE OBS as a plugin (obs-gmix-source) and osu!'s capture is
#  enabled persistently, not via a per-launch wrapper:
#    - osu!lazer Flatpak: `flatpak override --user` (sandbox access + env)
#    - osu!stable via osu-winello: a 00-gmix.cfg in osu-winello's own config
#      dir (named to sort first -- osu-wine only sources the first *.cfg),
#  Once this script has run, capture is active every time you launch osu!
#  normally (app menu, `osu-wine`, `flatpak run`, Steam, whatever) -- no
#  special command needed. Re-running this script is safe/idempotent
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
wine_launcher="$(ini_get capture wine_launcher)"
layer_dir="$HOME/.local/share/vulkan/implicit_layer.d"
socket_dir="$HOME/.cache/gmix"

# OBS plugin install paths (Flatpak OBS)
obs_plugin_dir="$HOME/.var/app/com.obsproject.Studio/config/obs-studio/plugins/obs-gmix-source"
obs_plugin_bin_dir="$obs_plugin_dir/bin/64bit"
obs_plugin_locale_dir="$obs_plugin_dir/data/locale"

# Locate build artifacts (release vs dev tree)
if [[ -f "$here/obs_plugin/bin/64bit/obs-gmix-source.so" ]]; then
  build_plugin_so="$here/obs_plugin/bin/64bit/obs-gmix-source.so"
  build_plugin_locale="$here/obs_plugin/data/locale/en-US.ini"
elif [[ -f "$here/../build/obs_plugin/bin/64bit/obs-gmix-source.so" ]]; then
  build_plugin_so="$here/../build/obs_plugin/bin/64bit/obs-gmix-source.so"
  build_plugin_locale="$here/../build/obs_plugin/data/locale/en-US.ini"
else
  build_plugin_so=""
  build_plugin_locale=""
fi

echo "  ===================================================================="
echo "    GMix - real-time motion blur for osu!   (v1.0.0, OBS plugin)"
echo "  ===================================================================="

# 1) Install OBS plugin (idempotent refresh)
if [[ -n "$build_plugin_so" && -f "$build_plugin_so" ]]; then
  echo "  [1/3] installing/refreshing OBS plugin (obs-gmix-source) ..."
  mkdir -p "$obs_plugin_bin_dir" "$obs_plugin_locale_dir"
  cp -f "$build_plugin_so" "$obs_plugin_bin_dir/"
  cp -f "$build_plugin_locale" "$obs_plugin_locale_dir/"
  echo "        done -- plugin installed to $obs_plugin_dir"
else
  echo "  [1/3] WARNING: obs-gmix-source.so not found (build with -DGMIX_BUILD_OBS_PLUGIN=ON)"
  echo "        Skipping OBS plugin install. Layer + capture config will still be set up."
fi

# 2) Make sure the implicit capture layer is registered (idempotent refresh).
echo "  [2/3] installing/refreshing the GMix capture layer ..."
"$GMIX" --install-layer || echo "        (layer install skipped/failed - continuing)"

# 3) Flatpak-sandboxed target: grant PERSISTENT sandbox access to the layer +
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
if [[ -n "$wine_launcher" ]]; then
  # osu!stable under Wine (yawl/Steam pressure-vessel): renders OpenGL, which
  # the 00-gmix.cfg below bridges through mesa Zink (GL-on-Vulkan).
  # Runtime (pressure-vessel). No sandbox override is needed -- pressure-
  # vessel re-exports the host's implicit Vulkan layers into the container
  # (the same mechanism that makes MangoHud/vkBasalt work there), and the
  # game runs as the same user with HOME shared, so the IPC socket under
  # ~/.cache/gmix and the dma-buf fds cross the boundary unchanged. The only
  # thing to arrange is ENABLE_GMIX reaching the game's environment, which
  # osu-wine's own config mechanism does persistently -- with one caveat: it
  # sources only the FIRST *.cfg (bash `source` takes a single file), so the
  # written file must sort before the others and re-source them itself (see
  # the heredoc below). Capture then stays on for plain `osu-wine` launches,
  # desktop entry included.
  if ! command -v "$wine_launcher" >/dev/null 2>&1; then
    echo "  [3/3] ERROR: wine_launcher '$wine_launcher' not found in PATH" >&2
    exit 1
  fi
  osuconfig_data="${XDG_DATA_HOME:-$HOME/.local/share}"
  osu_configs_dir="$osuconfig_data/osuconfig/configs"
  if [[ ! -d "$osu_configs_dir" ]]; then
    echo "  [3/3] ERROR: osu-winello config dir not found: $osu_configs_dir" >&2
    echo "        (is osu-winello installed for this user, and wine_launcher correct?)" >&2
    exit 1
  fi
  echo "  [3/3] enabling capture for osu-winello ($wine_launcher) ..."

  # IMPORTANT: osu-wine does `source "$dir/"*.cfg`, and bash's `source` only
  # sources the FIRST file -- the rest become positional parameters ($1, $2...)
  # and are silently ignored. A file named `gmix.cfg` therefore never applies
  # as soon as anything sorts before it (e.g. example.cfg), which is exactly
  # what happened live. So: sort FIRST (00-) and re-source the others here,
  # keeping their settings working while guaranteeing ours apply.
  cat > "$osu_configs_dir/00-gmix.cfg" <<'GMIXCFG'
# Written by GMix's gmix_launch.sh -- delete this file to disable capture.
#
# This file sorts first on purpose: osu-wine sources only the first *.cfg in
# this directory (bash's `source` takes one file; extra glob matches become
# $1, $2...), so a later-sorting file would never be applied. The loop below
# re-sources the other config files so their settings keep working.
gmix_self="${BASH_SOURCE[0]}"
for gmix_cfg in "$XDG_DATA_HOME/osuconfig/configs/"*.cfg; do
    [ "$gmix_cfg" = "$gmix_self" ] && continue
    [ -r "$gmix_cfg" ] && . "$gmix_cfg"
done
unset gmix_self gmix_cfg

# Read by VkLayer_GMIX's manifest `enable_environment` (implicit Vulkan layer).
ENABLE_GMIX=1
GMIX_TARGET_PROCESS=@GMIX_TARGET@

# osu! renders in OpenGL; Vulkan-layer capture can only intercept a present
# that flows through hookable Vulkan (vkQueuePresentKHR after a VK_KHR_swapchain
# comes from the app). Force mesa's Zink (GL-on-Vulkan) so osu!'s GL goes over
# Vulkan -- the game then presents through the GMix layer's hooks like any
# native Vulkan app. Wine itself does NOT translate GL to Vulkan, and without
# these lines the game's presentation never touches our layer (capture would
# stay dark). Remove to render GL directly again.
MESA_LOADER_DRIVER_OVERRIDE=zink
GMIXCFG
  sed -i "s|@GMIX_TARGET@|$target|" "$osu_configs_dir/00-gmix.cfg"
  echo "        wrote $osu_configs_dir/00-gmix.cfg"

  # Older versions of this script wrote gmix.cfg, which osu-wine never sourced.
  legacy_cfg="$osu_configs_dir/gmix.cfg"
  if [[ -f "$legacy_cfg" ]] && grep -q "Written by GMix's gmix_launch.sh" "$legacy_cfg"; then
    rm -f "$legacy_cfg"
    echo "        removed obsolete $legacy_cfg (osu-wine never sourced it)"
  fi

  # Verify with the exact command osu-wine runs, not just by writing the file.
  if bash -c 'set -a; source "$1"/*.cfg 2>/dev/null; set +a; [ "$ENABLE_GMIX" = 1 ]' \
       _ "$osu_configs_dir"; then
    echo "        verified: osu-wine's config sourcing exports ENABLE_GMIX"
  else
    echo "        WARNING: ENABLE_GMIX is still not exported after sourcing" >&2
    echo "                 $osu_configs_dir/*.cfg -- check that file and re-run" >&2
  fi

  echo "        done -- launch osu! with '$wine_launcher' as usual; capture is always on."
  echo "        (a game that is already running must be restarted to pick this up)"
elif [[ -n "$flatpak_app_id" ]]; then
  if ! flatpak info "$flatpak_app_id" >/dev/null 2>&1; then
    echo "  [3/3] ERROR: flatpak app '$flatpak_app_id' is not installed" >&2
    exit 1
  fi
  # Query the sandbox's own baseline XDG_DATA_DIRS rather than hardcoding it
  # (varies by runtime/extensions) and prepend $HOME/.local/share to it.
  base_data_dirs="$(flatpak run --command=sh "$flatpak_app_id" -c 'echo "$XDG_DATA_DIRS"' 2>/dev/null || true)"
  : "${base_data_dirs:=/app/share:/usr/share:/usr/share/runtime/share:/run/host/user-share:/run/host/share}"
  echo "  [3/3] granting '$flatpak_app_id' persistent capture access (flatpak override) ..."
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
  echo "  [3/3] neither 'flatpak_app_id' nor 'wine_launcher' is set in $ini --"
  echo "        set one of them to have this script grant persistent capture"
  echo "        access ('sh.ppy.osu' for Flatpak lazer, 'osu-wine' for"
  echo "        osu-winello), or export these yourself before launching a"
  echo "        non-Flatpak build:"
  echo "          ENABLE_GMIX=1 GMIX_TARGET_PROCESS='$target' <your osu! binary>"
fi

echo ""
echo "  Done. Restart OBS, then add the 'GMix Motion Blur' source (once) to see the blur."
