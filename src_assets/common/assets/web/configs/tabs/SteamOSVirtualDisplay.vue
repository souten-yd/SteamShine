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
      <label class="form-label" for="steamos_virtual_display_mode">Virtual Display Mode</label>
      <select id="steamos_virtual_display_mode" class="form-select" v-model="config.steamos_virtual_display_mode">
        <option value="off">Off</option>
        <option value="auto">Auto</option>
        <option value="force">Force</option>
      </select>
      <div class="form-text">Force always creates and streams from a SteamShine-owned headless Gamescope display, even when a physical display or desktop Wayland session is available.</div>
    </div>
    <div class="alert alert-warning" role="status">Saving this policy requires restarting SteamShine before new Moonlight launches use it.</div>
  </div>
</template>
