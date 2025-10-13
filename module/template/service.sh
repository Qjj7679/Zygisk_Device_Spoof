#!/system/bin/sh
# Zygisk Device Spoof - Configuration Monitor Service
# This script monitors config file changes and syncs to cache

DEBUG=@DEBUG@

MODDIR=${0%/*}
CONFIG="${MODDIR}/config/config.json"
CACHE="${MODDIR}/cache/config_cached.json"
LOG_TAG="ZygiskDeviceSpoof"

# 创建缓存目录
mkdir -p "$(dirname "${CACHE}")"
chmod 0755 "$(dirname "${CACHE}")"

# 初始复制配置到缓存
if [ ! -f "${CACHE}" ] && [ -f "${CONFIG}" ]; then
  cp -f "${CONFIG}" "${CACHE}"
  chmod 0644 "${CACHE}"
  log -t ${LOG_TAG} "service: initial cache created"
fi

# 监控配置文件变更
while true; do
  # 优先使用 inotifywait
  if command -v inotifywait >/dev/null 2>&1; then
    inotifywait -e close_write,modify,moved_to,attrib "${CONFIG}" >/dev/null 2>&1
    if [ $? -eq 0 ]; then
      if cp -f "${CONFIG}" "${CACHE}"; then
        chmod 0644 "${CACHE}"
        log -t ${LOG_TAG} "service: cache updated (inotifywait)"
      else
        log -t ${LOG_TAG} "service: failed to copy config"
      fi
    fi
  else
    # 回退到轮询模式
    sleep 2
    # 检查配置文件是否比缓存新
    if [ -f "${CONFIG}" ] && { [ ! -f "${CACHE}" ] || [ "${CONFIG}" -nt "${CACHE}" ]; }; then
      if cp -f "${CONFIG}" "${CACHE}"; then
        chmod 0644 "${CACHE}"
        log -t ${LOG_TAG} "service: cache updated (polling)"
      else
        log -t ${LOG_TAG} "service: failed to copy config"
      fi
    fi
  fi
done &
