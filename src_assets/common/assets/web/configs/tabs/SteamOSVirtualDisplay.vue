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
