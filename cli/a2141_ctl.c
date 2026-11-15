/*
 * cli/a2141_ctl.c
 * MacBook Pro 16,1 (A2141 - Intel Core i7-9750H) Telemetry, Proactive Fan & XCPM Daemon
 * Author: Juan José Monzón (@mylnzon58)
 */

#include "smc_client.h"
#include "../include/a2141_shared.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <math.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>

static volatile sig_atomic_t g_keep_running = 1;

static void handle_sigint(int sig) {
    (void)sig;
    g_keep_running = 0;
}

static int read_kext_telemetry(a2141_telemetry_t *out) {
    size_t len = sizeof(a2141_telemetry_t);
    memset(out, 0, len);
    if (sysctlbyname("machdep.a2141_telemetry", out, &len, NULL, 0) != 0) {
        return -1;
    }
    return 0;
}

static int read_core_actual_ratios(uint32_t ratios_out[A2141_PHYSICAL_CORES]) {
    size_t len = sizeof(uint32_t) * A2141_PHYSICAL_CORES;
    memset(ratios_out, 0, len);
    if (sysctlbyname("machdep.a2141_core_actual_ratio", ratios_out, &len, NULL, 0) != 0) {
        return -1;
    }
    return 0;
}

static int set_sysctl_int_silent(const char *name, int val) {
    return sysctlbyname(name, NULL, NULL, &val, sizeof(val));
}

static int set_sysctl_int(const char *name, int val) {
    if (sysctlbyname(name, NULL, NULL, &val, sizeof(val)) != 0) {
        perror(name);
        return -1;
    }
    return 0;
}

/*
 * Pauses or resumes XNU's internal 1ms XCPM DVFS override loop via /dev/xcpm
 * (_bsd_xcpm_ioctl -> _xcpm_cpu_control -> _xcpm_dvfs_pause / _xcpm_dvfs_resume).
 * When paused, XNU stops overwriting MSR 0x774 with 4.5 GHz (Package_Control=1),
 * allowing A2141AsymmetricHWP.kext to hold per-core asymmetric HWP limits 100% of the time.
 */
static int xcpm_set_dvfs_pause(int pause_dvfs) {
    int fd = open("/dev/xcpm", O_RDWR);
    if (fd < 0) {
        return -1;
    }
    unsigned long cmd = pause_dvfs ? XCPM_IOCTL_DVFS_PAUSE : XCPM_IOCTL_DVFS_RESUME;
    int rc = ioctl(fd, cmd, NULL);
    close(fd);
    return rc;
}

/*
 * Proactive closed-loop controller step:
 *   1. Computes proactive minimum fan RPM (F0Mn / F1Mn) from TCXC, Ts0S (Bottom Skin),
 *      Th1H (Fin Stack), and PZ0E (Chassis Power Budget) to eliminate T2's 15s lag.
 *   2. Computes dynamic PZ0E-aware RAPL PL1/PL2 limits so PC0R + PG0R never exceeds PZ0E,
 *      preventing AppleSMCPDRC from collapsing CPU_Speed_Limit below 100%.
 *   3. Drives tighter BURST (<=68C) / HEAVY (>=75C) profile hysteresis.
 */
