#!/usr/bin/env bash
# install.sh — MacBook Pro 16,1 (A2141) Asymmetric HWP & Thermal Driver Installer (macOS)
# Author: Juan José Monzón (@mylnzon58)

set -euo pipefail

echo "=========================================================================="
echo " MacBook Pro 16,1 (A2141) Asymmetric HWP & Thermal Kernel Driver Installer"
echo " Author: mylnzon58 (Juan Monzon)"
echo "=========================================================================="

if [[ "$(id -u)" -ne 0 ]]; then
    echo "[-] Ejecuta este instalador como root: sudo ./install.sh"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KEXT_NAME="A2141AsymmetricHWP.kext"
KEXT_DEST="/Library/Extensions/$KEXT_NAME"
PLIST_NAME="com.juanmonzon.a2141-hwp.plist"
PLIST_DEST="/Library/LaunchDaemons/$PLIST_NAME"
CLI_DEST="/usr/local/bin/a2141-ctl"

echo "[+] 1. Compilando A2141AsymmetricHWP.kext (Ring-0) y a2141-ctl (Daemon/CLI)..."
make -C "$SCRIPT_DIR" clean
make -C "$SCRIPT_DIR" all

echo "[+] 2. Deteniendo instancias previas del demonio y procesos parasitos (AVGTuneUp)..."
launchctl unload "$PLIST_DEST" 2>/dev/null || true
killall a2141-ctl 2>/dev/null || true
killall -9 AVGTuneUp 2>/dev/null || true

echo "[+] 3. Instalando binario de telemetría y demonio proactivo en $CLI_DEST..."
mkdir -p /usr/local/bin
cp "$SCRIPT_DIR/build/a2141-ctl" "$CLI_DEST"
chown root:wheel "$CLI_DEST"
chmod 755 "$CLI_DEST"

echo "[+] 4. Instalando extensión de kernel en $KEXT_DEST..."
rm -rf "$KEXT_DEST"
cp -R "$SCRIPT_DIR/build/$KEXT_NAME" "$KEXT_DEST"
chown -R root:wheel "$KEXT_DEST"
chmod -R 755 "$KEXT_DEST"
chmod 644 "$KEXT_DEST/Contents/Info.plist"

echo "[+] 5. Configurando LaunchDaemon proactivo ($PLIST_DEST)..."
cp "$SCRIPT_DIR/launchd/$PLIST_NAME" "$PLIST_DEST"
chown root:wheel "$PLIST_DEST"
chmod 644 "$PLIST_DEST"

echo "[+] 6. Activando driver Ring-0, pausando XCPM DVFS e iniciando demonio proactivo..."
if ! kextstat 2>/dev/null | grep -q "com.juanmonzon.driver.A2141AsymmetricHWP"; then
    if csrutil status 2>/dev/null | grep -qi "disabled"; then
        kmutil load -p "$KEXT_DEST" || kextload "$KEXT_DEST" || true
    fi
fi

launchctl load -w "$PLIST_DEST" 2>/dev/null || true
sleep 1
echo ""
"$CLI_DEST" status

rm -rf electron-app/out 2>/dev/null || true
