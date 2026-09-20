#!/bin/sh
# GC1084 MIPI 链路稳定性诊断（板端执行）
#
# 背景：RAW10 直连 CIF 连续抓 30 帧时，约 21 帧（0.7s）后帧为空/断流。
# 本脚本逐轮抓帧并采集 rkcif 驱动在 /proc/<cif> 暴露的中断统计
# （frame dma end / csi overflow / bandwidth lack / size err / all err），
# 区分两类故障：
#   A. CIF 收不到数据（overflow/bwidth/size/err 计数增长）→ DPHY/信号问题
#   B. CIF 无错误但 frame dma end 数 < 预期 → 传感器侧停流/帧头丢失
# 每轮结束后再做 1 帧重测，判断链路是否能恢复（热重连 vs 需要断电）。
#
# 用法: gc1084-mipi-diag.sh [frames-per-round] [rounds]
# 注意: /data 只有 2.2MB，输出放 /tmp（27MB tmpfs）
set -u

OUT=${OUT:-/tmp/gc1084-mipi}
FRAMES=${1:-30}
ROUNDS=${2:-3}
W=1280
H=720
mkdir -p "$OUT"

# 探测 CIF raw 采集节点（stream_cif_mipi_id0 = 传感器直连的 raw 流；
# 注意不能用 "rkcif" 匹配，会误中 rkcif_tools_id2 等非采集节点）
RAW=$(grep -rl "cif_mipi_id0" /sys/class/video4linux/video*/name 2>/dev/null | head -1 | sed 's/.*video/video/;s|/name||')
[ -n "$RAW" ] || RAW=video0
CFIPROC=$(ls /proc | grep -i cif | head -1)
[ -n "$CFIPROC" ] && CFIPROC_PATH="/proc/$CFIPROC" || CFIPROC_PATH=""

# 每帧字节数（stride * height），从 v4l2 实际格式读取
BPL=$(v4l2-ctl -d "/dev/$RAW" --get-fmt-video 2>/dev/null | grep -oE "BytesperLine: *[0-9]+" | grep -oE "[0-9]+$")
[ -n "$BPL" ] || BPL=1792
PER_FRAME=$((BPL * H))
say() { echo "[mipi-diag] $*"; }

snapshot() { # $1 = label
  echo "=== $1 ==="
  if [ -n "$CFIPROC_PATH" ]; then
    cat "$CFIPROC_PATH"
  else
    echo "(no cif proc entry found)"
  fi
  grep -E "cif|isp" /proc/interrupts 2>/dev/null | head -5 || true
}

# BusyBox i2cget 只支持 8 位寄存器地址，GC1084 是 16 位，改查驱动绑定状态
sensor_state() {
  if [ -d /sys/bus/i2c/devices/4-0037 ]; then
    echo "sensor 4-0037 present: $(cat /sys/bus/i2c/devices/4-0037/name 2>/dev/null)"
  else
    echo "sensor 4-0037 MISSING (I2C/power problem)"
  fi
}

say "RAW node=/dev/$RAW  cif proc=${CFIPROC_PATH:-none}  bytes/frame=$PER_FRAME"
echo "GC1084 MIPI diagnostic: ${ROUNDS} rounds x ${FRAMES} frames"

for r in $(seq 1 "$ROUNDS"); do
  echo "--- round $r ---"
  snapshot "before round $r"
  # BusyBox 无 timeout 命令：后台运行 + 轮询超时
  rm -f "$OUT/frames-$r.raw"
  v4l2-ctl -d "/dev/$RAW" \
      --set-fmt-video=width=$W,height=$H,pixelformat=BA10 \
      --stream-mmap=3 --stream-count="$FRAMES" --stream-to="$OUT/frames-$r.raw" \
      > "$OUT/capture-$r.log" 2>&1 &
  PID=$!
  WAITED=0
  while kill -0 "$PID" 2>/dev/null && [ "$WAITED" -lt 90 ]; do
    sleep 1
    WAITED=$((WAITED + 1))
  done
  if kill -0 "$PID" 2>/dev/null; then
    echo "capture timed out after ${WAITED}s, killing"
    kill "$PID" 2>/dev/null || true
  fi
  wait "$PID" 2>/dev/null
  echo "v4l2-ctl rc=$? (0=全部 ${FRAMES} 帧完成)"
  SIZE=$(wc -c < "$OUT/frames-$r.raw" 2>/dev/null || echo 0)
  FULL=$((PER_FRAME * FRAMES))
  FULL_FRAMES=$((SIZE / PER_FRAME))
  echo "bytes=$SIZE expected=$FULL full_frames=$FULL_FRAMES tail_bytes=$((SIZE % PER_FRAME))"
  snapshot "after round $r"
  sensor_state
  # 1 帧重测：链路能否热恢复
  rm -f "$OUT/retest-$r.raw"
  v4l2-ctl -d "/dev/$RAW" \
      --set-fmt-video=width=$W,height=$H,pixelformat=BA10 \
      --stream-mmap=3 --stream-count=1 --stream-to="$OUT/retest-$r.raw" \
      > "$OUT/retest-$r.log" 2>&1 &
  PID=$!
  WAITED=0
  while kill -0 "$PID" 2>/dev/null && [ "$WAITED" -lt 30 ]; do
    sleep 1
    WAITED=$((WAITED + 1))
  done
  kill "$PID" 2>/dev/null || true
  wait "$PID" 2>/dev/null || true
  RSIZE=$(wc -c < "$OUT/retest-$r.raw" 2>/dev/null || echo 0)
  if [ "$RSIZE" -ge "$PER_FRAME" ]; then
    echo "retest: 1 frame OK (link recovered on re-stream)"
  else
    echo "retest: FAILED ($RSIZE bytes) - link needs power cycle / FPC reseat"
  fi
  sleep 1
done

dmesg | grep -iE "cif|isp|gc1084|dphy|csi" | tail -80 > "$OUT/dmesg.txt" 2>/dev/null || true
echo "diagnostics saved to $OUT/:"
ls -la "$OUT/"
echo "next: inspect $OUT/capture-*.log and $OUT/dmesg.txt; count per-round full_frames"
