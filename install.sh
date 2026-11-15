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

echo "[+] 1. Preparando binarios de A2141AsymmetricHWP.kext (Ring-0) y a2141-ctl (Daemon/CLI)..."
if [[ "${1:-}" == "--compile" ]]; then
    echo "    -> Compilando desde código fuente (--compile)..."
    make -C "$SCRIPT_DIR" clean
    make -C "$SCRIPT_DIR" all
    BIN_DIR="$SCRIPT_DIR/build"
elif [[ -f "$SCRIPT_DIR/prebuilt/a2141-ctl" && -d "$SCRIPT_DIR/prebuilt/$KEXT_NAME" ]]; then
    echo "    -> Usando binarios precompilados listos para instalar (prebuilt/)..."
    BIN_DIR="$SCRIPT_DIR/prebuilt"
    # Si tiene herramientas de Xcode instaladas y desea recompilar el kext para su kernel exacto:
    if command -v xcrun >/dev/null 2>&1 && [[ -d "$(xcrun --show-sdk-path 2>/dev/null)/System/Library/Frameworks/Kernel.framework" ]]; then
        if make -C "$SCRIPT_DIR" all >/dev/null 2>&1; then
            echo "    -> Binario ajustado automáticamente al kernel actual ($(uname -r))."
            BIN_DIR="$SCRIPT_DIR/build"
        fi
    fi
elif [[ -f "$SCRIPT_DIR/build/a2141-ctl" && -d "$SCRIPT_DIR/build/$KEXT_NAME" ]]; then
    echo "    -> Usando binarios existentes en build/..."
    BIN_DIR="$SCRIPT_DIR/build"
else
    echo "    -> Compilando desde código fuente..."
    make -C "$SCRIPT_DIR" clean
    make -C "$SCRIPT_DIR" all
    BIN_DIR="$SCRIPT_DIR/build"
fi

echo "[+] 2. Deteniendo instancias previas del demonio y procesos parasitos (AVGTuneUp)..."
launchctl unload "$PLIST_DEST" 2>/dev/null || true
killall a2141-ctl 2>/dev/null || true
killall -9 AVGTuneUp 2>/dev/null || true

echo "[+] 3. Instalando binario de telemetría y demonio proactivo en $CLI_DEST..."
mkdir -p /usr/local/bin
cp "$BIN_DIR/a2141-ctl" "$CLI_DEST"
chown root:wheel "$CLI_DEST"
chmod 755 "$CLI_DEST"
xattr -cr "$CLI_DEST" 2>/dev/null || true

echo "[+] 4. Instalando extensión de kernel en $KEXT_DEST..."
rm -rf "$KEXT_DEST"
cp -R "$BIN_DIR/$KEXT_NAME" "$KEXT_DEST"
chown -R root:wheel "$KEXT_DEST"
chmod -R 755 "$KEXT_DEST"
chmod 644 "$KEXT_DEST/Contents/Info.plist"
xattr -cr "$KEXT_DEST" 2>/dev/null || true

if [[ -d "$SCRIPT_DIR/prebuilt/A2141Monitor.app" ]]; then
    echo "[+] 4b. Instalando aplicación visual A2141 Monitor en /Applications/A2141Monitor.app..."
    rm -rf /Applications/A2141Monitor.app
    cp -R "$SCRIPT_DIR/prebuilt/A2141Monitor.app" /Applications/A2141Monitor.app
    chown -R root:wheel /Applications/A2141Monitor.app
    chmod -R 755 /Applications/A2141Monitor.app
    xattr -cr /Applications/A2141Monitor.app 2>/dev/null || true
fi

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
