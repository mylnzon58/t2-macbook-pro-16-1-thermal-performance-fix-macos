#!/usr/bin/env bash
# uninstall.sh — MacBook Pro 16,1 (A2141) Asymmetric HWP & Thermal Driver Uninstaller (macOS)
# Author: Juan José Monzón (@mylnzon58)

set -euo pipefail

echo "=========================================================================="
echo " Desinstalando A2141AsymmetricHWP.kext y restaurando estado de fábrica..."
echo "=========================================================================="

if [[ "$(id -u)" -ne 0 ]]; then
    echo "[-] Ejecuta este script como root: sudo ./uninstall.sh"
    exit 1
fi

KEXT_DEST="/Library/Extensions/A2141AsymmetricHWP.kext"
PLIST_DEST="/Library/LaunchDaemons/com.juanmonzon.a2141-hwp.plist"
CLI_DEST="/usr/local/bin/a2141-ctl"

echo "[+] 1. Deteniendo LaunchDaemon..."
launchctl unload -w "$PLIST_DEST" 2>/dev/null || true
rm -f "$PLIST_DEST"

echo "[+] 2. Descargando A2141AsymmetricHWP.kext del kernel..."
kmutil unload -p "$KEXT_DEST" 2>/dev/null || kextunload "$KEXT_DEST" 2>/dev/null || true
rm -rf "$KEXT_DEST"

echo "[+] 3. Eliminando binario CLI y logs..."
rm -f "$CLI_DEST"
rm -f /var/log/a2141-hwp.log

rm -rf electron-app/out 2>/dev/null || true

echo "[+] Desinstalación completada. Al reiniciar, los registros MSR vuelven 100% a fábrica."
