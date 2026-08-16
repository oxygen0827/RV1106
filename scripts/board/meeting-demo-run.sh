#!/bin/sh
# 板端运行 meeting_demo（前置：麦克风/喇叭参数调教 + 前台运行）
# 环境: APP=/root/meeting_demo, SERVER=wss://..., MODE=listen, CAFILE=/root/bin/cacert.pem
set -u

APP=${APP:-/root/meeting_demo}
SERVER=${SERVER:-wss://clare.vinex.top/voice-api}
MODE=${MODE:-listen}
CAFILE=${CAFILE:-/root/bin/cacert.pem}
EXTRA_ARGS=${EXTRA_ARGS:-}

# 麦克风：单端模式 + 数字音量 185（0.5dB/步，-12dBFS 附近，大声不削波）
amixer -c 0 cset numid=19 1 >/dev/null 2>&1 || true
amixer -c 0 cset numid=6 185 >/dev/null 2>&1 || true
amixer -c 0 cset numid=7 185 >/dev/null 2>&1 || true
# 喇叭：DAC LINEOUT 音量（0-30，15 适中）
amixer -c 0 cset numid=24 15 >/dev/null 2>&1 || true

cd "$APP" || exit 1
LD_LIBRARY_PATH="$APP:/oem/usr/lib:/usr/lib" \
    ./meeting_demo --server "$SERVER" --mode "$MODE" --cafile "$CAFILE" $EXTRA_ARGS
rc=$?
echo "meeting_demo rc=$rc"
exit $rc
