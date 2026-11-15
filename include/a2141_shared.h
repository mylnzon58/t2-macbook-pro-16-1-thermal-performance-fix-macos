/*
 * include/a2141_shared.h
 * MacBook Pro 16,1 (A2141 - Intel Core i7-9750H) Asymmetric HWP & Thermal Driver
 * Author: Juan José Monzón (@mylnzon58)
 *
 * Shared hardware definitions, MSR addresses, and telemetry structures
 * between Ring-0 XNU Kernel Extension and Ring-3 Userland CLI/Daemon.
 */

#ifndef A2141_SHARED_H
#define A2141_SHARED_H

#ifdef KERNEL
#include <libkern/OSTypes.h>
#else
#include <stdint.h>
#endif

#define A2141_DRIVER_VERSION        "2.0.0"
#define A2141_PHYSICAL_CORES        6
#define A2141_LOGICAL_CPUS          12

/* --- Architectural & Model-Specific Registers (Intel Coffee Lake 14nm) --- */
#define MSR_IA32_PERF_STATUS            0x198ULL
#define MSR_IA32_THERM_STATUS           0x19CULL
#define MSR_IA32_TEMPERATURE_TARGET     0x1A2ULL
#define MSR_IA32_PACKAGE_THERM_STATUS   0x1B1ULL
#define MSR_RAPL_POWER_UNIT             0x606ULL
#define MSR_PKG_POWER_LIMIT             0x610ULL
#define MSR_IA32_PM_ENABLE              0x770ULL
#define MSR_IA32_HWP_CAPABILITIES       0x771ULL
#define MSR_IA32_HWP_REQUEST_PKG        0x772ULL
#define MSR_IA32_HWP_REQUEST            0x774ULL

/* --- XNU /dev/xcpm ioctl Commands --- */
#define XCPM_IOCTL_DVFS_PAUSE           0x20005808UL
#define XCPM_IOCTL_DVFS_RESUME          0x20005809UL

/* --- Energy Performance Preference (EPP) Values --- */
#define A2141_EPP_PERFORMANCE           0x40U
#define A2141_EPP_BALANCE_PERFORMANCE   0x80U
#define A2141_EPP_BALANCE_POWER         0xC0U
#define A2141_EPP_POWER                 0xE0U

/* --- Default Power & Thermal Thresholds --- */
#define A2141_DEFAULT_PL1_WATTS         38U     /* Safe default with Radeon Pro 5300M active */
#define A2141_DEFAULT_PL2_WATTS         52U     /* Short-term turbo burst ceiling */
#define A2141_DEFAULT_TCC_OFFSET        6U      /* 100C - 6C = 94C silicon thermal ceiling */
#define A2141_DEFAULT_TJMAX             100U
#define A2141_HYSTERESIS_BURST_MAX_C    68U     /* Restore Burst profile at or below 68C */
#define A2141_HYSTERESIS_HEAVY_MIN_C    75U     /* Engage Heavy profile at or above 75C */
#define A2141_CORE_GUARD_SOFT_C         85U     /* Per-core soft step-down (-200 MHz) */
#define A2141_CORE_GUARD_MED_C          88U     /* Per-core medium step-down (-400 MHz) */
#define A2141_CORE_GUARD_HARD_C         91U     /* Per-core hard step-down (-600 MHz) */
#define A2141_POLL_INTERVAL_MS          250U    /* 250ms Ring-0 loop */

/* --- Operating Profiles --- */
typedef enum {
    A2141_PROFILE_BURST = 0,
    A2141_PROFILE_HEAVY = 1,
    A2141_PROFILE_MANUAL = 2
} a2141_profile_t;

/*
 * Default Per-Core Max Ratio Tables (in 100 MHz units, e.g., 45 = 4.50 GHz)
 * Calibrated from live bare-die telemetry on MacBookPro16,1 (i7-9750H):
 * - Core 0: Thermally sensitive bare-die corner (protected at 3.20 GHz / 2.90 GHz)
 * - Core 1: Optimal heatsink contact (4.30 GHz / 3.50 GHz)
 * - Core 2: Secondary high-flux zone (3.80 GHz / 3.30 GHz)
 * - Core 3: Secondary high-flux zone (3.80 GHz / 3.30 GHz)
 * - Core 4: Coolest physical core on die (4.50 GHz / 3.60 GHz)
 * - Core 5: Strong heatsink contact (4.50 GHz / 3.60 GHz)
 */
#define A2141_BURST_RATIOS_INIT  { 32, 43, 38, 38, 45, 45 }
#define A2141_HEAVY_RATIOS_INIT  { 29, 35, 33, 33, 36, 36 }
#define A2141_CORE_EPP_INIT      { \
    A2141_EPP_BALANCE_POWER,       \
    A2141_EPP_BALANCE_PERFORMANCE, \
    A2141_EPP_BALANCE_PERFORMANCE, \
    A2141_EPP_BALANCE_POWER,       \
    A2141_EPP_BALANCE_PERFORMANCE, \
    A2141_EPP_BALANCE_PERFORMANCE  \
}

/*
 * Shared Telemetry Snapshot exposed via sysctl "machdep.a2141_telemetry".
 * Exact size (128 bytes) is preserved across v1.0.0 and v2.0.0 for binary compatibility.
 */
typedef struct a2141_telemetry {
    uint32_t version;
    uint32_t active_profile;                            /* 0 = Burst, 1 = Heavy, 2 = Manual */
    uint32_t enabled;                                   /* 1 = Active, 0 = Passthrough */
    uint32_t pl1_watts;
    uint32_t pl2_watts;
    uint32_t tcc_offset;
    uint32_t pkg_temp_c;                                /* Instantaneous MSR 0x1B1 Package Temp */
    uint32_t avg_perf_ratio;                            /* Average real-time ratio from MSR 0x198 */
    uint32_t core_temp_c[A2141_PHYSICAL_CORES];         /* Instantaneous MSR 0x19C Per-Core Temp */
    uint32_t core_applied_ratio[A2141_PHYSICAL_CORES];  /* Applied max ratio (e.g. 32 = 3.2 GHz) */
    uint32_t core_guard_events[A2141_PHYSICAL_CORES];   /* Count of per-core thermal guard trims */
    uint64_t enforce_cycles;                            /* Total 250ms enforcement ticks */
    uint64_t rapl_Override_count;                       /* Times T2 reset RAPL and kext re-clamped */
    uint64_t last_pkg_power_msr;                        /* Last read MSR 0x610 */
} a2141_telemetry_t;

#endif /* A2141_SHARED_H */