static void apply_proactive_control_step(const a2141_smc_snapshot_t *smc,
                                         int *io_profile,
                                         int *io_pl1,
                                         float *io_f0_min) {
    /* 1. Proactive Dual-Turbine Curve */
    double skin_eff   = 58.0 + (smc->bottom_skin_c - 38.0) * 3.2;
    double fin_eff    = smc->fin_stack_c + 14.0;
    double budget_eff = (smc->chassis_max_w > 10.0 && smc->chassis_max_w < 76.0)
                        ? (60.0 + (76.0 - smc->chassis_max_w) * 0.85)
                        : 50.0;

    double ctrl_temp = smc->peci_max_c;
    if (skin_eff > ctrl_temp)   ctrl_temp = skin_eff;
    if (fin_eff > ctrl_temp)    ctrl_temp = fin_eff;
    if (budget_eff > ctrl_temp) ctrl_temp = budget_eff;

    float target_f0 = A2141_FAN0_STOCK_MIN_RPM;
    float target_f1 = A2141_FAN1_STOCK_MIN_RPM;

    if (ctrl_temp <= 58.0) {
        target_f0 = A2141_FAN0_STOCK_MIN_RPM;
        target_f1 = A2141_FAN1_STOCK_MIN_RPM;
    } else if (ctrl_temp >= 80.0) {
        target_f0 = A2141_FAN0_MAX_RPM;
        target_f1 = A2141_FAN1_MAX_RPM;
    } else {
        double frac = (ctrl_temp - 58.0) / (80.0 - 58.0);
        target_f0 = (float)(2300.0 + frac * (A2141_FAN0_MAX_RPM - 2300.0));
        target_f1 = (float)(2120.0 + frac * (A2141_FAN1_MAX_RPM - 2120.0));
    }

    if (fabsf(target_f0 - *io_f0_min) >= 60.0f) {
        if (a2141_smc_set_min_fans(target_f0, target_f1) == 0) {
            *io_f0_min = target_f0;
        }
    }

    /* 2. Dynamic PZ0E-Aware RAPL Power Budgeting (CPU <-> AMD dGPU Arbitration)
     *    - When the AMD Radeon Pro dGPU demands high power (e.g. 22W-42W in 3D/Video),
     *      we lower the CPU floor down to 16W so we never starve the GPU or exceed PZ0E.
     *    - When the AMD dGPU is idle/off (< 5W) and temps are cool, we allow CPU PL1
     *      up to 48W (PL2 60W) so the CPU gets full power when the GPU doesn't need it.
     */
    double pz0e = (smc->chassis_max_w > 25.0) ? smc->chassis_max_w : 65.0;
    double gpu_w = (smc->gpu_power_w >= 0.0) ? smc->gpu_power_w : 18.0;
    int avail_cpu_w = (int)(pz0e - gpu_w - 2.5);

    int min_cpu_floor = (gpu_w >= 22.0) ? 16 : 22;
    int max_cpu_ceil  = (gpu_w < 5.0 && smc->peci_max_c < 74.0) ? 48 : 42;

    if (avail_cpu_w < min_cpu_floor) avail_cpu_w = min_cpu_floor;
    if (avail_cpu_w > max_cpu_ceil)  avail_cpu_w = max_cpu_ceil;

    if (smc->peci_max_c >= 85.0 && avail_cpu_w > 30) {
        avail_cpu_w = 30;
    } else if (smc->peci_max_c >= 78.0 && avail_cpu_w > 35) {
        avail_cpu_w = 35;
    }

    int target_pl1 = avail_cpu_w;
    int max_pl2    = (gpu_w < 5.0) ? 60 : 54;
    int target_pl2 = (target_pl1 + 12 <= max_pl2) ? (target_pl1 + 12) : max_pl2;

    if (target_pl1 != *io_pl1) {
        if (set_sysctl_int_silent("machdep.a2141_pl1_watts", target_pl1) == 0) {
            set_sysctl_int_silent("machdep.a2141_pl2_watts", target_pl2);
            *io_pl1 = target_pl1;
        }
    }

    /* 3. Tighter Thermal Profile Hysteresis (68C Burst / 75C Heavy) */
    if (smc->peci_max_c >= (double)A2141_HYSTERESIS_HEAVY_MIN_C && *io_profile != 1) {
        if (set_sysctl_int_silent("machdep.a2141_mode_override", 1) == 0) {
            *io_profile = 1;
        }
    } else if (smc->peci_max_c <= (double)A2141_HYSTERESIS_BURST_MAX_C && *io_profile != 0) {
        if (set_sysctl_int_silent("machdep.a2141_mode_override", 0) == 0) {
            *io_profile = 0;
        }
    }
}

static void init_optimal_tunables(void) {
    xcpm_set_dvfs_pause(1);
    set_sysctl_int_silent("machdep.a2141_enabled", 1);
    set_sysctl_int_silent("machdep.a2141_tcc_offset", A2141_DEFAULT_TCC_OFFSET);
    set_sysctl_int_silent("machdep.a2141_core0_burst_ratio", 32);
    set_sysctl_int_silent("machdep.a2141_core0_heavy_ratio", 29);
}

