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
        <label class="form-check-label" for="steamos_virtual_display_enabled">Enable SteamOS virtual display</label>
      </div>
      <div class="form-text">Create a SteamShine-managed display when the selected policy requires it.</div>
    </div>
    <div class="mb-3">
      <label class="form-label" for="steamos_session_source">Gamescope Session Source</label>
      <select id="steamos_session_source" v-model="config.steamos_session_source" class="form-select">
        <option value="auto">Auto: verified Game Mode first, then private</option>
        <option value="existing_gamescope">Existing Game Mode only</option>
        <option value="owned_private">SteamShine private session only</option>
      </select>
      <div class="form-text">Existing Game Mode is accepted only after process, PipeWire node, and GPU checks succeed.</div>
    </div>
    <div class="mb-3">
      <div class="form-check">
        <input id="steamos_keep_session_alive" v-model="config.steamos_keep_session_alive" class="form-check-input" type="checkbox" true-value="enabled" false-value="disabled" />
        <label class="form-check-label" for="steamos_keep_session_alive">Keep a SteamShine-owned session after disconnect</label>
      </div>
      <div class="form-text">Disconnect preserves the owned session for reconnect. Explicit stop destroys only SteamShine-owned resources.</div>
    </div>
    <div class="mb-3">
      <label class="form-label" for="steamos_existing_gamescope_pid">Existing Gamescope PID</label>
      <input id="steamos_existing_gamescope_pid" v-model="config.steamos_existing_gamescope_pid" class="form-control" type="number" min="0" step="1" />
      <div class="form-text">Use 0 for automatic selection. A nonzero PID must be the uniquely verified current-user Game Mode Gamescope.</div>
    </div>
    <div class="mb-3">
      <label class="form-label" for="steamos_local_presentation">Local Presentation</label>
      <select id="steamos_local_presentation" v-model="config.steamos_local_presentation" class="form-select">
        <option value="auto">Auto: mirror owned private sessions when a host display is available</option>
        <option value="off">Off: remote streaming only</option>
        <option value="mirror">Mirror SteamShine-owned private sessions locally</option>
      </select>
      <div class="form-text">Existing Game Mode retains its physical output and is never mirrored a second time.</div>
    </div>
    <div class="mb-3">
      <label class="form-label" for="steamos_virtual_display_mode">Virtual Display Mode</label>
      <select id="steamos_virtual_display_mode" class="form-select" v-model="config.steamos_virtual_display_mode">
        <option value="off">Off</option>
        <option value="auto">Auto</option>
        <option value="force">Force</option>
      </select>
      <div class="form-text">Force always creates and streams from a SteamShine-owned headless Gamescope display, even when a physical display or desktop Wayland session is available.</div>
    </div>
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
      <div class="mb-3 form-check">
        <input id="steamos_cleanup_orphan_sessions" v-model="config.steamos_cleanup_orphan_sessions" class="form-check-input" type="checkbox" true-value="enabled" false-value="disabled" />
        <label class="form-check-label" for="steamos_cleanup_orphan_sessions">Clean marked orphan owned-session runtimes at startup</label>
      </div>
    </details>
    <div class="alert alert-warning" role="status">Saving this policy requires restarting SteamShine before new Moonlight launches use it.</div>
  </div>
</template>
