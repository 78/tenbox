<template>
  <section class="hero">
    <div class="hero-bg">
      <div class="hero-glow hero-glow-1"></div>
      <div class="hero-glow hero-glow-2"></div>
    </div>
    <div class="container hero-content">
      <div class="hero-image">
        <div class="hero-image-frame">
          <img src="/images/screenshot.png" :alt="$t('hero.title')" />
        </div>
      </div>
      <div class="hero-text">
        <h1 class="hero-title">{{ $t('hero.title') }}</h1>
        <p class="hero-subtitle">
          <span class="typewriter-text">{{ displayText }}</span>
          <span class="typewriter-cursor">|</span>
        </p>
        <p class="hero-desc">{{ $t('hero.description') }}</p>
        <div class="hero-actions">
          <a :href="downloadUrl" class="btn btn-large btn-hero">
            <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
              <path d="M21 15v4a2 2 0 01-2 2H5a2 2 0 01-2-2v-4" />
              <polyline points="7 10 12 15 17 10" />
              <line x1="12" y1="15" x2="12" y2="3" />
            </svg>
            {{ $t('hero.cta_win') }}
          </a>
          <a v-if="downloadUrlMac" :href="downloadUrlMac" class="btn btn-large btn-hero btn-hero-mac">
            <svg width="20" height="20" viewBox="0 0 24 24" fill="currentColor">
              <path d="M18.71 19.5c-.83 1.24-1.71 2.45-3.05 2.47-1.34.03-1.77-.79-3.29-.79-1.53 0-2 .77-3.27.82-1.31.05-2.3-1.32-3.14-2.53C4.25 17 2.94 12.45 4.7 9.39c.87-1.52 2.43-2.48 4.12-2.51 1.28-.02 2.5.87 3.29.87.78 0 2.26-1.07 3.8-.91.65.03 2.47.26 3.64 1.98-.09.06-2.17 1.28-2.15 3.81.03 3.02 2.65 4.03 2.68 4.04-.03.07-.42 1.44-1.38 2.83M13 3.5c.73-.83 1.94-1.46 2.94-1.5.13 1.17-.34 2.35-1.04 3.19-.69.85-1.83 1.51-2.95 1.42-.15-1.15.41-2.35 1.05-3.11z"/>
            </svg>
            {{ $t('hero.cta_mac') }}
          </a>
        </div>
        <p class="hero-meta">v{{ latestVersion }} · {{ $t('hero.requirements') }}</p>
        <p class="hero-qq">{{ $t('hero.qq_group') }}：{{ $t('hero.qq_group_number') }}</p>
      </div>
    </div>
  </section>
</template>

<script setup>
import { ref, onMounted, onUnmounted, watch } from 'vue'

/* global __APP_VERSION__, __APP_DOWNLOAD_URL__, __APP_DOWNLOAD_URL_MAC__ */
import { useI18n } from 'vue-i18n'

const { tm, locale } = useI18n()

const downloadUrl = __APP_DOWNLOAD_URL__
const downloadUrlMac = __APP_DOWNLOAD_URL_MAC__
const latestVersion = __APP_VERSION__

const displayText = ref('')
let timerId = null
let taglineIndex = 0
let charIndex = 0
let phase = 'typing'

const TYPING_SPEED = 80
const DELETING_SPEED = 40
const PAUSE_AFTER_TYPING = 2000
const PAUSE_AFTER_DELETING = 500

function getTaglines() {
  return tm('hero.taglines') || []
}

function tick() {
  const taglines = getTaglines()
  if (!taglines.length) return

  const current = taglines[taglineIndex % taglines.length]

  if (phase === 'typing') {
    charIndex++
    displayText.value = current.slice(0, charIndex)
    if (charIndex >= current.length) {
      phase = 'pausing'
      timerId = setTimeout(tick, PAUSE_AFTER_TYPING)
      return
    }
    timerId = setTimeout(tick, TYPING_SPEED)
  } else if (phase === 'pausing') {
    phase = 'deleting'
    timerId = setTimeout(tick, DELETING_SPEED)
  } else if (phase === 'deleting') {
    charIndex--
    displayText.value = current.slice(0, charIndex)
    if (charIndex <= 0) {
      phase = 'waiting'
      timerId = setTimeout(tick, PAUSE_AFTER_DELETING)
      return
    }
    timerId = setTimeout(tick, DELETING_SPEED)
  } else if (phase === 'waiting') {
    taglineIndex = (taglineIndex + 1) % taglines.length
    charIndex = 0
    phase = 'typing'
    timerId = setTimeout(tick, TYPING_SPEED)
  }
}

