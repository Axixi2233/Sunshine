<script setup>
import { ref } from 'vue'
import { Check, Copy, ExternalLink, Smartphone, Package, Grid2X2 } from '@lucide/vue'
import { streamingClients } from './client_downloads'

const icons = { ios: Smartphone, android: Package, harmonyos: Grid2X2 }
const copied = ref('')
const copyError = ref('')

/**
 * @brief Copy a client download URL and show an accessible result beside its card.
 * @param client Client destination selected by the user.
 */
async function copyLink(client) {
  copied.value = ''
  copyError.value = ''
  try {
    await navigator.clipboard.writeText(client.url)
    copied.value = client.id
  } catch {
    copyError.value = client.id
  }
}
</script>

<template>
  <section class="card my-4" aria-labelledby="client-downloads-title">
    <div class="card-body">
      <div class="d-flex align-items-center gap-3 mb-4">
        <img src="/assets/client-downloads/client-moonlight.png" width="56" height="56" alt="" class="client-brand-icon">
        <div>
          <h2 id="client-downloads-title" class="mb-1">{{ $t('client_downloads.title') }}</h2>
          <p class="mb-0">{{ $t('client_downloads.description') }}</p>
        </div>
      </div>
      <div class="row g-3">
        <div v-for="client in streamingClients" :key="client.id" class="col-12 col-md-4">
          <article class="client-download-tile h-100 d-flex flex-column">
            <div class="d-flex align-items-center gap-3 mb-3">
              <component :is="icons[client.id]" :size="24" class="client-platform-icon" aria-hidden="true" />
              <div>
                <div class="client-platform">{{ client.platform }}</div>
                <h3 class="h5 mb-0">{{ $t(`client_downloads.${client.id}_name`) }}</h3>
              </div>
            </div>
            <p class="small mb-3">{{ $t(`client_downloads.${client.id}_detail`) }}</p>
            <div class="text-center mt-auto mb-3">
              <img :src="`/assets/client-downloads/client-${client.id}-qr.svg`" width="132" height="132"
                   :alt="$t('client_downloads.qr_alt', { platform: client.platform })" class="client-qr">
              <div class="small text-muted mt-2">{{ $t('client_downloads.scan') }}</div>
            </div>
            <a :href="client.url" target="_blank" rel="noopener noreferrer" class="btn btn-primary w-100">
              <ExternalLink :size="16" class="icon" />{{ $t(`client_downloads.${client.id}_action`) }}
            </a>
            <button class="btn btn-outline-secondary w-100 mt-2" type="button" @click="copyLink(client)">
              <component :is="copied === client.id ? Check : Copy" :size="16" class="icon" />
              {{ $t(copied === client.id ? 'client_downloads.copied' : 'client_downloads.copy') }}
            </button>
            <div aria-live="polite" role="status">
              <p v-if="copyError === client.id" class="small text-danger mt-2 mb-0">{{ $t('client_downloads.copy_failed') }}</p>
              <span v-else-if="copied === client.id" class="visually-hidden">{{ $t('client_downloads.copied') }}</span>
            </div>
          </article>
        </div>
      </div>
      <div class="client-connection-guide mt-4">
        <h3 class="h5">{{ $t('client_downloads.guide_title') }}</h3>
        <p class="small">{{ $t('client_downloads.guide_description') }}</p>
        <ol class="row g-3 list-unstyled mb-0">
          <li v-for="step in 3" :key="step" class="col-12 col-md-4 d-flex align-items-start gap-2">
            <span class="client-step">{{ step }}</span>
            <span>{{ $t(`client_downloads.step_${step}`) }}
              <a v-if="step === 3" href="./pin">{{ $t('client_downloads.open_pairing') }}</a>
            </span>
          </li>
        </ol>
      </div>
    </div>
  </section>
</template>

<style scoped>
.client-brand-icon { border-radius: var(--radius-md); object-fit: contain; }
.client-download-tile { padding: 1.25rem; border: 1px solid var(--color-border); border-radius: var(--radius-lg); background: var(--color-bg-subtle); }
.client-platform { color: var(--color-text-muted); font-size: .8rem; margin-bottom: .2rem; }
.client-platform-icon { color: var(--color-primary); flex-shrink: 0; }
.client-qr { background: white; border-radius: .75rem; }
.client-connection-guide { border-top: 1px solid var(--color-border); padding-top: 1.25rem; }
.client-step { display: inline-flex; flex-shrink: 0; align-items: center; justify-content: center; width: 1.5rem; height: 1.5rem; border-radius: 50%; background: var(--color-primary); color: var(--color-on-primary); font-size: .8rem; font-weight: 600; }
</style>
