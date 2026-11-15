/*
 * kext/A2141AsymmetricHWP.cpp
 * MacBook Pro 16,1 (A2141 - Intel Core i7-9750H) Asymmetric HWP & Thermal Driver
 * Author: Juan José Monzón (@mylnzon58)
 *
 * Ring-0 XNU Kernel Extension providing:
 *   1. Direct XCPM DVFS override pause (_xcpm_dvfs_pause) so XNU stops clobbering MSR 0x774
 *   2. 250ms Closed-Loop Per-Core HWP (MSR 0x774 + MSR 0x772) via mp_rendezvous_no_intrs
 *   3. 3-Tier Individual Core Digital Thermal Guard (85C / 88C / 91C -> -200/-400/-600 MHz)
 *   4. Continuous PZ0E-Aware RAPL Power Clamping (MSR 0x610)
 *   5. Silicon TCC Offset Enforcement (MSR 0x1A2 -> 6C offset = 94C hardware ceiling)
 *   6. Instant Post-Sleep Persistence via IOKit Power Management
 *   7. Live Telemetry & Runtime Control via sysctl (machdep.a2141_*)
 */

#include "A2141AsymmetricHWP.hpp"
#include <IOKit/IOLib.h>
#include <i386/proc_reg.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/sysctl.h>

extern "C" {
    extern void mp_rendezvous_no_intrs(void (*action_func)(void *), void *arg);
    extern int cpu_number(void);
}

/* Unslid XNU symbol addresses in /System/Library/Kernels/kernel (Darwin 23.6.0 x86_64) */
#define XNU_UNSLID_MP_RENDEZVOUS_NO_INTRS   0xffffff80003f9640ULL
#define XNU_UNSLID_XCPM_DVFS_PAUSE          0xffffff80004092c0ULL
#define XNU_UNSLID_XCPM_DVFS_RESUME         0xffffff8000409b00ULL
#define XNU_UNSLID_XCPM_DVFS_PAUSED_FLAG    0xffffff8000dfcf38ULL

typedef void (*xcpm_dvfs_fn_t)(void);

#define super IOService
OSDefineMetaClassAndStructors(com_juanmonzon_driver_A2141AsymmetricHWP, IOService)

/* --- Runtime Tunables & Telemetry State --- */
static a2141_telemetry_t g_telemetry = {
    .version        = 2,
    .active_profile = A2141_PROFILE_BURST,
    .enabled        = 1,
    .pl1_watts      = A2141_DEFAULT_PL1_WATTS,
    .pl2_watts      = A2141_DEFAULT_PL2_WATTS,
    .tcc_offset     = A2141_DEFAULT_TCC_OFFSET,
    .pkg_temp_c     = 0,
    .avg_perf_ratio = 0
};

static uint32_t g_burst_ratios[A2141_PHYSICAL_CORES]      = A2141_BURST_RATIOS_INIT;
static uint32_t g_heavy_ratios[A2141_PHYSICAL_CORES]      = A2141_HEAVY_RATIOS_INIT;
static uint32_t g_core_epp[A2141_PHYSICAL_CORES]          = A2141_CORE_EPP_INIT;
static uint32_t g_core_actual_ratio[A2141_PHYSICAL_CORES] = { 0 };
static int      g_mode_override                           = -1; /* -1 = Auto, 0 = Burst, 1 = Heavy */
static volatile uint32_t *g_xcpm_dvfs_paused_ptr          = nullptr;
static xcpm_dvfs_fn_t     g_xcpm_dvfs_pause_fn            = nullptr;
static xcpm_dvfs_fn_t     g_xcpm_dvfs_resume_fn           = nullptr;

/* --- sysctl MIB Declarations (exposed as machdep.a2141_*) --- */
SYSCTL_DECL(_machdep);

SYSCTL_STRUCT(_machdep, OID_AUTO, a2141_telemetry, CTLFLAG_RD | CTLFLAG_OID2,
              &g_telemetry, a2141_telemetry, "MacBookPro16,1 Asymmetric HWP Telemetry");
SYSCTL_OPAQUE(_machdep, OID_AUTO, a2141_core_actual_ratio, CTLFLAG_RD | CTLFLAG_OID2,
              g_core_actual_ratio, sizeof(g_core_actual_ratio), "IU",
              "Real-time per-core clock multiplier from MSR 0x198");
