#!/bin/sh
# 板端运行 camera_yolo（3A server + 实时 YOLO 同一会话）
# 用法: gc1084-yolo-run.sh [count] [device]
# 环境: KIT=/root/gc1084-isp-kit, APP=/root/camera-yolo
set -u

KIT=${KIT:-/root/gc1084-isp-kit}
APP=${APP:-/root/camera-yolo}
OUT=${OUT:-/tmp/camera-yolo-frames}
COUNT=${1:-100}
DEVICE=${2:-/dev/video11}

export LD_LIBRARY_PATH="$KIT:/oem/usr/lib:/usr/lib"

killall rkaiq_3A_server 2>/dev/null || true
rm -rf "$OUT"
mkdir -p "$OUT"

# 确保 IQ 文件就位
mkdir -p /etc/iqfiles
cp "$KIT"/gc1084*.json "$KIT"/gc1084*.bin /etc/iqfiles/ 2>/dev/null || true

# 启动 3A server（本会话内后台，随脚本结束清理）
"$KIT/rkaiq_3A_server" > /root/3a-server.log 2>&1 &
SERVER_PID=$!
sleep 4

# 运行 camera_yolo（自带连续取流，AE 在运行中收敛）
cd "$APP" || exit 1
LD_LIBRARY_PATH="$APP/lib:$KIT:/oem/usr/lib:/usr/lib" \
    ./camera_yolo ./model/yolov5.rknn "$DEVICE" \
    --count "$COUNT" --dump-prefix "$OUT/frame" --every 10
rc=$?

kill "$SERVER_PID" 2>/dev/null || true
sleep 1
echo "camera_yolo rc=$rc"
exit $rc
