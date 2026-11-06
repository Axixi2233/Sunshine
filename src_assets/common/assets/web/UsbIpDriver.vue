<script setup>
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { apiFetch } from './fetch_utils'
import { driverView } from './usbip_driver'

const props = defineProps({ warningOnly: Boolean })
const status = ref(null)
const pending = ref(false)
const message = ref('')
const view = computed(() => driverView(status.value))
const labels = { checking: '正在检测', healthy: '正常', missing: '未安装', stale: '异常：控制器没有绑定驱动', error: '异常：控制器不可用', helper_missing: '修复工具缺失，请重新安装星核', unknown: '检测失败，请重试' }
const results = {
  success: '修复完成，请重新连接串流。', reboot_required: 'Windows 要求重启电脑。请保存工作并手动重启，再重新连接串流。',
  admin_required: '需要管理员权限，请在电脑上允许授权后重试。', admin_cancelled: '已取消管理员授权，驱动未修复。',
  failed: '修复失败，请查看 config/usbip.log 获取详细原因。', busy: '已有修复操作正在运行。', streaming: '请先断开所有串流，再修复驱动。',
  helper_missing: '修复工具缺失，请重新安装星核。',
}
let timer
let disposed = false

/** @brief Refresh health and poll only while the bounded driver repair is running. */
async function refresh() {
  clearTimeout(timer)
  try {
    const response = await apiFetch('./api/usbip/driver/status')
    if (!response.ok) throw new Error('检测失败，请刷新页面重试。')
    const value = await response.json()
    if (disposed) return
    status.value = value
    if (value.repair_result) message.value = results[value.repair_result] || results.failed
    if (value.repairing) timer = setTimeout(refresh, 1200)
  } catch (error) {
    if (!disposed) { status.value = { state: 'unknown' }; message.value = error.message }
  }
}

/** @brief Submit a CSRF-protected repair of the fixed bundled driver; no device IDs are sent. */
async function repair() {
  if (pending.value || view.value.disabled) return
  pending.value = true
  message.value = '正在请求修复，请留意电脑上的管理员权限提示。'
  try {
    const tokenResponse = await apiFetch('./api/csrf-token')
    if (!tokenResponse.ok) throw new Error('无法取得安全凭据，请刷新页面重试。')
    const token = await tokenResponse.json()
    const response = await apiFetch('./api/usbip/driver/repair', {
      method: 'POST', headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': token.csrf_token }, body: '{}',
    })
    const result = await response.json()
    if (!response.ok || !result.status) throw new Error(results[result.error] || results.failed)
    await refresh()
  } catch (error) { message.value = error.message }
  finally { pending.value = false }
}

onMounted(refresh)
onUnmounted(() => { disposed = true; clearTimeout(timer) })
</script>

<template>
  <div v-if="props.warningOnly && view.warning" class="alert alert-warning my-4" role="status">
    <strong>USB/IP 外设共享驱动异常</strong>
    <p class="mb-2">外设可能无法由 Windows 接管，可前往疑难杂症检查并修复。</p>
    <a class="btn btn-warning" href="./troubleshooting#usbip">前往修复</a>
  </div>
  <section v-else-if="!props.warningOnly" id="usbip" class="card my-4" aria-labelledby="usbip-title">
    <div class="card-body">
      <h2 id="usbip-title">USB/IP 外设共享</h2>
      <p>修复 Windows 接管手机 USB 外设所需的驱动。请先断开所有串流，操作可能需要在电脑上授权管理员权限。</p>
      <p><strong>驱动状态：</strong>{{ labels[view.state] || labels.unknown }}</p>
      <p class="text-muted small">仅清理已确认没有驱动绑定的 USB/IP 虚拟控制器，并安装星核自带驱动。正常工作的控制器会保留。</p>
      <p v-if="status?.streaming" class="text-warning">当前正在串流，请断开后刷新状态。</p>
      <div class="d-flex gap-2 flex-wrap">
        <button class="btn btn-primary" :disabled="pending || view.disabled" @click="repair">{{ pending || status?.repairing ? '正在修复…' : '修复驱动' }}</button>
        <button class="btn btn-outline-secondary" :disabled="pending || status?.repairing" @click="refresh">刷新状态</button>
      </div>
      <p v-if="message" class="mt-3 mb-0" role="status" aria-live="polite">{{ message }}</p>
    </div>
  </section>
</template>
