<script setup>
import { ref, computed, onMounted } from 'vue'
import Checkbox from "../../../Checkbox.vue";

const props = defineProps([
  'config',
])

const config = ref(props.config)
const status = ref(null)

onMounted(async () => {
  try {
    const response = await fetch('./api/frame-limiter/status', { credentials: 'include' })
    if (response.ok) {
      status.value = await response.json()
    }
  } catch (e) {
    // The settings stay usable without the status line
  }
})

// What the host can actually use right now, so a missing RTSS or NVIDIA driver is not
// discovered only by the stream stuttering.
const availability = computed(() => {
  if (!status.value) {
    return []
  }
  const rtss = status.value.rtss || {}
  return [
    { name: 'RTSS', ok: rtss.path_exists && rtss.hooks_found, detail: rtss.resolved_path },
    { name: 'NVIDIA', ok: status.value.nvidia_available && status.value.nvcp_ready, detail: '' },
  ]
})
</script>

<template>
  <div class="mb-3 accordion">
    <div class="accordion-item">
      <h2 class="accordion-header">
        <button class="accordion-button" type="button" data-bs-toggle="collapse" data-bs-target="#panelsFrameLimiter">
          {{ $t('config.frame_limiter_header') }}
        </button>
      </h2>
      <div id="panelsFrameLimiter" class="accordion-collapse collapse show">
        <div class="accordion-body">
          <Checkbox class="mb-3"
                    id="frame_limiter_enable"
                    locale-prefix="config"
                    v-model="config.frame_limiter_enable"
                    default="false"
          ></Checkbox>

          <div class="mb-3">
            <label for="frame_limiter_provider" class="form-label">{{ $t('config.frame_limiter_provider') }}</label>
            <select id="frame_limiter_provider" class="form-select" v-model="config.frame_limiter_provider">
              <option value="auto">{{ $t('config.frame_limiter_provider_auto') }}</option>
              <option value="rtss">RTSS</option>
              <option value="nvidia-control-panel">{{ $t('config.frame_limiter_provider_nvcp') }}</option>
              <option value="none">{{ $t('config.frame_limiter_provider_none') }}</option>
            </select>
            <div class="form-text">{{ $t('config.frame_limiter_provider_desc') }}</div>
          </div>

          <div class="mb-3">
            <label for="frame_limiter_fps_limit" class="form-label">{{ $t('config.frame_limiter_fps_limit') }}</label>
            <input type="number" min="0" max="1000" step="0.001" class="form-control" id="frame_limiter_fps_limit" placeholder="0"
                   v-model="config.frame_limiter_fps_limit" />
            <div class="form-text">{{ $t('config.frame_limiter_fps_limit_desc') }}</div>
          </div>

          <Checkbox class="mb-3"
                    id="frame_limiter_disable_vsync"
                    locale-prefix="config"
                    v-model="config.frame_limiter_disable_vsync"
                    default="false"
          ></Checkbox>

          <div class="mb-3">
            <label for="rtss_install_path" class="form-label">{{ $t('config.rtss_install_path') }}</label>
            <input type="text" class="form-control" id="rtss_install_path" placeholder="C:\Program Files (x86)\RivaTuner Statistics Server"
                   v-model="config.rtss_install_path" />
            <div class="form-text">{{ $t('config.rtss_install_path_desc') }}</div>
          </div>

          <div class="mb-3">
            <label for="rtss_frame_limit_type" class="form-label">{{ $t('config.rtss_frame_limit_type') }}</label>
            <select id="rtss_frame_limit_type" class="form-select" v-model="config.rtss_frame_limit_type">
              <option value="async">Async</option>
              <option value="front edge sync">Front edge sync</option>
              <option value="back edge sync">Back edge sync</option>
              <option value="nvidia reflex">NVIDIA Reflex</option>
            </select>
            <div class="form-text">{{ $t('config.rtss_frame_limit_type_desc') }}</div>
          </div>

          <Checkbox class="mb-3"
                    id="rtss_allow_virtual_display_override"
                    locale-prefix="config"
                    v-model="config.rtss_allow_virtual_display_override"
                    default="false"
          ></Checkbox>

          <div class="form-text" v-if="availability.length">
            {{ $t('config.frame_limiter_availability') }}
            <span v-for="provider in availability" :key="provider.name" class="me-3">
              <i class="fas" :class="provider.ok ? 'fa-circle-check text-success' : 'fa-circle-xmark text-secondary'"></i>
              {{ provider.name }}<span v-if="provider.ok && provider.detail"> ({{ provider.detail }})</span>
            </span>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>
