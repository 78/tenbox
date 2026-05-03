<template>
  <nav class="navbar" :class="{ scrolled }">
    <div class="container nav-inner">
      <a href="#" class="nav-brand">
        <img src="../assets/logo.png" alt="TenBox" class="nav-logo" />
        <span class="nav-name">TenBox</span>
      </a>
      <div class="nav-actions">
        <a
          href="https://my.tenbox.ai/"
          target="_blank"
          rel="noopener noreferrer"
          class="nav-link nav-link-icon"
          :title="$t('nav.console')"
        >
          <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
            <rect x="2" y="4" width="20" height="16" rx="2" ry="2"/>
            <polyline points="6 9 9 12 6 15"/>
            <line x1="12" y1="15" x2="17" y2="15"/>
          </svg>
          <span>{{ $t('nav.console') }}</span>
        </a>
        <a
          href="https://github.com/78/tenbox"
          target="_blank"
          rel="noopener noreferrer"
          class="nav-link nav-link-icon"
          title="GitHub"
        >
          <svg width="20" height="20" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true">
            <path d="M12 0c-6.626 0-12 5.373-12 12 0 5.302 3.438 9.8 8.207 11.387.599.111.793-.261.793-.577v-2.234c-3.338.726-4.033-1.416-4.033-1.416-.546-1.387-1.333-1.756-1.333-1.756-1.089-.745.083-.729.083-.729 1.205.084 1.839 1.237 1.839 1.237 1.07 1.834 2.807 1.304 3.492.997.107-.775.418-1.305.762-1.604-2.665-.305-5.467-1.334-5.467-5.931 0-1.311.469-2.381 1.236-3.221-.124-.303-.535-1.524.117-3.176 0 0 1.008-.322 3.301 1.23.957-.266 1.983-.399 3.003-.404 1.02.005 2.047.138 3.006.404 2.291-1.552 3.297-1.23 3.297-1.23.653 1.653.242 2.874.118 3.176.77.84 1.235 1.911 1.235 3.221 0 4.609-2.807 5.624-5.479 5.921.43.372.823 1.102.823 2.222v3.293c0 .319.192.694.801.576 4.765-1.589 8.199-6.086 8.199-11.386 0-6.627-5.373-12-12-12z"/>
          </svg>
          <span>GitHub</span>
        </a>
        <button class="lang-toggle" @click="toggleLocale" :title="localeName">
          {{ localeName }}
        </button>
      </div>
    </div>
  </nav>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted } from 'vue'
import { useI18n } from 'vue-i18n'

const { locale } = useI18n()
const scrolled = ref(false)

const localeName = computed(() => (locale.value === 'zh-CN' ? 'EN' : '中文'))

function toggleLocale() {
  const next = locale.value === 'zh-CN' ? 'en-US' : 'zh-CN'
  locale.value = next
  localStorage.setItem('tenbox-locale', next)
  document.documentElement.lang = next === 'zh-CN' ? 'zh' : 'en'
}

function onScroll() {
  scrolled.value = window.scrollY > 10
}

onMounted(() => window.addEventListener('scroll', onScroll, { passive: true }))
onUnmounted(() => window.removeEventListener('scroll', onScroll))
</script>

<style scoped>
.navbar {
  position: fixed;
  top: 0;
  left: 0;
  right: 0;
  height: var(--nav-height);
  z-index: 100;
  transition: all var(--transition);
  background: rgba(255, 255, 255, 0.7);
  backdrop-filter: blur(8px);
  border-bottom: 1px solid transparent;
}

.navbar.scrolled {
  background: rgba(255, 255, 255, 0.92);
  backdrop-filter: blur(12px);
  border-bottom-color: var(--color-border);
  box-shadow: 0 1px 4px rgba(15, 23, 42, 0.04);
}

.nav-inner {
  display: flex;
  align-items: center;
  justify-content: space-between;
  height: 100%;
}

.nav-brand {
  display: flex;
  align-items: center;
  gap: 10px;
}

.nav-logo {
  width: 32px;
  height: 32px;
  border-radius: 6px;
}

.nav-name {
  font-size: 1.25rem;
  font-weight: 700;
  color: var(--color-text);
}

.nav-actions {
  display: flex;
  align-items: center;
  gap: 1.5rem;
}

.nav-link {
  color: var(--color-text-light);
  font-size: 0.875rem;
  font-weight: 600;
  padding: 0.5rem 0.75rem;
  border-radius: 8px;
  transition: color var(--transition), background var(--transition);
}

.nav-link-icon {
  display: inline-flex;
  align-items: center;
  gap: 0.5rem;
}

.nav-link:hover {
  color: var(--color-primary);
  background: rgba(15, 23, 42, 0.04);
}

.lang-toggle {
  padding: 0.5rem 1rem;
  border-radius: 8px;
  font-size: 0.875rem;
  font-weight: 600;
  color: var(--color-text-light);
  border: 1px solid var(--color-border);
  background: #fff;
  transition: all var(--transition);
}

.lang-toggle:hover {
  color: var(--color-primary);
  border-color: var(--color-primary);
  background: rgba(37, 99, 235, 0.04);
}

@media (max-width: 768px) {
  .nav-name {
    font-size: 1.1rem;
  }
}
</style>
