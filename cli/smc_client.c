/*
 * cli/smc_client.c
 * MacBook Pro 16,1 (A2141 - Intel Core i7-9750H) AppleSMC Telemetry & Fan Control
 * Author: Juan José Monzón (@mylnzon58)
 */

#include "smc_client.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <IOKit/IOKitLib.h>

#define KERNEL_INDEX_SMC     2
#define SMC_CMD_READ_BYTES   5
#define SMC_CMD_WRITE_BYTES  6
#define SMC_CMD_READ_KEYINFO 9

typedef struct {
    char major;
    char minor;
    char build;
    char reserved[1];
    uint16_t release;
} smc_vers_t;

typedef struct {
    uint16_t version;
    uint16_t length;
    uint32_t cpuPLimit;
    uint32_t gpuPLimit;
    uint32_t memPLimit;
} smc_plimit_t;

typedef struct {
    uint32_t dataSize;
    uint32_t dataType;
    char dataAttributes;
} smc_keyinfo_t;

typedef unsigned char smc_bytes_t[32];

typedef struct {
    uint32_t key;
    smc_vers_t vers;
    smc_plimit_t pLimitData;
    smc_keyinfo_t keyInfo;
    char result;
    char status;
    char data8;
    uint32_t data32;
    smc_bytes_t bytes;
} smc_keydata_t;

static io_connect_t g_smc_conn = 0;

static uint32_t key_to_u32(const char *str) {
    return ((uint32_t)str[0] << 24) |
           ((uint32_t)str[1] << 16) |
           ((uint32_t)str[2] << 8)  |
           (uint32_t)str[3];
}

static void u32_to_type(uint32_t val, char *str) {
    str[0] = (char)((val >> 24) & 0xFF);
    str[1] = (char)((val >> 16) & 0xFF);
    str[2] = (char)((val >> 8) & 0xFF);
    str[3] = (char)(val & 0xFF);
    str[4] = '\0';
}

int a2141_smc_open(void) {
    if (g_smc_conn != 0) {
        return 0;
    }
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"));
    if (!service) {
        return -1;
    }
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &g_smc_conn);
    IOObjectRelease(service);
    return (kr == kIOReturnSuccess) ? 0 : -1;
}

void a2141_smc_close(void) {
    if (g_smc_conn != 0) {
        IOServiceClose(g_smc_conn);
        g_smc_conn = 0;
    }
}

static double smc_read_key(const char *key) {
    if (g_smc_conn == 0 && a2141_smc_open() != 0) {
        return -1.0;
    }

    smc_keydata_t in, out;
    size_t outSize = sizeof(smc_keydata_t);
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));

    in.key = key_to_u32(key);
    in.data8 = SMC_CMD_READ_KEYINFO;

    if (IOConnectCallStructMethod(g_smc_conn, KERNEL_INDEX_SMC, &in, sizeof(in), &out, &outSize) != kIOReturnSuccess) {
        return -1.0;
    }

    uint32_t dataSize = out.keyInfo.dataSize;
    uint32_t dataType = out.keyInfo.dataType;
    if (dataSize == 0) {
        return -1.0;
    }

    memset(&in, 0, sizeof(in));
    in.key = key_to_u32(key);
    in.keyInfo.dataSize = dataSize;
    in.data8 = SMC_CMD_READ_BYTES;
    memset(&out, 0, sizeof(out));

    if (IOConnectCallStructMethod(g_smc_conn, KERNEL_INDEX_SMC, &in, sizeof(in), &out, &outSize) != kIOReturnSuccess) {
        return -1.0;
    }

    char typeStr[5];
    u32_to_type(dataType, typeStr);

    if (strcmp(typeStr, "flt ") == 0 && dataSize == 4) {
        float f = 0.0f;
        memcpy(&f, out.bytes, 4);
        return (double)f;
    } else if (strcmp(typeStr, "sp78") == 0 && dataSize == 2) {
        int16_t v = (int16_t)((out.bytes[0] << 8) | out.bytes[1]);
        return v / 256.0;
    } else if (strcmp(typeStr, "fpe2") == 0 && dataSize == 2) {
        uint16_t v = (uint16_t)((out.bytes[0] << 8) | out.bytes[1]);
        return v / 4.0;
    } else if (strcmp(typeStr, "ui16") == 0 && dataSize == 2) {
        return (double)((out.bytes[0] << 8) | out.bytes[1]);
    } else if (strcmp(typeStr, "ui8 ") == 0 && dataSize == 1) {
        return (double)out.bytes[0];
    }
    return -1.0;
}