static void cmd_daemon(void) {
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    init_optimal_tunables();

    int cur_profile = -1;
    int cur_pl1     = -1;
    float cur_f0    = -1.0f;
    unsigned long tick = 0;

    printf("A2141 Proactive Thermal & HWP Daemon v%s iniciado (XCPM DVFS Paused).\n", A2141_DRIVER_VERSION);
    fflush(stdout);

    while (g_keep_running) {
        /* Periodically re-assert XCPM DVFS pause every 5 seconds (10 ticks) */
        if ((tick % 10UL) == 0UL) {
            xcpm_set_dvfs_pause(1);
        }

        a2141_smc_snapshot_t smc;
        if (a2141_smc_read_snapshot(&smc) == 0) {
            apply_proactive_control_step(&smc, &cur_profile, &cur_pl1, &cur_f0);
        }

        tick++;
        usleep(500000); /* 500ms proactive loop */
    }

    /* Restore stock minimum fan speeds on clean daemon shutdown */
    a2141_smc_set_min_fans(A2141_FAN0_STOCK_MIN_RPM, A2141_FAN1_STOCK_MIN_RPM);
    printf("A2141 Daemon detenido limpiamente.\n");
}

static void cmd_status(void) {
    a2141_smc_snapshot_t smc;
    a2141_telemetry_t kext;
    uint32_t actual_ratios[A2141_PHYSICAL_CORES];
    int kext_loaded = (read_kext_telemetry(&kext) == 0);
    int has_actual  = (read_core_actual_ratios(actual_ratios) == 0);

    if (a2141_smc_read_snapshot(&smc) != 0) {
        fprintf(stderr, "Error: No se pudo abrir AppleSMC.\n");
        return;
    }

    printf("==========================================================================\n");
    printf(" MacBook Pro 16,1 (A2141) Asymmetric HWP & Thermal Telemetry v%s\n", A2141_DRIVER_VERSION);
    printf(" Author: mylnzon58 (Juan Monzon)\n");
    printf("==========================================================================\n\n");

    printf("[1] Estado del Driver Ring-0 (A2141AsymmetricHWP.kext + XCPM Override):\n");
    if (kext_loaded) {
        const char *prof_str = (kext.active_profile == A2141_PROFILE_HEAVY)
                               ? "HEAVY (>=75C)"
                               : "BURST (<=68C)";
        printf("    Estado Kext:       ACTIVO (Ciclos Ring-0: %llu)\n", (unsigned long long)kext.enforce_cycles);
        printf("    Perfil Activo:     %s\n", prof_str);
        printf("    Limites RAPL:      PL1 = %u W | PL2 = %u W (Reseteos T2 anulados: %llu)\n",
               kext.pl1_watts, kext.pl2_watts, (unsigned long long)kext.rapl_Override_count);
        printf("    TCC Offset:        -%u C (Techo de silicio = %u C)\n",
               kext.tcc_offset, 100U - kext.tcc_offset);
        printf("    MSR 0x610 (RAPL):  0x%016llx\n", (unsigned long long)kext.last_pkg_power_msr);
    } else {
        printf("    Estado Kext:       NO CARGADO (Modo telemetria SMC lectura directa)\n");
        printf("    Para cargar:       Requiere SIP desactivado y ejecutar: sudo ./install.sh\n");
    }
    printf("\n");

    printf("[2] Telemetria Por Nucleo Fisico (Intel Core i7-9750H 6C/12T):\n");
    printf("    Nucleo   | Temp SMC | Temp MSR | Tope HWP (0x774)  | Recortes Guardia\n");
    printf("    ---------+----------+----------+-------------------+-----------------\n");
    uint32_t default_burst[A2141_PHYSICAL_CORES] = A2141_BURST_RATIOS_INIT;
    for (int i = 0; i < A2141_PHYSICAL_CORES; i++) {
        if (kext_loaded) {
            if (has_actual && actual_ratios[i] > 0) {
                printf("    Core %d   |  %5.1f C |   %4u C | %4u MHz (Act %2u) | %8u eventos\n",
                       i, smc.core_temp_c[i], kext.core_temp_c[i],
                       kext.core_applied_ratio[i] * 100U, actual_ratios[i], kext.core_guard_events[i]);
            } else {
                printf("    Core %d   |  %5.1f C |   %4u C |       %4u MHz    | %8u eventos\n",
                       i, smc.core_temp_c[i], kext.core_temp_c[i],
                       kext.core_applied_ratio[i] * 100U, kext.core_guard_events[i]);
            }
        } else {
            printf("    Core %d   |  %5.1f C |      N/A |  Target %4u MHz  |      N/A\n",
                   i, smc.core_temp_c[i], default_burst[i] * 100U);
        }
    }
    printf("\n");

    printf("[3] Sensores del Chip Apple T2 (SMC), Chasis y Potencia en Vivo:\n");
    printf("    PECI Max Die (TCXC):     %6.2f C\n", smc.peci_max_c);
    printf("    T2 Filtrado  (TC0E):     %6.2f C (Desfase actual: %+.2f C)\n",
           smc.t2_filtered_c, smc.peci_max_c - smc.t2_filtered_c);
    printf("    Disipador    (Th1H):     %6.2f C | Piel Inferior (Ts0S): %.2f C\n",
           smc.fin_stack_c, smc.bottom_skin_c);
    printf("    GPU Radeon   (TGDD):     %6.2f C\n", smc.gpu_temp_c);
    printf("    Potencia CPU (PC0R):     %6.2f W\n", smc.cpu_power_w);
    printf("    Potencia GPU (PG0R):     %6.2f W\n", smc.gpu_power_w);
    printf("    CPU + GPU    (PHPC):     %6.2f W / %.2f W Presupuesto (PZ0E)\n",
           smc.combined_power_w, smc.chassis_max_w);
    printf("    Potencia Sys (PSTR):     %6.2f W\n", smc.sys_power_w);
    printf("    Turbinas Act (F0/F1):    %4.0f RPM / %4.0f RPM (Min F0Mn/F1Mn: %.0f / %.0f)\n",
           smc.fan0_rpm, smc.fan1_rpm, smc.fan0_min_rpm, smc.fan1_min_rpm);
    printf("    Speed Limit  (XCPM):     %4d %%\n", smc.cpu_speed_limit_pct);
    printf("==========================================================================\n");
}

