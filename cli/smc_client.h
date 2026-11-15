/*
 * cli/smc_client.h
 * MacBook Pro 16,1 (A2141 - Intel Core i7-9750H) AppleSMC Telemetry & Control Interface
 * Author: Juan José Monzón (@mylnzon58)
 */

#ifndef A2141_SMC_CLIENT_H
#define A2141_SMC_CLIENT_H

#include "../include/a2141_shared.h"

#define A2141_FAN0_STOCK_MIN_RPM    1836.0f
#define A2141_FAN0_MAX_RPM          5297.0f
#define A2141_FAN1_STOCK_MIN_RPM    1700.0f
#define A2141_FAN1_MAX_RPM          4905.0f

typedef struct {
    double core_temp_c[A2141_PHYSICAL_CORES]; /* TC1C..TC6C */
    double peci_max_c;                        /* TCXC (Instantaneous Max Die) */
    double t2_filtered_c;                     /* TC0E (~15s low-pass filter) */
    double t2_adjusted_c;                     /* TC0F */
    double gpu_temp_c;                        /* TGDD (Radeon Pro 5300M) */
    double fin_stack_c;                       /* Th1H (Heatsink Fin Stack) */
    double bottom_skin_c;                     /* Ts0S (Bottom Case Skin Sensor) */
    double cpu_power_w;                       /* PC0R */
    double gpu_power_w;                       /* PG0R */
    double combined_power_w;                  /* PHPC / PZ0G (CPU + dGPU) */
    double sys_power_w;                       /* PSTR */
    double chassis_max_w;                     /* PZ0E (Dynamic CPU+dGPU Budget: 36W..82W) */
    double fan0_rpm;                          /* F0Ac */
    double fan1_rpm;                          /* F1Ac */
    double fan0_min_rpm;                      /* F0Mn */
    double fan1_min_rpm;                      /* F1Mn */
    int    cpu_speed_limit_pct;               /* pmset -g therm CPU_Speed_Limit */
} a2141_smc_snapshot_t;

int  a2141_smc_open(void);
void a2141_smc_close(void);
int  a2141_smc_read_snapshot(a2141_smc_snapshot_t *out);
int  a2141_smc_write_flt(const char *key, float val);
int  a2141_smc_set_min_fans(float fan0_min_rpm, float fan1_min_rpm);

#endif /* A2141_SMC_CLIENT_H */
