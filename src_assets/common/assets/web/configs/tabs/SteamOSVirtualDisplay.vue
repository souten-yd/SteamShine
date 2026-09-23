<script setup>
import { ref } from 'vue'

const props = defineProps({
  config: Object,
})
const config = ref(props.config)
</script>

<template>
  <div id="steamos-display" class="config-page">
    <div class="mb-3">
      <div class="form-check">
        <input id="steamos_virtual_display_enabled" v-model="config.steamos_virtual_display_enabled" class="form-check-input" type="checkbox" true-value="enabled" false-value="disabled" />
        <label class="form-check-label" for="steamos_virtual_display_enabled">Enable custom startup display policy</label>
      </div>
      <div class="form-text">Unchecked uses the safe automatic startup-probe defaults. It does not disable the Moonlight invariant: every client app stops stock Game Mode when present and uses a client-sized SteamShine-owned Gamescope.</div>
    </div>
    <div class="mb-3">
      <label class="form-label" for="steamos_session_source">Gamescope Session Source</label>
      <select id="steamos_session_source" v-model="config.steamos_session_source" class="form-select">
        <option value="auto">Auto: verified stock first, then owned</option>
        <option value="existing_gamescope">Verified stock Game Mode only</option>
        <option value="owned_private">SteamShine-owned only</option>
      </select>
      <div class="form-text">Used only for startup encoder probing. A real Moonlight app never attaches to this source; it always hands off to an owned Gamescope.</div>
    </div>
    <div class="mb-3">
      <div class="form-check">
        <input id="steamos_keep_session_alive" v-model="config.steamos_keep_session_alive" class="form-check-input" type="checkbox" true-value="enabled" false-value="disabled" />
        <label class="form-check-label" for="steamos_keep_session_alive">Keep a SteamShine-owned session after disconnect</label>
      </div>
      <div class="form-text">Enabled preserves the owned Gamescope for a compatible Moonlight reconnect. Disabled stops it after the last stream and restores stock Game Mode.</div>
    </div>
    <div class="mb-3">
      <label class="form-label" for="steamos_existing_gamescope_pid">Existing Gamescope PID</label>
      <input id="steamos_existing_gamescope_pid" v-model="config.steamos_existing_gamescope_pid" class="form-control" type="number" min="0" step="1" />
      <div class="form-text">Used only by startup probing. 0 selects automatically; a nonzero PID must be the uniquely verified current-user stock Game Mode Gamescope.</div>
    </div>
    <div class="mb-3">
      <label class="form-label" for="steamos_local_presentation">Local Presentation</label>
      <select id="steamos_local_presentation" v-model="config.steamos_local_presentation" class="form-select">
        <option value="auto">Auto: mirror with verified KWin and a physical output; otherwise remote only</option>
        <option value="off">Off: remote streaming only</option>
        <option value="mirror">Require fullscreen nested Gamescope</option>
      </select>
      <div class="form-text">Controls presentation of the Moonlight-owned Gamescope. Mirror requires a verified KWin Wayland endpoint and a connected physical output.</div>
    </div>
    <div class="mb-3">
      <label class="form-label" for="steamos_virtual_display_mode">Virtual Display Mode</label>
      <select id="steamos_virtual_display_mode" class="form-select" v-model="config.steamos_virtual_display_mode">
        <option value="off">Off: do not create a display during startup probing</option>
        <option value="auto">Auto: use a verified available source during startup probing</option>
        <option value="force">Force: require an owned display during startup probing</option>
      </select>
      <div class="form-text">This changes startup encoder probing only. Off does not disable client handoff; Moonlight still gets its requested owned Gamescope geometry.</div>
    </div>
    <div class="mb-3">
      <label class="form-label" for="steamos_steam_migration">Desktop Steam migration</label>
      <select id="steamos_steam_migration" v-model="config.steamos_steam_migration" class="form-select">
        <option value="auto_idle">Auto: migrate only verified idle Steam</option>
        <option value="reject">Reject while Desktop Steam is running</option>
      </select>
      <div class="form-text">Auto uses Steam's normal shutdown command for owned headless or nested Gamescope. Active games, ambiguous metadata, and timeouts leave Steam running and reject the launch.</div>
    </div>
    <div class="alert alert-info mb-3">Moonlight handoff is fixed: stop stock Game Mode if it exists, then start or reuse an owned Gamescope at the requested size, refresh rate, and HDR mode. An already-stopped stock session is a successful no-op. The legacy <code>steamos_stock_session_handoff</code> value is retained only for configuration compatibility.</div>
    <details class="mb-3">
      <summary class="mb-3">Advanced SteamOS session settings</summary>
      <div class="mb-3">
        <label class="form-label" for="steamos_gamescope_path">Gamescope executable</label>
        <input id="steamos_gamescope_path" v-model="config.steamos_gamescope_path" class="form-control" type="text" />
      </div>
      <div class="mb-3">
        <label class="form-label" for="steamos_virtual_desktop_command">Virtual Desktop command</label>
        <input id="steamos_virtual_desktop_command" v-model="config.steamos_virtual_desktop_command" class="form-control" type="text" />
        <div class="form-text">Launched only for the commandless Desktop app inside a SteamShine-owned private session. It is never launched on the physical Desktop or inside attached Game Mode.</div>
      </div>
      <div class="mb-3">
        <label class="form-label" for="steamos_runtime_directory">Owned-session runtime directory</label>
        <input id="steamos_runtime_directory" v-model="config.steamos_runtime_directory" class="form-control" type="text" />
        <div class="form-text">Leave empty to create a private directory below the current user's XDG runtime.</div>
      </div>
      <div class="mb-3">
        <label class="form-label" for="steamos_game_gpu">Game GPU</label>
        <input id="steamos_game_gpu" v-model="config.steamos_game_gpu" class="form-control" type="text" placeholder="PCI BDF or /dev/dri/renderD*" />
      </div>
      <div class="mb-3">
        <label class="form-label" for="steamos_capture_gpu">Capture GPU</label>
        <input id="steamos_capture_gpu" v-model="config.steamos_capture_gpu" class="form-control" type="text" placeholder="PCI BDF or /dev/dri/renderD*" />
      </div>
      <div class="mb-3">
        <label class="form-label" for="steamos_encoder_gpu">Encoder GPU</label>
        <input id="steamos_encoder_gpu" v-model="config.steamos_encoder_gpu" class="form-control" type="text" placeholder="PCI BDF or /dev/dri/renderD*" />
        <div class="form-text">Game, capture, and encoder selectors must resolve to the same verified GPU.</div>
      </div>
      <div class="mb-3">
        <label class="form-label" for="steamos_pipewire_runtime">Host PipeWire runtime</label>
        <input id="steamos_pipewire_runtime" v-model="config.steamos_pipewire_runtime" class="form-control" type="text" />
      </div>
      <div class="mb-3">
        <label class="form-label" for="steamos_pipewire_remote">Host PipeWire remote</label>
        <input id="steamos_pipewire_remote" v-model="config.steamos_pipewire_remote" class="form-control" type="text" placeholder="pipewire-0" />
      </div>
      <div class="row">
        <div class="col-md-4 mb-3">
          <label class="form-label" for="steamos_pipewire_node_timeout_ms">PipeWire node timeout (ms)</label>
          <input id="steamos_pipewire_node_timeout_ms" v-model="config.steamos_pipewire_node_timeout_ms" class="form-control" type="number" min="1000" max="60000" />
        </div>
        <div class="col-md-4 mb-3">
          <label class="form-label" for="steamos_startup_timeout_seconds">Startup timeout (seconds)</label>
          <input id="steamos_startup_timeout_seconds" v-model="config.steamos_startup_timeout_seconds" class="form-control" type="number" min="1" max="60" />
        </div>
        <div class="col-md-4 mb-3">
          <label class="form-label" for="steamos_shutdown_timeout_seconds">Shutdown timeout (seconds)</label>
          <input id="steamos_shutdown_timeout_seconds" v-model="config.steamos_shutdown_timeout_seconds" class="form-control" type="number" min="1" max="60" />
        </div>
      </div>
      <div class="row">
        <div class="col-md-4 mb-3">
          <label class="form-label" for="steamos_max_frame_pixels">Maximum frame pixels</label>
          <input id="steamos_max_frame_pixels" v-model="config.steamos_max_frame_pixels" class="form-control" type="number" min="307200" max="33177600" />
        </div>
        <div class="col-md-4 mb-3">
          <label class="form-label" for="steamos_max_pixel_rate">Maximum pixels per second</label>
          <input id="steamos_max_pixel_rate" v-model="config.steamos_max_pixel_rate" class="form-control" type="number" min="9216000" max="2000000000" />
        </div>
        <div class="col-md-4 mb-3">
          <label class="form-label" for="steamos_max_buffer_megabytes">Maximum frame-buffer memory (MiB)</label>
          <input id="steamos_max_buffer_megabytes" v-model="config.steamos_max_buffer_megabytes" class="form-control" type="number" min="64" max="2048" />
        </div>
      </div>
      <div class="row">
        <div class="col-md-4 mb-3">
          <label class="form-label" for="steamos_default_width">Default width</label>
          <input id="steamos_default_width" v-model="config.steamos_default_width" class="form-control" type="number" min="640" max="7680" />
        </div>
        <div class="col-md-4 mb-3">
          <label class="form-label" for="steamos_default_height">Default height</label>
          <input id="steamos_default_height" v-model="config.steamos_default_height" class="form-control" type="number" min="480" max="4320" />
        </div>
        <div class="col-md-4 mb-3">
          <label class="form-label" for="steamos_default_fps">Default refresh rate</label>
          <input id="steamos_default_fps" v-model="config.steamos_default_fps" class="form-control" type="number" min="30" max="240" />
        </div>
      </div>
      <div class="row">
        <div class="col-md-6 mb-3">
          <label class="form-label" for="steamos_geometry_alignment">Geometry alignment</label>
          <select id="steamos_geometry_alignment" v-model="config.steamos_geometry_alignment" class="form-select">
            <option value="auto">Auto: minimally align coded dimensions</option>
            <option value="require_exact">Require exact client dimensions</option>
          </select>
          <div class="form-text">Auto reports any alignment adjustment; exact mode rejects an unrepresentable coded extent.</div>
        </div>
        <div class="col-md-6 mb-3">
          <label class="form-label" for="steamos_margin_input">Input in fitted-output margins</label>
          <select id="steamos_margin_input" v-model="config.steamos_margin_input" class="form-select">
            <option value="clamp">Clamp to the nearest visible edge</option>
            <option value="reject">Ignore input in margins</option>
          </select>
        </div>
      </div>
      <div class="mb-3 form-check">
        <input id="steamos_cleanup_orphan_sessions" v-model="config.steamos_cleanup_orphan_sessions" class="form-check-input" type="checkbox" true-value="enabled" false-value="disabled" />
        <label class="form-check-label" for="steamos_cleanup_orphan_sessions">Clean marked orphan owned-session runtimes at startup</label>
      </div>
    </details>
    <div class="alert alert-warning" role="status">Saving this policy requires restarting SteamShine before new Moonlight launches use it.</div>
  </div>
</template>