static void cmd_monitor(int interval_ms) {
    if (interval_ms < 100) interval_ms = 100;
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    printf("Monitoreo en vivo cada %d ms (Ctrl+C para salir)...\n\n", interval_ms);
    printf("Core0 | Core1 | Core2 | Core3 | Core4 | Core5 | MaxDie | Ts0S(Skin) | CPU_W | PZ0E_W | Fan0 | SpeedLim | Perfil\n");
    printf("------+-------+-------+-------+-------+-------+--------+------------+-------+--------+------+----------+-------\n");

    while (g_keep_running) {
        a2141_smc_snapshot_t smc;
        a2141_telemetry_t kext;
        int kext_ok = (read_kext_telemetry(&kext) == 0);
        if (a2141_smc_read_snapshot(&smc) == 0) {
            const char *prof = "STOCK";
            if (kext_ok) {
                prof = (kext.active_profile == A2141_PROFILE_HEAVY) ? "HEAVY" : "BURST";
            }
            printf("%5.0f | %5.0f | %5.0f | %5.0f | %5.0f | %5.0f | %6.1f |   %6.2f C | %5.1f |  %5.1f | %4.0f |    %3d%%  | %-6s\n",
                   smc.core_temp_c[0], smc.core_temp_c[1], smc.core_temp_c[2],
                   smc.core_temp_c[3], smc.core_temp_c[4], smc.core_temp_c[5],
                   smc.peci_max_c, smc.bottom_skin_c,
                   smc.cpu_power_w, smc.chassis_max_w,
                   smc.fan0_rpm, smc.cpu_speed_limit_pct, prof);
            fflush(stdout);
        }
        usleep((useconds_t)(interval_ms * 1000));
    }
    printf("\nMonitoreo finalizado.\n");
}

static volatile int g_bench_running = 0;
static unsigned long long g_thread_ops[A2141_LOGICAL_CPUS];