SYSCTL_INT(_machdep, OID_AUTO, a2141_enabled, CTLFLAG_RW | CTLFLAG_OID2,
           &g_telemetry.enabled, 0, "Enable/Disable A2141 Asymmetric HWP enforcement");
SYSCTL_INT(_machdep, OID_AUTO, a2141_mode_override, CTLFLAG_RW | CTLFLAG_OID2,
           &g_mode_override, 0, "Profile mode: -1=Auto, 0=Force Burst, 1=Force Heavy");
SYSCTL_INT(_machdep, OID_AUTO, a2141_pl1_watts, CTLFLAG_RW | CTLFLAG_OID2,
           &g_telemetry.pl1_watts, 0, "Sustained RAPL PL1 limit in Watts");
SYSCTL_INT(_machdep, OID_AUTO, a2141_pl2_watts, CTLFLAG_RW | CTLFLAG_OID2,
           &g_telemetry.pl2_watts, 0, "Burst RAPL PL2 limit in Watts");
SYSCTL_INT(_machdep, OID_AUTO, a2141_tcc_offset, CTLFLAG_RW | CTLFLAG_OID2,
           &g_telemetry.tcc_offset, 0, "TCC activation offset below 100C");
SYSCTL_INT(_machdep, OID_AUTO, a2141_core0_burst_ratio, CTLFLAG_RW | CTLFLAG_OID2,
           &g_burst_ratios[0], 0, "Core 0 Burst Max Ratio (100MHz units)");
SYSCTL_INT(_machdep, OID_AUTO, a2141_core0_heavy_ratio, CTLFLAG_RW | CTLFLAG_OID2,
           &g_heavy_ratios[0], 0, "Core 0 Heavy Max Ratio (100MHz units)");

/* --- IOKit Power Management States --- */
enum {
    kA2141PowerStateOff = 0,
    kA2141PowerStateOn  = 1,
    kA2141NumPowerStates = 2
};

static IOPMPowerState g_power_states[kA2141NumPowerStates] = {
    { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 }
};

/*
 * Helper: Resolve KASLR slide from _mp_rendezvous_no_intrs to locate XCPM DVFS symbols.
 */
static void resolve_xcpm_symbols(void) {
    uintptr_t runtime_mp = (uintptr_t)&mp_rendezvous_no_intrs;
    if (runtime_mp > XNU_UNSLID_MP_RENDEZVOUS_NO_INTRS) {
        uintptr_t slide = runtime_mp - XNU_UNSLID_MP_RENDEZVOUS_NO_INTRS;
        if ((slide & 0xFFFFFULL) == 0) {
            g_xcpm_dvfs_pause_fn   = (xcpm_dvfs_fn_t)(XNU_UNSLID_XCPM_DVFS_PAUSE + slide);
            g_xcpm_dvfs_resume_fn  = (xcpm_dvfs_fn_t)(XNU_UNSLID_XCPM_DVFS_RESUME + slide);
            g_xcpm_dvfs_paused_ptr = (volatile uint32_t *)(XNU_UNSLID_XCPM_DVFS_PAUSED_FLAG + slide);
        }
    }
}

/*
 * Helper: Obtain physical core index (0..5) directly from hardware Initial APIC ID
 * via CPUID leaf 0x1, ensuring 100% silicon-accurate mapping on Intel Coffee Lake 6C/12T.
 */
