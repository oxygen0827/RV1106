#!/bin/sh
# GC1084 ISP 通路测试（板端执行）
#
# 背景：RAW10 直连 CIF（/dev/video0）已验证可取帧；ISP 路径（/dev/video11
# NV12）之前 rk_mpi_vi_test 输出全零。本脚本用两条更干净的通路重测 ISP：
#   demo : rkisp_demo（进程内 rkaiq，直接 V4L2 操作 ISP mainpath）
#   v4l2 : rkaiq_3A_server（后台 3A）+ v4l2-ctl 抓 ISP mainpath
#
# 用法: gc1084-isp-test.sh [demo|v4l2] [frame-count] [width] [height]
# 环境: KIT=/root/gc1084-isp-kit（rkisp_demo/rkaiq_3A_server/librkaiq.so/IQ 文件）
# 注意: /data 只有 2.2MB，输出放 /tmp（27MB tmpfs）
set -u

KIT=${KIT:-/root/gc1084-isp-kit}
OUT=${OUT:-/tmp/gc1084-isp}
MODE=${1:-demo}
COUNT=${2:-10}
W=${3:-1280}
H=${4:-720}
IQPATH=/etc/iqfiles
FAIL=0

mkdir -p "$OUT" "$IQPATH"

say() { echo "[isp-test] $*"; }

# ---- 0. IQ 文件就位（3A server 默认读 /etc/iqfiles/）----
if ! ls "$IQPATH"/gc1084*.json >/dev/null 2>&1; then
  say "copying gc1084 IQ files to $IQPATH"
  cp "$KIT"/gc1084*.json "$KIT"/gc1084*.bin "$IQPATH"/ 2>/dev/null || \
    say "WARN: no gc1084 IQ files in $KIT"
fi
ls -la "$IQPATH"/gc1084* 2>/dev/null || say "WARN: $IQPATH has no gc1084 IQ files"

# ---- 1. 现场信息 ----
say "modules:"
lsmod | grep -E "video_rkcif|video_rkisp|rockit|phy-rockchip-csi2" || true
media-ctl -d /dev/media0 -p > "$OUT/topology.txt" 2>&1 || true
say "gc1084 entity in media graph:"
grep -iE "gc1084" "$OUT/topology.txt" | head -5 || say "WARN: no gc1084 entity found"

# 自动探测 ISP mainpath/selfpath 节点
MP=$(grep -rl "rkisp_mainpath" /sys/class/video4linux/video*/name 2>/dev/null | head -1 | sed 's/.*video/video/;s|/name||')
SP=$(grep -rl "rkisp_selfpath" /sys/class/video4linux/video*/name 2>/dev/null | head -1 | sed 's/.*video/video/;s|/name||')
[ -n "$MP" ] || MP=video11
[ -n "$SP" ] || SP=video12
say "ISP mainpath=/dev/$MP selfpath=/dev/$SP"

# ---- 2. 清理旧进程，保证可重复 ----
killall rkaiq_3A_server 2>/dev/null || true
killall rkisp_demo 2>/dev/null || true
sleep 1

# ---- 3. 执行 ----
# rkisp_demo 动态依赖 librkaiq.so + librga.so；librga.so 在工具包内，
# 同时把系统库目录纳入路径以防其它依赖缺失
export LD_LIBRARY_PATH="$KIT:/oem/usr/lib:/usr/lib"
case "$MODE" in
  demo)
    say "mode=demo: rkisp_demo --device /dev/$MP --rkaiq --iqpath $IQPATH"
    "$KIT/rkisp_demo" \
        --device "/dev/$MP" --device2 "/dev/$SP" --rkaiq \
        --iqpath "$IQPATH" \
        --stream-to "$OUT/isp-demo.nv12" --stream-count "$COUNT" --stream-skip 5 \
        > "$OUT/demo.log" 2>&1 &
    PID=$!
    WAITED=0
    while kill -0 "$PID" 2>/dev/null && [ "$WAITED" -lt 120 ]; do
      sleep 1
      WAITED=$((WAITED + 1))
    done
    kill "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null
    echo "rkisp_demo rc=$? (killed after ${WAITED}s)" >> "$OUT/demo.log"
    tail -40 "$OUT/demo.log" | tee "$OUT/demo-tail.log"
    ;;
  v4l2)
    say "mode=v4l2: rkaiq_3A_server + v4l2-ctl on /dev/$MP"
    "$KIT/rkaiq_3A_server" > "$OUT/3a.log" 2>&1 &
    SERVER_PID=$!
    sleep 4
    v4l2-ctl -d "/dev/$MP" --set-fmt-video=width=$W,height=$H,pixelformat=NV12 \
        --stream-mmap=4 --stream-count="$COUNT" \
        --stream-to "$OUT/isp-v4l2.nv12" > "$OUT/v4l2.log" 2>&1 &
    PID=$!
    WAITED=0
    while kill -0 "$PID" 2>/dev/null && [ "$WAITED" -lt 60 ]; do
      sleep 1
      WAITED=$((WAITED + 1))
    done
    kill "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null
    echo "v4l2-ctl rc=$? (killed after ${WAITED}s)" >> "$OUT/v4l2.log"
    kill "$SERVER_PID" 2>/dev/null || true
    sleep 1
    tail -40 "$OUT/3a.log" | tee "$OUT/3a-tail.log"
    echo "--- v4l2-ctl log ---"
    tail -20 "$OUT/v4l2.log" | tee "$OUT/v4l2-tail.log"
    ;;
  *)
    echo "unknown mode: $MODE (demo|v4l2)" >&2
    exit 2
    ;;
esac

# ---- 4. 产物校验 ----
F="$OUT/isp-$MODE.nv12"
if [ -f "$F" ]; then
  SIZE=$(wc -c < "$F" 2>/dev/null || echo 0)
  EXP=$((W * H * 3 / 2 * COUNT))
  say "captured: $SIZE bytes, expected $EXP bytes for $COUNT x ${W}x${H} NV12"
  NZ=$(od -v -An -tu1 -N 65536 "$F" 2>/dev/null \
       | awk '{for(i=1;i<=NF;i++) if($i!=0) n++} END{print n+0}')
  say "nonzero bytes in first 64KB: $NZ"
  if [ "$SIZE" -lt "$((W * H * 3 / 2))" ]; then
    say "FAIL: captured file smaller than one frame"
    FAIL=1
  elif [ "$NZ" -eq 0 ]; then
    say "FAIL: frames are all zero (ISP pipeline not delivering data)"
    FAIL=1
  else
    say "PASS: ISP frames captured with real content"
  fi
  md5sum "$F" >> "$OUT/summary.txt" 2>/dev/null || true
else
  say "FAIL: no output file"
  FAIL=1
fi

# ---- 5. 附加诊断 ----
dmesg | grep -iE "cif|isp|gc1084|dphy|csi|rkaiq" | tail -80 > "$OUT/dmesg.txt" || true
CFIPROC=$(ls /proc | grep -i cif | head -1)
if [ -n "$CFIPROC" ]; then
  cat "/proc/$CFIPROC" > "$OUT/cif-proc.txt" 2>/dev/null || true
fi
say "results in $OUT/:"
ls -la "$OUT/"

[ "$FAIL" -eq 0 ] || exit 1
exit 0