static void *bench_worker(void *arg) {
    int tid = *(int *)arg;
    double a[8];
    for (int i = 0; i < 8; i++) a[i] = 1.000001 * (i + 1 + tid);
    unsigned long long ops = 0;

    while (g_bench_running) {
        for (int k = 0; k < 100000; k++) {
            a[0] = fma(a[0], 1.0000001, 0.0000001);
            a[1] = fma(a[1], 1.0000001, 0.0000001);
            a[2] = fma(a[2], 1.0000001, 0.0000001);
            a[3] = fma(a[3], 1.0000001, 0.0000001);
            a[4] = fma(a[4], 1.0000001, 0.0000001);
            a[5] = fma(a[5], 1.0000001, 0.0000001);
            a[6] = fma(a[6], 1.0000001, 0.0000001);
            a[7] = fma(a[7], 1.0000001, 0.0000001);
        }
        ops += 800000ULL;
    }
    g_thread_ops[tid] = ops;
    if (a[0] == -999.0) printf("%f", a[0]);
    return NULL;
}

static void cmd_benchmark(int seconds) {
    if (seconds < 3) seconds = 3;
    if (seconds > 120) seconds = 120;

    init_optimal_tunables();

    printf("==================================================================================\n");
    printf(" Benchmark de Estres FMA/AVX2 (12 Hilos - %d Segundos)\n", seconds);
    printf("==================================================================================\n");
    printf("Sec | Core0 | Core1 | Core2 | Core3 | Core4 | Core5 | MaxDie | Ts0S  | CPU_W | PZ0E_W | Fan0 | SpeedLim | Perfil\n");
    printf("----+-------+-------+-------+-------+-------+-------+--------+-------+-------+--------+------+----------+-------\n");

    pthread_t th[A2141_LOGICAL_CPUS];
    int ids[A2141_LOGICAL_CPUS];
    g_bench_running = 1;
    for (int i = 0; i < A2141_LOGICAL_CPUS; i++) {
        ids[i] = i;
        g_thread_ops[i] = 0;
        pthread_create(&th[i], NULL, bench_worker, &ids[i]);
    }

    int cur_profile = -1;
    int cur_pl1     = -1;
    float cur_f0    = -1.0f;

    for (int s = 1; s <= seconds; s++) {
        sleep(1);
        a2141_smc_snapshot_t smc;
        a2141_telemetry_t kext;
        int kext_ok = (read_kext_telemetry(&kext) == 0);
        if (a2141_smc_read_snapshot(&smc) == 0) {
            apply_proactive_control_step(&smc, &cur_profile, &cur_pl1, &cur_f0);
            const char *prof = kext_ok ? ((kext.active_profile == A2141_PROFILE_HEAVY) ? "HEAVY" : "BURST") : "STOCK";
            printf("%3d | %5.0f | %5.0f | %5.0f | %5.0f | %5.0f | %5.0f | %6.1f | %5.1f | %5.1f |  %5.1f | %4.0f |    %3d%%  | %-6s\n",
                   s, smc.core_temp_c[0], smc.core_temp_c[1], smc.core_temp_c[2],
                   smc.core_temp_c[3], smc.core_temp_c[4], smc.core_temp_c[5],
                   smc.peci_max_c, smc.bottom_skin_c,
                   smc.cpu_power_w, smc.chassis_max_w, smc.fan0_rpm, smc.cpu_speed_limit_pct, prof);
            fflush(stdout);
        }
    }

    g_bench_running = 0;
    unsigned long long total_ops = 0;
    for (int i = 0; i < A2141_LOGICAL_CPUS; i++) {
        pthread_join(th[i], NULL);
        total_ops += g_thread_ops[i];
    }

    double gops = (double)total_ops / ((double)seconds * 1e9);
    printf("----------------------------------------------------------------------------------\n");
    printf(" Throughput Sostenido: %.2f GigaFLOPs/s (%llu ops en %d s)\n", gops, total_ops, seconds);
    printf("==================================================================================\n");
}