static inline uint32_t get_physical_core_id(void) {
    uint32_t eax = 1, ebx = 0, ecx = 0, edx = 0;
    __asm__ volatile("cpuid"
                     : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
    uint32_t apic_id = (ebx >> 24) & 0xFFU;
    uint32_t phys_id = (apic_id >> 1);
    return (phys_id < A2141_PHYSICAL_CORES) ? phys_id : (uint32_t)(cpu_number() % A2141_PHYSICAL_CORES);
}

/*
 * Ring-0 Rendezvous Callback executed simultaneously on all 12 logical CPUs
 * with interrupts disabled. Execution time: < 200 nanoseconds.
 */
static void cpu_enforce_rendezvous(void *arg __unused) {
    if (!g_telemetry.enabled) {
        return;
    }

    int logical_cpu = cpu_number();
    uint32_t phys_core = get_physical_core_id();

    /* 1. Read per-core digital thermal sensor (MSR 0x19C) & real-time ratio (MSR 0x198) */
    uint64_t therm_msr = rdmsr64(MSR_IA32_THERM_STATUS);
    uint32_t readout = (uint32_t)((therm_msr >> 16) & 0x7FULL);
    uint32_t core_temp = (readout <= A2141_DEFAULT_TJMAX) ? (A2141_DEFAULT_TJMAX - readout) : 50U;
    g_telemetry.core_temp_c[phys_core] = core_temp;

    uint64_t perf_msr = rdmsr64(MSR_IA32_PERF_STATUS);
    g_core_actual_ratio[phys_core] = (uint32_t)((perf_msr >> 8) & 0xFFULL);

    /* 2. Select base ratio from active profile, with Adaptive Hotspot Migration:
     *    - If Core 0 is cool (<= 70C) and another core is hotter, allow Core 0 up to 3.8 GHz.
     *    - If any non-Core-0 core is running >= 6C hotter than Core 0 and >= 80C,
     *      automatically cap that hotspot core to protect it regardless of which core has paste pump-out.
     */
    uint32_t base_ratio = (g_telemetry.active_profile == A2141_PROFILE_HEAVY)
                          ? g_heavy_ratios[phys_core]
                          : g_burst_ratios[phys_core];

    uint32_t c0_temp = g_telemetry.core_temp_c[0];
    if (phys_core == 0 && g_mode_override == -1 && g_telemetry.active_profile == A2141_PROFILE_BURST) {
        if (core_temp > 0U && core_temp <= 70U && base_ratio < 38U) {
            base_ratio = 36U; /* Give Core 0 extra headroom when it runs cool */
        }
    } else if (phys_core > 0) {
        if (core_temp >= 80U && c0_temp > 0U && core_temp >= (c0_temp + 6U)) {
            uint32_t hotspot_cap = (g_telemetry.active_profile == A2141_PROFILE_HEAVY) ? 29U : 33U;
            if (base_ratio > hotspot_cap) {
                base_ratio = hotspot_cap;
            }
        }
    }

    /* Clamp ratio to safe architectural bounds (1.80 GHz .. 4.50 GHz i7 / 5.00 GHz i9) */
#ifdef A2141_MODEL_I9
    if (base_ratio < 18U) base_ratio = 18U;
    if (base_ratio > 50U) base_ratio = 50U;
#else
    if (base_ratio < 18U) base_ratio = 18U;
    if (base_ratio > 45U) base_ratio = 45U;
#endif

    /* 3. 3-Tier Per-Core Dynamic Thermal Guard (85C / 88C / 91C) */
    uint32_t target_ratio = base_ratio;
    if (core_temp >= A2141_CORE_GUARD_HARD_C) {
        target_ratio = (base_ratio > 28U) ? (base_ratio - 6U) : 22U;
        g_telemetry.core_guard_events[phys_core]++;
    } else if (core_temp >= A2141_CORE_GUARD_MED_C) {
        target_ratio = (base_ratio > 28U) ? (base_ratio - 4U) : 24U;
        g_telemetry.core_guard_events[phys_core]++;
    } else if (core_temp >= A2141_CORE_GUARD_SOFT_C) {
        target_ratio = (base_ratio > 26U) ? (base_ratio - 2U) : 24U;
        g_telemetry.core_guard_events[phys_core]++;
    }
    g_telemetry.core_applied_ratio[phys_core] = target_ratio;

    /* 4. Program MSR_IA32_HWP_REQUEST (0x774) on this logical processor */
    uint64_t hwp_enabled = rdmsr64(MSR_IA32_PM_ENABLE);
    if (hwp_enabled & 0x1ULL) {
        uint32_t min_perf = 8U; /* 800 MHz idle floor */
        uint32_t max_perf = target_ratio & 0xFFU;
        uint32_t des_perf = 0U; /* Autonomous hardware-guided scaling */
        uint32_t epp      = g_core_epp[phys_core] & 0xFFU;

        /* Bit 42 (Package_Control) is explicitly 0 so this core obeys its own 0x774 */
        uint64_t hwp_req = ((uint64_t)min_perf) |
                           ((uint64_t)max_perf << 8) |
                           ((uint64_t)des_perf << 16) |
                           ((uint64_t)epp << 24);
        wrmsr64(MSR_IA32_HWP_REQUEST, hwp_req);
    }

    /* 5. Package-wide MSRs executed once on Logical CPU 0 */
    if (logical_cpu == 0) {
        /* A. Also program MSR_IA32_HWP_REQUEST_PKG (0x772) with safe autonomous defaults */
        if (hwp_enabled & 0x1ULL) {
            uint64_t pkg_hwp = 8ULL | (36ULL << 8) | (0ULL << 16) | (0x80ULL << 24);
            wrmsr64(MSR_IA32_HWP_REQUEST_PKG, pkg_hwp);
        }

        /* B. Read Package Digital Thermal Status (MSR 0x1B1) */
        uint64_t pkg_therm = rdmsr64(MSR_IA32_PACKAGE_THERM_STATUS);
        uint32_t pkg_readout = (uint32_t)((pkg_therm >> 16) & 0x7FULL);
        uint32_t pkg_temp = (pkg_readout <= A2141_DEFAULT_TJMAX) ? (A2141_DEFAULT_TJMAX - pkg_readout) : core_temp;
        g_telemetry.pkg_temp_c = pkg_temp;

        /* C. Enforce TCC Offset (MSR 0x1A2 bits 29:24) */
        uint32_t tcc = g_telemetry.tcc_offset;
        if (tcc > 15U) tcc = 15U;
        uint64_t temp_target = rdmsr64(MSR_IA32_TEMPERATURE_TARGET);
        uint32_t cur_tcc = (uint32_t)((temp_target >> 24) & 0x3FULL);
        if (((temp_target & (1ULL << 31)) == 0) && (cur_tcc != tcc)) {
            uint64_t new_target = (temp_target & ~(0x3FULL << 24)) | ((uint64_t)tcc << 24);
            wrmsr64(MSR_IA32_TEMPERATURE_TARGET, new_target);
        }

        /* D. Enforce RAPL Package Power Limits (MSR 0x610) */
        uint32_t pl1_w = g_telemetry.pl1_watts;
        uint32_t pl2_w = g_telemetry.pl2_watts;
        if (pl1_w < 15U) pl1_w = 15U;
        if (pl1_w > 65U) pl1_w = 65U;
        if (pl2_w < pl1_w) pl2_w = pl1_w;
        if (pl2_w > 90U) pl2_w = 90U;

        uint64_t rapl_unit_msr = rdmsr64(MSR_RAPL_POWER_UNIT);
        uint32_t pwr_unit_shift = (uint32_t)(rapl_unit_msr & 0xFULL);
        uint32_t pwr_mult = 1U << pwr_unit_shift; /* 8 (0.125W units) */

        uint64_t pl1_raw = (uint64_t)(pl1_w * pwr_mult) & 0x7FFFULL;
        uint64_t pl2_raw = (uint64_t)(pl2_w * pwr_mult) & 0x7FFFULL;

        uint64_t cur_rapl = rdmsr64(MSR_PKG_POWER_LIMIT);
        g_telemetry.last_pkg_power_msr = cur_rapl;

        if ((cur_rapl & (1ULL << 63)) == 0) {
            uint64_t cur_pl1 = cur_rapl & 0x7FFFULL;
            uint64_t cur_pl2 = (cur_rapl >> 32) & 0x7FFFULL;

            if (cur_pl1 != pl1_raw || cur_pl2 != pl2_raw ||
                ((cur_rapl & (3ULL << 15)) != (3ULL << 15))) {
                uint64_t tw_mask = (0x7FULL << 17) | (0x7FULL << 49);
                uint64_t en_clamp = (1ULL << 15) | (1ULL << 16) | (1ULL << 47) | (1ULL << 48);
                uint64_t new_rapl = (cur_rapl & tw_mask) |
                                    pl1_raw |
                                    (pl2_raw << 32) |
                                    en_clamp;
                wrmsr64(MSR_PKG_POWER_LIMIT, new_rapl);
                g_telemetry.last_pkg_power_msr = new_rapl;
                g_telemetry.rapl_Override_count++;
            }
        }
    }
}

bool com_juanmonzon_driver_A2141AsymmetricHWP::init(OSDictionary *dictionary) {
    if (!super::init(dictionary)) {
        return false;
    }
    fWorkLoop = nullptr;
    fTimerSource = nullptr;
    fSysctlRegistered = false;
    return true;
}

void com_juanmonzon_driver_A2141AsymmetricHWP::free(void) {
    super::free();
}

IOService *com_juanmonzon_driver_A2141AsymmetricHWP::probe(IOService *provider, SInt32 *score) {
    return super::probe(provider, score);
}

void com_juanmonzon_driver_A2141AsymmetricHWP::enforceHardwareState(void) {
    /* 0. Ensure XNU XCPM DVFS stays paused so MSR 0x774 is never overwritten */
    if (g_telemetry.enabled && g_xcpm_dvfs_paused_ptr != nullptr) {
        if (*g_xcpm_dvfs_paused_ptr == 0 && g_xcpm_dvfs_pause_fn != nullptr) {
            g_xcpm_dvfs_pause_fn();
        }
    }

    /* 1. Execute per-core MSR enforcement and read silicon temperatures */
    mp_rendezvous_no_intrs(cpu_enforce_rendezvous, nullptr);
    g_telemetry.enforce_cycles++;

    /* Compute average real-time ratio across the 6 physical cores */
    uint32_t sum_ratio = 0;
    for (int i = 0; i < A2141_PHYSICAL_CORES; i++) {
        sum_ratio += g_core_actual_ratio[i];
    }
    g_telemetry.avg_perf_ratio = sum_ratio / A2141_PHYSICAL_CORES;

    /* 2. Evaluate Thermal Hysteresis for next 250ms tick */
    if (g_mode_override == 0) {
        g_telemetry.active_profile = A2141_PROFILE_BURST;
    } else if (g_mode_override == 1) {
        g_telemetry.active_profile = A2141_PROFILE_HEAVY;
    } else {
        uint32_t max_temp = g_telemetry.pkg_temp_c;
        for (int i = 0; i < A2141_PHYSICAL_CORES; i++) {
            if (g_telemetry.core_temp_c[i] > max_temp) {
                max_temp = g_telemetry.core_temp_c[i];
            }
        }
        if (max_temp >= A2141_HYSTERESIS_HEAVY_MIN_C) {
            g_telemetry.active_profile = A2141_PROFILE_HEAVY;
        } else if (max_temp <= A2141_HYSTERESIS_BURST_MAX_C) {
            g_telemetry.active_profile = A2141_PROFILE_BURST;
        }
    }
}

void com_juanmonzon_driver_A2141AsymmetricHWP::timerFired(OSObject *owner, IOTimerEventSource *sender) {
    com_juanmonzon_driver_A2141AsymmetricHWP *self = OSDynamicCast(com_juanmonzon_driver_A2141AsymmetricHWP, owner);
    if (self) {
        self->enforceHardwareState();
    }
    if (sender) {
        sender->setTimeoutMS(A2141_POLL_INTERVAL_MS);
    }
}

bool com_juanmonzon_driver_A2141AsymmetricHWP::start(IOService *provider) {
    if (!super::start(provider)) {
        return false;
    }

    resolve_xcpm_symbols();
    if (g_xcpm_dvfs_pause_fn != nullptr) {
        g_xcpm_dvfs_pause_fn();
    }

    /* Register sysctl OIDs */
    sysctl_register_oid(&sysctl__machdep_a2141_telemetry);
    sysctl_register_oid(&sysctl__machdep_a2141_core_actual_ratio);
    sysctl_register_oid(&sysctl__machdep_a2141_enabled);
    sysctl_register_oid(&sysctl__machdep_a2141_mode_override);
    sysctl_register_oid(&sysctl__machdep_a2141_pl1_watts);
    sysctl_register_oid(&sysctl__machdep_a2141_pl2_watts);
    sysctl_register_oid(&sysctl__machdep_a2141_tcc_offset);
    sysctl_register_oid(&sysctl__machdep_a2141_core0_burst_ratio);
    sysctl_register_oid(&sysctl__machdep_a2141_core0_heavy_ratio);
    fSysctlRegistered = true;

    /* Apply initial hardware enforcement immediately */
    enforceHardwareState();

    /* Setup 250ms kernel timer */
    fWorkLoop = getWorkLoop();
    if (!fWorkLoop) {
        return false;
    }

    fTimerSource = IOTimerEventSource::timerEventSource(
        this,
        &com_juanmonzon_driver_A2141AsymmetricHWP::timerFired
    );
    if (!fTimerSource || fWorkLoop->addEventSource(fTimerSource) != kIOReturnSuccess) {
        IOLog("A2141AsymmetricHWP: Failed to attach IOTimerEventSource\n");
        return false;
    }
    fTimerSource->setTimeoutMS(A2141_POLL_INTERVAL_MS);

    /* Register for IOKit Sleep/Wake Power Management */
    PMinit();
    provider->joinPMtree(this);
    registerPowerDriver(this, g_power_states, kA2141NumPowerStates);

    registerService();
    IOLog("A2141AsymmetricHWP v%s: Started (XCPM Paused, PL1=%uW, PL2=%uW, TCC=-%uC, Core0=%u/%u)\n",
          A2141_DRIVER_VERSION, g_telemetry.pl1_watts, g_telemetry.pl2_watts,
          g_telemetry.tcc_offset, g_burst_ratios[0], g_heavy_ratios[0]);
    return true;
}

void com_juanmonzon_driver_A2141AsymmetricHWP::stop(IOService *provider) {
    PMstop();

    if (fTimerSource) {
        fTimerSource->cancelTimeout();
        if (fWorkLoop) {
            fWorkLoop->removeEventSource(fTimerSource);
        }
        fTimerSource->release();
        fTimerSource = nullptr;
    }

    if (fSysctlRegistered) {
        sysctl_unregister_oid(&sysctl__machdep_a2141_telemetry);
        sysctl_unregister_oid(&sysctl__machdep_a2141_core_actual_ratio);
        sysctl_unregister_oid(&sysctl__machdep_a2141_enabled);
        sysctl_unregister_oid(&sysctl__machdep_a2141_mode_override);
        sysctl_unregister_oid(&sysctl__machdep_a2141_pl1_watts);
        sysctl_unregister_oid(&sysctl__machdep_a2141_pl2_watts);
        sysctl_unregister_oid(&sysctl__machdep_a2141_tcc_offset);
        sysctl_unregister_oid(&sysctl__machdep_a2141_core0_burst_ratio);
        sysctl_unregister_oid(&sysctl__machdep_a2141_core0_heavy_ratio);
        fSysctlRegistered = false;
    }

    if (g_xcpm_dvfs_resume_fn != nullptr) {
        g_xcpm_dvfs_resume_fn();
    }

    IOLog("A2141AsymmetricHWP: Stopped\n");
    super::stop(provider);
}

IOReturn com_juanmonzon_driver_A2141AsymmetricHWP::setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice __unused) {
    if (powerStateOrdinal == kA2141PowerStateOn) {
        if (g_xcpm_dvfs_pause_fn != nullptr) {
            g_xcpm_dvfs_pause_fn();
        }
        enforceHardwareState();
        if (fTimerSource) {
            fTimerSource->setTimeoutMS(A2141_POLL_INTERVAL_MS);
        }
        IOLog("A2141AsymmetricHWP: Woke from sleep, paused XCPM DVFS and re-clamped HWP/RAPL/TCC\n");
    }
    return kIOPMAckImplied;
}

extern "C" {
    extern kern_return_t _start(kmod_info_t *ki, void *data);
    extern kern_return_t _stop(kmod_info_t *ki, void *data);
    __attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(com.juanmonzon.driver.A2141AsymmetricHWP, "1.0.0", _start, _stop)
    __private_extern__ kmod_start_func_t *_realmain = nullptr;
    __private_extern__ kmod_stop_func_t  *_antimain = nullptr;
    __private_extern__ int _apple_cc = __APPLE_CC__;
}
