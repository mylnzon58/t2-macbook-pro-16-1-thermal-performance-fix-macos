# MacBook Pro 16,1 (2019, A2141) Ring-0 Asymmetric HWP & Thermal Fix for macOS

[![macOS x86_64](https://img.shields.io/badge/macOS-Catalina_to_Sequoia_(x86__64)-111827?style=flat-square&logo=apple&logoColor=white)](#compatibilidad-exacta-de-hardware)
[![Hardware](https://img.shields.io/badge/Model-MacBookPro16%2C1_(A2141_EMC_3347)-1f2937?style=flat-square&logo=apple&logoColor=white)](#compatibilidad-exacta-de-hardware)
[![CPU](https://img.shields.io/badge/CPU-Intel_Core_i7--9750H_(6C%2F12T)-0071C5?style=flat-square&logo=intel&logoColor=white)](#arquitectura-ring-0-y-control-smc)
[![GPU](https://img.shields.io/badge/GPU-AMD_Radeon_Pro_5300M%2F5500M-ED1C24?style=flat-square&logo=amd&logoColor=white)](#arquitectura-ring-0-y-control-smc)
[![Author](https://img.shields.io/badge/Author-mylnzon58_(Juan_Monz%C3%B3n)-c9a45a?style=flat-square&logo=github&logoColor=black)](https://github.com/mylnzon58)
[![License: MIT](https://img.shields.io/badge/License-MIT-374151?style=flat-square)](LICENSE)

**Autor:** **[mylnzon58 (Juan José Monzón)](https://www.linkedin.com/in/juan-m-97b85681/)** · Full Stack Developer & Systems Engineering (Argentina)  
**Documentación Web Interactiva:** **[mylnzon58.github.io/t2-macbook-pro-16-1-thermal-performance-fix-macos](https://mylnzon58.github.io/t2-macbook-pro-16-1-thermal-performance-fix-macos/)**  
**Versión Hermana para Linux (Ubuntu 24.04 LTS):** **[mylnzon58/t2-macbook-pro-16-1-thermal-performance-fix-ubuntu](https://github.com/mylnzon58/t2-macbook-pro-16-1-thermal-performance-fix-ubuntu)**

**[Español](#resumen-directo-es)** | **[English](#direct-summary-en)**

---

<a id="resumen-directo-es"></a>

## Resumen Directo (Ley de Hick: Qué hace y qué resuelve)

```text
PROBLEMA EN MACOS DE FABRICA (XCPM + CHIP T2):
  XNU sobrescribe MSR 0x774 cada 1 ms forzando 4.50 GHz (Bit 42 Package_Control = 1) en los 6 núcleos
  → Core 0 (esquina crítica del cristal) salta a 97–99 °C y la piel inferior (Ts0S) sube a 47 °C
  → El algoritmo AppleSMCPDRC del chip T2 recorta el presupuesto combinado CPU+GPU (PZ0E) de 82 W a 36 W
  → Con monitor externo (Radeon Pro 5300M consumiendo 18.4 W fijos), quedan solo 17.6 W para el CPU
  → macOS activa kernel_task y desploma CPU_Speed_Limit al 22 %–58 % (800 MHz).

SOLUCION IMPLEMENTADA (A2141AsymmetricHWP.kext + a2141-ctl daemon v2.0.0):
  1. Pausa el bucle DVFS de 1 ms de XCPM (/dev/xcpm ioctl 0x20005808 -> _xcpm_dvfs_pause)
  2. Fija techos HWP asimétricos por núcleo físico en Ring-0 cada 250 ms (MSR 0x774 con Bit 42 = 0):
       Core 0 (protegido): 3.20 GHz Burst / 2.90 GHz Heavy (EPP 0xC0)
       Core 1 (intermedio): 4.30 GHz Burst / 3.50 GHz Heavy (EPP 0x80)
       Core 2 y Core 3:     3.80 GHz Burst / 3.30 GHz Heavy
       Core 4 y Core 5:     4.50 GHz Burst / 3.60 GHz Heavy (núcleos más fríos a máxima potencia)
  3. Ajusta RAPL (MSR 0x610) cada 500 ms según PZ0E - PG0R para que CPU+GPU jamás exceda PZ0E
  4. Fija TCC Offset en -6 °C (94 °C techo de silicio en MSR 0x1A2).

RESULTADO MEDIDO EN HARDWARE REAL:
  Reposo con monitor externo:  89.84 °C (35.45 W)  →  64.73 °C (8.60 W)   [-25.11 °C / -75.7 % consumo]
  Estrés AVX2 (12 hilos, 15s): 99.00 °C (58 % lim) →  79.30 °C (100 % lim) [-19.70 °C / 0 % throttling]
  Rendimiento sostenido FMA:   68.01 GigaFLOPs/s   →  80.03 GigaFLOPs/s   [+17.7 % potencia sostenida]
```

---

<a id="compatibilidad-exacta-de-hardware"></a>

## 1. Compatibilidad Exacta de Hardware y Requisitos

Este driver opera directamente sobre registros arquitectónicos `MSR` de Intel Coffee Lake y llaves privadas de `AppleSMC` del chip Apple T2. **Instálalo únicamente si tu equipo coincide de forma exacta con esta especificación:**

| Componente | Especificación Soportada (Exclusiva) | Cómo verificarlo en Terminal |
| :--- | :--- | :--- |
| **Modelo e Identificador** | **MacBook Pro (16-inch, 2019)** · `MacBookPro16,1` · Chasis **A2141** (EMC 3347) | `sysctl -n hw.model` → debe devolver `MacBookPro16,1` |
| **Procesador (CPU)** | **Intel Core i7-9750H** (Coffee Lake-H 14nm, **6 núcleos físicos / 12 hilos**, Base 2.60 GHz, Turbo 4.50 GHz, TDP 45 W) | `sysctl -n machdep.cpu.brand_string` |
| **Gráficos (dGPU + iGPU)** | **AMD Radeon Pro 5300M (4 GB GDDR6)** o **5500M** + **Intel UHD Graphics 630** | `system_profiler SPDisplaysDataType` |
| **Coprocesador SMC** | **Apple T2 Security Chip** (llaves `TCXC`, `TC0E`, `Ts0S`, `Th1H`, `PC0R`, `PG0R`, `PZ0E`) | Integrado en `MacBookPro16,1` |
| **Sistema Operativo** | **macOS x86_64** (Catalina 10.15, Big Sur 11, Monterey 12, Ventura 13, Sonoma 14, Sequoia 15) | `uname -m` → debe devolver `x86_64` |

> **Advertencia de incompatibilidad:**
> - **NO compatible** con equipos Apple Silicon (`M1`, `M2`, `M3`, `M4`).
> - **NO compatible** con MacBook Pro de 13" o 15" de generaciones previas.
> - Para la variante **Core i9-9880H / i9-9980HK (8 núcleos / 16 hilos)** del A2141, se requiere modificar `A2141_PHYSICAL_CORES` a `8` y extender los vectores `A2141_BURST_RATIOS_INIT` y `A2141_HEAVY_RATIOS_INIT` en `include/a2141_shared.h` antes de compilar.

### Recursos obligatorios antes de instalar

1. **Xcode Command Line Tools:** Compilador `clang`/`clang++`, `make` y el SDK del kernel (`Kernel.framework` e `IOKit.framework`).
   - Verificar: `xcode-select -p`
   - Instalar si falta: `xcode-select --install`
2. **System Integrity Protection (SIP) deshabilitado:** macOS exige firma empresarial EV de Apple (*Kext Signing Certificate*) para cargar extensiones `.kext` en Ring-0 con SIP activo. Para cargar este driver open-source compilado localmente debes ejecutar `csrutil disable` desde **macOS Recovery Mode**.
3. **Permisos de administrador (`sudo`):** Requeridos para escribir en `/Library/Extensions/`, `/Library/LaunchDaemons/` y `/dev/xcpm`.

---

## 2. Guía de Instalación Paso a Paso (Flujo Directo)

Sigue los 4 pasos en orden exacto:

```text
[Paso 1: Recovery Mode]  →  [Paso 2: Compilar e Instalar]  →  [Paso 3: Aprobar Kext]  →  [Paso 4: Verificar]
    csrutil disable             sudo ./install.sh            Privacidad y Seguridad       a2141-ctl status
```

### Paso 1 → Desactivar SIP en macOS Recovery Mode (Una sola vez)

1. Apaga o reinicia el MacBook Pro y **mantén presionadas las teclas `Cmd (⌘) + R`** hasta ver el logo de Apple o el globo de recuperación.
2. En la barra superior de macOS Recovery, abre **Utilidades → Terminal**.
3. Ejecuta el comando para desactivar la restricción de carga de kexts:

```bash
csrutil disable
```

4. Reinicia normalmente hacia macOS:

```bash
reboot
```

### Paso 2 → Clonar, Compilar e Instalar el Driver Ring-0 y Demonio

Abre **Terminal** en macOS y ejecuta:

```bash
git clone https://github.com/mylnzon58/t2-macbook-pro-16-1-thermal-performance-fix-macos.git
cd t2-macbook-pro-16-1-thermal-performance-fix-macos
chmod +x install.sh uninstall.sh
sudo ./install.sh
```

### Paso 3 → Autorizar la Extensión de Kernel en macOS (Solo en la primera instalación)

Cuando `install.sh` ejecuta `kmutil load`, macOS registra el `.kext` en la colección auxiliar (`AuxKC`):

1. Abre **Ajustes del Sistema (System Settings) → Privacidad y Seguridad (Privacy & Security)**.
2. Baja hasta la sección **Seguridad** y presiona **Permitir (Allow)** junto al aviso de bloqueo de la extensión de sistema (`A2141AsymmetricHWP`).
3. Confirma con tu contraseña y **reinicia el equipo** cuando macOS lo solicite para reconstruir el caché del kernel.
4. Al iniciar sesión tras el reinicio, el servicio `/Library/LaunchDaemons/com.juanmonzon.a2141-hwp.plist` arranca automáticamente en segundo plano. (Si deseas forzar la recarga inmediata tras aprobar: `sudo ./install.sh`).

### Paso 4 → Verificar Funcionamiento y Correr Benchmark

```bash
# 1. Ver estado del Kext en Ring-0, topes HWP por núcleo, sensores T2 y presupuesto PZ0E:
a2141-ctl status

# 2. Monitoreo en vivo cada 1 segundo (Ctrl+C para salir):
a2141-ctl monitor 1000

# 3. Prueba de estrés FMA/AVX2 en los 12 hilos durante 15 segundos:
a2141-ctl benchmark 15
```

---

## 3. Telemetría Real Verificada (Antes vs. Después)

Pruebas ejecutadas sobre hardware físico **MacBookPro16,1 (A2141 · Intel Core i7-9750H · AMD Radeon Pro 5300M activa con monitor externo conectado · macOS Darwin 23.6.0 x86_64)**:

### A. Estado en Reposo / Escritorio con Monitor Externo

| Métrica de Hardware / Sensor | macOS Stock (`XCPM` activo) | Con `A2141AsymmetricHWP v2.0.0` | Diferencia Medida |
| :--- | :---: | :---: | :---: |
| **PECI Max Die (`TCXC`)** | `89.84 °C` | **`64.73 °C`** | **`-25.11 °C`** |
| **Temperatura `Core 0` (`MSR 0x19C`)** | `92 °C` (`79.0 °C` SMC) | **`63 °C`** (`62.0 °C` SMC) | **`-29.0 °C`** |
| **Temperatura `Core 1..5` (`MSR 0x19C`)** | `73 °C – 84 °C` | **`62 °C – 63 °C`** | **`-11 °C a -21 °C`** |
| **Potencia del Paquete CPU (`PC0R`)** | `35.45 W` | **`8.60 W`** | **`-26.85 W` (`-75.7 %`)** |
| **Potencia Combinada `CPU+GPU` (`PHPC`)** | `53.83 W` (excede `PZ0E`) | **`26.87 W`** (bajo `PZ0E = 44.26 W`) | **`17.39 W` de margen libre** |
| **Recortes de Guardia Térmica (`>=93 °C`)** | `+15 a +20` eventos/minuto | **`0` eventos nuevos** | **Estabilidad total** |

### B. Estrés Computacional al 100 % (`a2141-ctl benchmark 15` — 12 Hilos AVX2/FMA)

| Métrica en Carga Sostenida (15 s) | macOS Stock / Sin Pausa XCPM | Con `A2141AsymmetricHWP v2.0.0` | Mejora Real |
| :--- | :---: | :---: | :---: |
| **Temperatura Pico de Silicio (`TCXC`)** | `99.0 °C` (en 2 segundos) | **`79.3 °C`** (promedio `77.1 °C`) | **`-19.7 °C` más frío** |
| **Rango Térmico de `Core 0`** | `97 °C – 99 °C` | **`72 °C – 81 °C`** | **`-18.0 °C` en el núcleo crítico** |
| **Rango Térmico de `Core 4` (Sano)** | `91 °C – 95 °C` | **`70 °C – 78 °C`** | **`-17.0 °C`** |
| **Límite de Velocidad (`CPU_Speed_Limit`)** | Cae a `58 %` y luego a `22 %` | **`100 %` constante (15/15 s)** | **Cero throttling de `kernel_task`** |
| **Throughput Sostenido (`GigaFLOPs/s`)** | `68.01 GigaFLOPs/s` | **`80.03 GigaFLOPs/s`** | **`+17.7 %` más rendimiento** |

---

<a id="arquitectura-ring-0-y-control-smc"></a>

## 4. Arquitectura Técnica: Por qué fallaban las herramientas convencionales en macOS

```text
┌────────────────────────────────────────────────────────────────────────────┐
│ Espacio de Usuario (Ring-3): /usr/local/bin/a2141-ctl daemon (Cada 500 ms) │
│  • Pausa el bucle DVFS de XCPM: ioctl(/dev/xcpm, 0x20005808)               │
│  • Lee AppleSMC: TCXC, TC0E, Th1H, Ts0S, PC0R, PG0R y presupuesto PZ0E     │
│  • Calcula PL1 disponible = clamp(PZ0E - PG0R - 2.5W, 22W..42W)            │
│  • Conmuta perfil por histéresis: BURST (<=68 °C) ↔ HEAVY (>=75 °C)        │
└──────────────────────────────────────┬─────────────────────────────────────┘
                                       │ sysctl (machdep.a2141_*)
                                       ▼
┌────────────────────────────────────────────────────────────────────────────┐
│ Kernel XNU (Ring-0): A2141AsymmetricHWP.kext (Cada 250 ms en los 12 hilos) │
│  • mp_rendezvous_no_intrs() ejecuta cpu_enforce_rendezvous en las 12 CPUs  │
│  • Identifica el núcleo físico real (0..5) vía CPUID Leaf 0x1 Initial APIC │
│  • Lee sensor térmico digital por núcleo (MSR 0x19C) y aplica guardia 3N   │
│  • Escribe MSR_IA32_HWP_REQUEST (0x774) con Bit 42 (Package_Control) = 0   │
│  • Escribe MSR_PKG_POWER_LIMIT (0x610) y MSR_TEMPERATURE_TARGET (0x1A2)    │
└────────────────────────────────────────────────────────────────────────────┘
```

1. **El conflicto oculto en `XCPM` (`0xffffff8000406290`):** El motor `XCPM` del kernel de macOS ejecuta `_xcpm_urgency` y `_xcpm_timer` en cada tick del planificador (~1 ms), escribiendo `0x40000000000` (`Bit 42 = Package_Control = 1`) y `Desired_Performance = 45` (`4.50 GHz`) en `MSR 0x774`. Cualquier herramienta que escriba `MSR 0x774` periódicamente sin pausar `XCPM` pierde el control 1 ms después (el 99.6 % del tiempo). Al invocar `_xcpm_dvfs_pause()` mediante `/dev/xcpm` (`0x20005808`) y desde el `.kext`, `_xcpm_dvfs_paused` queda en `1` y `XCPM` deja de sobrescribir `MSR 0x774`.
2. **El presupuesto oculto `PZ0E` del chip T2 (`AppleSMCPDRC`):** Cuando el sensor de piel inferior (`Ts0S`) supera los `45 °C`, el chip T2 reduce dinámicamente la llave SMC `PZ0E` (presupuesto máximo combinado `CPU + dGPU`) desde `82.0 W` hasta `36.1 W`. Con un monitor externo conectado, la GPU Radeon Pro 5300M consume `18.4 W` constantes (`PG0R`). Si el CPU supera `PZ0E - PG0R`, el T2 fuerza a `kernel_task` a estrangular la frecuencia al `22 %`. Nuestro demonio sincroniza `RAPL PL1` en tiempo real con `PZ0E - PG0R - 2.5 W`, manteniendo el consumo total bajo el umbral de castigo del T2.

---

## 5. Comandos Manuales y Desinstalación

```bash
# Cambiar perfil manualmente (auto | burst | heavy):
sudo a2141-ctl profile auto

# Ajustar límites RAPL PL1 y PL2 manualmente (en Watts):
sudo a2141-ctl power 35 48

# Ajustar TCC Offset bajo 100 °C (ej. 6 = techo de 94 °C):
sudo a2141-ctl tcc 6

# Desinstalar por completo y restaurar macOS a su estado original de fábrica:
sudo ./uninstall.sh
```

---

<a id="direct-summary-en"></a>

## English — Executive Summary & Installation Guide

### Hardware Scope (Strictly For This Model)

- **Target Machine:** Apple MacBook Pro 16-inch (Late 2019) · Identifier `MacBookPro16,1` · Chassis **A2141** (EMC 3347).
- **Processor:** Intel Core i7-9750H (6 physical cores / 12 threads, Coffee Lake 14nm).
- **Graphics & SMC:** AMD Radeon Pro 5300M / 5500M + Intel UHD Graphics 630 + Apple T2 Security Chip.
- **Operating System:** macOS Catalina (10.15) through macOS Sequoia (15) (`x86_64`).
- **Prerequisites:** Xcode Command Line Tools (`xcode-select --install`), SIP disabled in Recovery Mode (`csrutil disable`), and root privileges (`sudo`).

### Why Stock macOS Throttles the A2141

1. **XNU `XCPM` 1ms Override (`0xffffff8000406290`):** By default, XNU writes `MSR_IA32_HWP_REQUEST (0x774)` every ~1 ms with `Bit 42 (Package_Control) = 1` and `Desired_Performance = 45` (`4.50 GHz`), forcing high Vcore across all 6 cores—even when a single background thread is active.
2. **Bare-Die Corner Hotspot on `Core 0`:** Due to uneven heatsink mounting pressure on the bare rectangular die, `Core 0` reaches `95–99 °C` while `Core 4` sits `18 °C` cooler.
3. **T2 `PZ0E` Combined Power Budget Collapse:** When bottom skin temperature (`Ts0S`) rises, the T2's `AppleSMCPDRC` controller lowers `PZ0E` (maximum combined `CPU + GPU` power budget) from `82 W` down to `~36 W`. Because the AMD Radeon Pro 5300M draws a constant `18.4 W` (`PG0R`) whenever an external monitor is connected, any CPU draw above `18 W` breaches `PZ0E` and triggers `CPU_Speed_Limit = 22%` (`kernel_task` throttling).

### Guided 4-Step Installation

```text
[Step 1: Recovery Mode]  →  [Step 2: Build & Install]  →  [Step 3: Approve Kext]  →  [Step 4: Verify]
    csrutil disable             sudo ./install.sh          Privacy & Security        a2141-ctl status
```

1. **Disable SIP in Recovery Mode:** Hold `Cmd + R` on boot → open **Utilities → Terminal** → run `csrutil disable` → run `reboot`.
2. **Clone & Install:**
   ```bash
   git clone https://github.com/mylnzon58/t2-macbook-pro-16-1-thermal-performance-fix-macos.git
   cd t2-macbook-pro-16-1-thermal-performance-fix-macos
   chmod +x install.sh uninstall.sh
   sudo ./install.sh
   ```
3. **Approve Kernel Extension:** Open **System Settings → Privacy & Security**, click **Allow** for `A2141AsymmetricHWP`, and reboot when prompted so macOS rebuilds `AuxKC`.
4. **Verify & Benchmark:**
   ```bash
   a2141-ctl status
   a2141-ctl benchmark 15
   ```

---

## Soporte Open-Source y Patrocinio / Sponsorship

Este driver Ring-0 y demonio de control térmico fueron investigados, desensamblados y desarrollados de forma independiente por **Juan José Monzón (`@mylnzon58`)**. Si esta solución recuperó el rendimiento sostenido de tu MacBook Pro 16,1 y evitó el estrangulamiento por temperatura en tu flujo de trabajo diario:

- **GitHub Sponsors:** [github.com/sponsors/mylnzon58](https://github.com/sponsors/mylnzon58)
- **Planes Mensuales en Patreon:** [patreon.com/JuanjoClassic](https://www.patreon.com/JuanjoClassic) (Soporte continuo a herramientas open-source e ingeniería de sistemas)
- **Aporte directo (Tecito):** [tecito.app/juanjoclassic](https://tecito.app/juanjoclassic)
- **Contacto Profesional:** [LinkedIn — Juan José Monzón](https://www.linkedin.com/in/juan-m-97b85681/) · [GitHub `@mylnzon58`](https://github.com/mylnzon58) · `mylnzon58@gmail.com`