static void print_usage(const char *prog) {
    printf("Uso: %s <comando> [argumentos]\n\n", prog);
    printf("Comandos:\n");
    printf("  status                    Muestra estado del driver Ring-0, MSRs y sensores SMC\n");
    printf("  daemon                    Inicia el demonio proactivo (XCPM Pause + Turbinas + PZ0E RAPL)\n");
    printf("  monitor [intervalo_ms]    Tabla de telemetria en tiempo real (default: 1000 ms)\n");
    printf("  benchmark [segundos]      Ejecuta benchmark FMA/AVX2 en 12 hilos con telemetria (default: 10 s)\n");
    printf("  profile <auto|burst|heavy> Cambia el modo de perfil HWP del kext en caliente\n");
    printf("  power <pl1_w> <pl2_w>     Ajusta limites RAPL PL1 y PL2 en Watts (ej: 38 52)\n");
    printf("  tcc <offset_c>            Ajusta el TCC Offset bajo 100C (ej: 6 -> 94C)\n");
    printf("  fans <auto|max|rpm>       Ajusta el piso minimo de las turbinas en RPM\n");
}

int main(int argc, char **argv) {
    const char *cmd = (argc > 1) ? argv[1] : "status";

    if (strcmp(cmd, "status") == 0) {
        cmd_status();
    } else if (strcmp(cmd, "daemon") == 0) {
        cmd_daemon();
    } else if (strcmp(cmd, "monitor") == 0) {
        int ms = (argc > 2) ? atoi(argv[2]) : 1000;
        cmd_monitor(ms);
    } else if (strcmp(cmd, "benchmark") == 0) {
        int sec = (argc > 2) ? atoi(argv[2]) : 10;
        cmd_benchmark(sec);
    } else if (strcmp(cmd, "profile") == 0 && argc > 2) {
        int mode = -1;
        if (strcmp(argv[2], "burst") == 0) mode = 0;
        else if (strcmp(argv[2], "heavy") == 0) mode = 1;
        else if (strcmp(argv[2], "auto") == 0) mode = -1;
        else {
            fprintf(stderr, "Perfil invalido: use auto, burst o heavy.\n");
            return 1;
        }
        if (set_sysctl_int("machdep.a2141_mode_override", mode) == 0) {
            printf("Perfil HWP actualizado a: %s\n", argv[2]);
        }
    } else if (strcmp(cmd, "power") == 0 && argc > 3) {
        int pl1 = atoi(argv[2]);
        int pl2 = atoi(argv[3]);
        if (set_sysctl_int("machdep.a2141_pl1_watts", pl1) == 0 &&
            set_sysctl_int("machdep.a2141_pl2_watts", pl2) == 0) {
            printf("Limites RAPL actualizados a: PL1=%dW, PL2=%dW\n", pl1, pl2);
        }
    } else if (strcmp(cmd, "tcc") == 0 && argc > 2) {
        int tcc = atoi(argv[2]);
        if (set_sysctl_int("machdep.a2141_tcc_offset", tcc) == 0) {
            printf("TCC Offset actualizado a: -%dC (Techo = %dC)\n", tcc, 100 - tcc);
        }
    } else if (strcmp(cmd, "fans") == 0 && argc > 2) {
        float f0 = A2141_FAN0_STOCK_MIN_RPM;
        float f1 = A2141_FAN1_STOCK_MIN_RPM;
        if (strcmp(argv[2], "max") == 0) {
            f0 = A2141_FAN0_MAX_RPM;
            f1 = A2141_FAN1_MAX_RPM;
        } else if (strcmp(argv[2], "auto") != 0) {
            f0 = (float)atof(argv[2]);
            f1 = f0 * (A2141_FAN1_MAX_RPM / A2141_FAN0_MAX_RPM);
        }
        if (a2141_smc_set_min_fans(f0, f1) == 0) {
            printf("Turbinas F0Mn/F1Mn ajustadas a: %.0f / %.0f RPM\n", f0, f1);
        } else {
            fprintf(stderr, "Error al escribir F0Mn/F1Mn en AppleSMC (requiere sudo).\n");
            return 1;
        }
    } else {
        print_usage(argv[0]);
        return 1;
    }

    a2141_smc_close();
    return 0;
}
