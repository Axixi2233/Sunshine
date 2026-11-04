<template>
  <nav class="navbar navbar-sunshine">
    <div class="container-fluid">
      <span class="navbar-brand d-flex align-items-center gap-2" title="StarCore星核">
        <img src="/images/logo-sunshine-45.png" width="45" height="45" alt="StarCore星核">
        <span>StarCore星核</span>
      </span>
      <ThemeToggle/>
    </div>
  </nav>
</template>

<script>
import ThemeToggle from './ThemeToggle.vue'

export default {
  components: {
    ThemeToggle,
  },
}
</script>