int a2141_smc_write_flt(const char *key, float val) {
    if (g_smc_conn == 0 && a2141_smc_open() != 0) {
        return -1;
    }

    smc_keydata_t in, out;
    size_t outSize = sizeof(smc_keydata_t);
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));

    in.key = key_to_u32(key);
    in.data8 = SMC_CMD_READ_KEYINFO;

    if (IOConnectCallStructMethod(g_smc_conn, KERNEL_INDEX_SMC, &in, sizeof(in), &out, &outSize) != kIOReturnSuccess) {
        return -1;
    }

    if (out.keyInfo.dataSize != 4) {
        return -1;
    }

    memset(&in, 0, sizeof(in));
    in.key = key_to_u32(key);
    in.data8 = SMC_CMD_WRITE_BYTES;
    in.keyInfo.dataSize = 4;
    memcpy(in.bytes, &val, 4);
    memset(&out, 0, sizeof(out));

    kern_return_t kr = IOConnectCallStructMethod(g_smc_conn, KERNEL_INDEX_SMC, &in, sizeof(in), &out, &outSize);
    if (kr != kIOReturnSuccess || out.result != 0) {
        return -1;
    }
    return 0;
}

int a2141_smc_set_min_fans(float fan0_min_rpm, float fan1_min_rpm) {
    if (fan0_min_rpm < A2141_FAN0_STOCK_MIN_RPM) fan0_min_rpm = A2141_FAN0_STOCK_MIN_RPM;
    if (fan0_min_rpm > A2141_FAN0_MAX_RPM)       fan0_min_rpm = A2141_FAN0_MAX_RPM;
    if (fan1_min_rpm < A2141_FAN1_STOCK_MIN_RPM) fan1_min_rpm = A2141_FAN1_STOCK_MIN_RPM;
    if (fan1_min_rpm > A2141_FAN1_MAX_RPM)       fan1_min_rpm = A2141_FAN1_MAX_RPM;

    int r0 = a2141_smc_write_flt("F0Mn", fan0_min_rpm);
    int r1 = a2141_smc_write_flt("F1Mn", fan1_min_rpm);
    return (r0 == 0 && r1 == 0) ? 0 : -1;
}

static int read_pmset_speed_limit(void) {
    static int cached_limit = 100;
    static unsigned int call_counter = 0;
    /* Only spawn pmset every 16 calls (~8 seconds in daemon) to keep CPU overhead at 0.0% */
    if ((call_counter++ % 16U) != 0U) {
        return cached_limit;
    }
    FILE *fp = popen("pmset -g therm 2>/dev/null | awk '/CPU_Speed_Limit/{print $3}'", "r");
    if (!fp) {
        return cached_limit;
    }
    int limit = 100;
    if (fscanf(fp, "%d", &limit) == 1) {
        cached_limit = limit;
    }
    pclose(fp);
    return cached_limit;
}

int a2141_smc_read_snapshot(a2141_smc_snapshot_t *out) {
    if (!out) {
        return -1;
    }
    if (g_smc_conn == 0 && a2141_smc_open() != 0) {
        return -1;
    }

    static const char *core_keys[A2141_PHYSICAL_CORES] = {
#ifdef A2141_MODEL_I9
        "TC1C", "TC2C", "TC3C", "TC4C", "TC5C", "TC6C", "TC7C", "TC8C"
#else
        "TC1C", "TC2C", "TC3C", "TC4C", "TC5C", "TC6C"
#endif
    };

    for (int i = 0; i < A2141_PHYSICAL_CORES; i++) {
        out->core_temp_c[i] = smc_read_key(core_keys[i]);
    }
    out->peci_max_c          = smc_read_key("TCXC");
    out->t2_filtered_c       = smc_read_key("TC0E");
    out->t2_adjusted_c       = smc_read_key("TC0F");
    out->gpu_temp_c          = smc_read_key("TGDD");
    out->fin_stack_c         = smc_read_key("Th1H");
    out->bottom_skin_c       = smc_read_key("Ts0S");
    out->cpu_power_w         = smc_read_key("PC0R");
    out->gpu_power_w         = smc_read_key("PG0R");
    out->combined_power_w    = smc_read_key("PHPC");
    out->sys_power_w         = smc_read_key("PSTR");
    out->chassis_max_w       = smc_read_key("PZ0E");
    out->fan0_rpm            = smc_read_key("F0Ac");
    out->fan1_rpm            = smc_read_key("F1Ac");
    out->fan0_min_rpm        = smc_read_key("F0Mn");
    out->fan1_min_rpm        = smc_read_key("F1Mn");
    out->cpu_speed_limit_pct = read_pmset_speed_limit();

    return 0;
}
