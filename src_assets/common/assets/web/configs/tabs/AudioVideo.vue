<script setup>
import {ref} from 'vue'
import {$tp} from '../../platform-i18n'
import PlatformLayout from '../../PlatformLayout.vue'
import AdapterNameSelector from './audiovideo/AdapterNameSelector.vue'
import DisplayOutputSelector from './audiovideo/DisplayOutputSelector.vue'
import DisplayDeviceOptions from "./audiovideo/DisplayDeviceOptions.vue";
import DisplayModesSettings from "./audiovideo/DisplayModesSettings.vue";
import Checkbox from "../../Checkbox.vue";

const props = defineProps([
  'platform',
  'config',
])

const config = ref(props.config)
</script>

<template>
  <div id="audio-video" class="config-page">
    <!-- Audio Sink -->
    <div class="mb-3">
      <label for="audio_sink" class="form-label">{{ $t('config.audio_sink') }}</label>
      <input type="text" class="form-control" id="audio_sink"
             :placeholder="$tp('config.audio_sink_placeholder', 'alsa_output.pci-0000_09_00.3.analog-stereo')"
             v-model="config.audio_sink" />
      <div class="form-text">
        {{ $tp('config.audio_sink_desc') }}<br>
        <PlatformLayout :platform="platform">
          <template #windows>
            <pre>tools\audio-info.exe</pre>
          </template>
          <template #freebsd>
            <pre>pacmd list-sinks | grep "name:"</pre>
            <pre>pactl info | grep Source</pre>
          </template>
          <template #linux>
            <pre>pacmd list-sinks | grep "name:"</pre>
            <pre>pactl info | grep Source</pre>
          </template>
          <template #macos>
            <a href="https://github.com/mattingalls/Soundflower" target="_blank">Soundflower</a><br>
            <a href="https://github.com/ExistentialAudio/BlackHole" target="_blank">BlackHole</a>.
          </template>
        </PlatformLayout>
      </div>
    </div>


    <PlatformLayout :platform="platform">
      <template #windows>
        <!-- Virtual Sink -->
        <div class="mb-3">
          <label for="virtual_sink" class="form-label">{{ $t('config.virtual_sink') }}</label>
          <input type="text" class="form-control" id="virtual_sink" :placeholder="$t('config.virtual_sink_placeholder')"
                 v-model="config.virtual_sink" />
          <div class="form-text">{{ $t('config.virtual_sink_desc') }}</div>
        </div>

        <!-- Install Steam Audio Drivers -->
        <Checkbox class="mb-3"
                  id="install_steam_audio_drivers"
                  locale-prefix="config"
                  v-model="config.install_steam_audio_drivers"
                  default="true"
        ></Checkbox>

        <!-- Client Microphone Uplink -->
        <Checkbox class="mb-3"
                  id="mic_uplink"
                  locale-prefix="config"
                  v-model="config.mic_uplink"
                  default="true"
        ></Checkbox>

        <div class="mb-3">
          <label for="mic_uplink_device" class="form-label">{{ $t('config.mic_uplink_device') }}</label>
          <input type="text" class="form-control" id="mic_uplink_device" :placeholder="$t('config.mic_uplink_device_placeholder')"
                 v-model="config.mic_uplink_device" />
          <div class="form-text">{{ $t('config.mic_uplink_device_desc') }}</div>
        </div>
      </template>
    </PlatformLayout>

    <!-- Disable Audio -->
    <Checkbox class="mb-3"
              id="stream_audio"
              locale-prefix="config"
              v-model="config.stream_audio"
              default="true"
    ></Checkbox>

    <AdapterNameSelector
        :platform="platform"
        :config="config"
    />

    <DisplayOutputSelector
      :platform="platform"
      :config="config"
    />

    <PlatformLayout :platform="platform">
      <template #windows>
        <div class="card mb-3"><div class="card-body">
          <Checkbox id="virtual_display_enabled" locale-prefix="config"
                    v-model="config.virtual_display_enabled" default="false" />
          <div class="mt-3">
            <label for="virtual_display_mode" class="form-label">{{ $t('config.virtual_display_mode') }}</label>
            <select id="virtual_display_mode" class="form-select" v-model.number="config.virtual_display_mode"
                    :disabled="!['enabled', 'true', true].includes(config.virtual_display_enabled)">
              <option :value="1">{{ $t('config.virtual_display_mode_extend') }}</option>
              <option :value="2">{{ $t('config.virtual_display_mode_exclusive') }}</option>
            </select>
            <div class="form-text">{{ $t('config.virtual_display_mode_desc') }}</div>
          </div>
        </div></div>
      </template>
    </PlatformLayout>

    <DisplayDeviceOptions
      :platform="platform"
      :config="config"
    />

    <!-- Display Modes -->
    <DisplayModesSettings
        :platform="platform"
        :config="config"
    />

  </div>
</template>

<style scoped>
</style>
