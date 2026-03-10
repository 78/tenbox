<template>
  <section class="features" id="features">
    <div class="container">
      <h2 class="section-title">{{ $t('features.title') }}</h2>
      <div class="features-grid">
        <div v-for="f in features" :key="f.key" class="feature-card">
          <div class="feature-icon" v-html="f.icon"></div>
          <h3 class="feature-title">{{ $t(`features.${f.key}.title`) }}</h3>
          <p class="feature-desc">{{ $t(`features.${f.key}.desc`) }}</p>
        </div>
      </div>
    </div>
  </section>
</template>

<script setup>
const features = [
  {
    key: 'security',
    icon: '<svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"/></svg>',
  },
  {
    key: 'oneclick',
    icon: '<svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M13 2L3 14h9l-1 8 10-12h-9l1-8z"/></svg>',
  },
  {
    key: 'gui',
    icon: '<svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="2" y="3" width="20" height="14" rx="2" ry="2"/><line x1="8" y1="21" x2="16" y2="21"/><line x1="12" y1="17" x2="12" y2="21"/></svg>',
  },
  {
    key: 'native',
    icon: '<svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="4" y="4" width="16" height="16" rx="2" ry="2"/><rect x="9" y="9" width="6" height="6"/><line x1="9" y1="1" x2="9" y2="4"/><line x1="15" y1="1" x2="15" y2="4"/><line x1="9" y1="20" x2="9" y2="23"/><line x1="15" y1="20" x2="15" y2="23"/><line x1="20" y1="9" x2="23" y2="9"/><line x1="20" y1="14" x2="23" y2="14"/><line x1="1" y1="9" x2="4" y2="9"/><line x1="1" y1="14" x2="4" y2="14"/></svg>',
  },
  {
    key: 'virtio',
    icon: '<svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polygon points="12 2 2 7 12 12 22 7 12 2"/><polyline points="2 17 12 22 22 17"/><polyline points="2 12 12 17 22 12"/></svg>',
  },
  {
    key: 'network',
    icon: '<svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><line x1="2" y1="12" x2="22" y2="12"/><path d="M12 2a15.3 15.3 0 014 10 15.3 15.3 0 01-4 10 15.3 15.3 0 01-4-10 15.3 15.3 0 014-10z"/></svg>',
  },
]
</script>

<style scoped>
.features {
  padding: var(--section-padding);
  background: var(--color-bg);
}

.features-grid {
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  gap: 24px;
  margin-top: 48px;
}

.feature-card {
  background: var(--color-card-bg);
  border: 1px solid var(--color-border);
  border-radius: var(--border-radius);
  padding: 32px;
  transition: all 0.25s ease;
}

.feature-card:hover {
  box-shadow: var(--color-card-shadow-hover);
  transform: translateY(-4px);
  border-color: var(--color-primary-light);
}

.feature-icon {
  width: 56px;
  height: 56px;
  border-radius: 12px;
  background: linear-gradient(135deg, rgba(37, 99, 235, 0.1), rgba(6, 182, 212, 0.1));
  display: flex;
  align-items: center;
  justify-content: center;
  margin-bottom: 20px;
  color: var(--color-primary);
}

.feature-title {
  font-size: 1.2rem;
  font-weight: 700;
  margin-bottom: 10px;
  color: var(--color-text);
}

.feature-desc {
  font-size: 0.95rem;
  line-height: 1.65;
  color: var(--color-text-light);
}

@media (max-width: 1024px) {
  .features-grid {
    grid-template-columns: repeat(2, 1fr);
  }
}

@media (max-width: 640px) {
  .features-grid {
    grid-template-columns: 1fr;
  }

  .feature-card {
    padding: 24px;
  }
}
</style>