function reset() {
  if (timerId) clearTimeout(timerId)
  taglineIndex = 0
  charIndex = 0
  phase = 'typing'
  displayText.value = ''
  timerId = setTimeout(tick, TYPING_SPEED)
}

onMounted(() => {
  reset()
})

onUnmounted(() => {
  if (timerId) clearTimeout(timerId)
})

watch(locale, () => {
  reset()
})
</script>

<style scoped>
.hero {
  position: relative;
  min-height: 100vh;
  display: flex;
  align-items: center;
  overflow: hidden;
  background: var(--color-bg-dark);
}

.hero-bg {
  position: absolute;
  inset: 0;
  overflow: hidden;
}

.hero-glow {
  position: absolute;
  border-radius: 50%;
  filter: blur(100px);
  opacity: 0.3;
}

.hero-glow-1 {
  width: 600px;
  height: 600px;
  background: var(--color-primary);
  top: -200px;
  right: -100px;
}

.hero-glow-2 {
  width: 400px;
  height: 400px;
  background: var(--color-accent);
  bottom: -100px;
  left: -100px;
}

.hero-content {
  position: relative;
  display: grid;
  grid-template-columns: 1.1fr 0.9fr;
  gap: 80px;
  align-items: center;
  padding-top: calc(var(--nav-height) + 40px);
  padding-bottom: 60px;
}

.hero-text {
  color: var(--color-text-inverse);
}

.hero-title {
  font-size: 4rem;
  font-weight: 800;
  line-height: 1.1;
  margin-bottom: 8px;
  letter-spacing: -0.02em;
}

.hero-subtitle {
  font-size: 1.5rem;
  font-weight: 500;
  color: var(--color-accent);
  margin-bottom: 24px;
  font-family: var(--font-mono);
  min-height: 2.2em;
}

.typewriter-cursor {
  display: inline-block;
  margin-left: 2px;
  animation: blink 0.7s step-end infinite;
}

@keyframes blink {
  0%, 100% { opacity: 1; }
  50% { opacity: 0; }
}

.hero-desc {
  font-size: 1.2rem;
  line-height: 1.7;
  color: rgba(241, 245, 249, 0.8);
  margin-bottom: 40px;
  max-width: 500px;
}

.hero-actions {
  display: flex;
  gap: 16px;
  flex-wrap: wrap;
}

.btn-hero-mac {
  background: rgba(255, 255, 255, 0.1);
  border: 1px solid rgba(255, 255, 255, 0.2);
}

.btn-hero-mac:hover {
  background: rgba(255, 255, 255, 0.18);
}

.hero-qq {
  margin-top: 8px;
  font-size: 0.85rem;
  color: rgba(6, 182, 212, 0.85);
}

.hero-meta {
  margin-top: 16px;
  font-size: 0.85rem;
  color: rgba(241, 245, 249, 0.45);
}

.hero-image {
  display: flex;
  justify-content: center;
}

.hero-image-frame {
  border-radius: 4px;
  overflow: hidden;
  box-shadow: 0 24px 80px rgba(0, 0, 0, 0.4), 0 0 0 1px rgba(255, 255, 255, 0.08);
  transition: transform 0.3s ease;
}

.hero-image-frame:hover {
  transform: translateY(-4px);
}

.hero-image-frame img {
  width: 100%;
  display: block;
}

@media (max-width: 1024px) {
  .hero-content {
    grid-template-columns: 1fr;
    text-align: center;
    gap: 48px;
  }

  .hero-title {
    font-size: 3rem;
  }

  .hero-desc {
    margin: 0 auto 32px;
  }

  .hero-actions {
    justify-content: center;
  }
}

@media (max-width: 768px) {
  .hero {
    min-height: auto;
    padding-top: 0;
  }

  .hero-content {
    padding-top: calc(var(--nav-height) + 32px);
    padding-bottom: 48px;
    gap: 32px;
  }

  .hero-title {
    font-size: 2.5rem;
  }

  .hero-subtitle {
    font-size: 1.2rem;
  }

  .hero-desc {
    font-size: 1rem;
  }
}
</style>
