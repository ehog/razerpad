/*
 * razerpad — fan-curve + lighting controller for the Razer Laptop Cooling Pad
 * on macOS (Intel).
 *
 * Reads the CPU die temperature from the SMC and drives the pad's fans over
 * USB HID feature reports. The pad speaks the standard Razer Chroma wire
 * protocol (90-byte reports, XOR checksum), so lighting is driven with the
 * standard "extended matrix" commands (class 0x0F). Fan protocol originally
 * reverse-engineered by github.com/Benson5650/FanControl.RazerCoolingPadPlugin
 * and github.com/Nox1con/razer-coolingpad-linux.
 *
 * Build:  make
 * Usage:  razerpad                 run the fan-curve loop (daemon mode)
 *         razerpad -t              print CPU temperature and exit
 *         razerpad -r              print the pad's current commanded RPM
 *         razerpad -s 2000         set a fixed RPM and exit
 *         razerpad -o              release fan control back to the pad
 *         razerpad -L static:00ff00 | spectrum | wave | wave2 |
 *                     breath:RRGGBB | off        set lighting effect
 *         razerpad -B 80           set LED brightness (0-100)
 *         razerpad -v              verbose loop
 *         razerpad -i SECS         poll interval (default 3)
 *         razerpad -C "40:0,60:15,75:25,85:40,93:65,98:100"  custom curve
 *
 * Daemon mode also:
 *   - re-reads ~/.config/razerpad/config (key=value) when it changes
 *   - writes live status JSON to /tmp/razerpad-status.json each cycle
 */

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ───────────────────────── SMC (CPU temperature) ───────────────────────── */

#define KERNEL_INDEX_SMC 2
#define SMC_CMD_READ_BYTES 5
#define SMC_CMD_READ_KEYINFO 9

typedef struct {
    char major;
    char minor;
    char build;
    char reserved[1];
    UInt16 release;
} SMCKeyData_vers_t;

typedef struct {
    UInt16 version;
    UInt16 length;
    UInt32 cpuPLimit;
    UInt32 gpuPLimit;
    UInt32 memPLimit;
} SMCKeyData_pLimitData_t;

typedef struct {
    UInt32 dataSize;
    UInt32 dataType;
    char dataAttributes;
} SMCKeyData_keyInfo_t;

typedef unsigned char SMCBytes_t[32];

typedef struct {
    UInt32 key;
    SMCKeyData_vers_t vers;
    SMCKeyData_pLimitData_t pLimitData;
    SMCKeyData_keyInfo_t keyInfo;
    char result;
    char status;
    char data8;
    UInt32 data32;
    SMCBytes_t bytes;
} SMCKeyData_t;

static io_connect_t g_smc_conn = 0;

static kern_return_t smc_open(void) {
    io_service_t service = IOServiceGetMatchingService(
        kIOMainPortDefault, IOServiceMatching("AppleSMC"));
    if (!service)
        return kIOReturnNotFound;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &g_smc_conn);
    IOObjectRelease(service);
    return kr;
}

static kern_return_t smc_call(SMCKeyData_t *in, SMCKeyData_t *out) {
    size_t out_size = sizeof(SMCKeyData_t);
    return IOConnectCallStructMethod(g_smc_conn, KERNEL_INDEX_SMC,
                                     in, sizeof(SMCKeyData_t),
                                     out, &out_size);
}

/* Read one SMC key; returns temperature in °C or -1 on failure. */
static double smc_read_temp(const char *key) {
    SMCKeyData_t in, out;
    UInt32 k = ((UInt32)key[0] << 24) | ((UInt32)key[1] << 16) |
               ((UInt32)key[2] << 8) | (UInt32)key[3];

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.key = k;
    in.data8 = SMC_CMD_READ_KEYINFO;
    if (smc_call(&in, &out) != kIOReturnSuccess || out.result != 0)
        return -1;

    UInt32 size = out.keyInfo.dataSize;
    UInt32 type = out.keyInfo.dataType;

    memset(&in, 0, sizeof(in));
    in.key = k;
    in.keyInfo.dataSize = size;
    in.data8 = SMC_CMD_READ_BYTES;
    memset(&out, 0, sizeof(out));
    if (smc_call(&in, &out) != kIOReturnSuccess || out.result != 0)
        return -1;

    if (type == 'sp78' && size == 2) {
        SInt16 raw = (SInt16)(((UInt16)out.bytes[0] << 8) | out.bytes[1]);
        return raw / 256.0;
    }
    if (type == 'flt ' && size == 4) {
        float f;
        memcpy(&f, out.bytes, 4);
        return f;
    }
    return -1;
}

/* CPU sensor keys to try; the hottest sane reading wins. */
static const char *TEMP_KEYS[] = {"TC0P", "TC0D", "TC0E", "TC0F", "TC1C",
                                  "TC2C", "TC3C", "TC4C", "TC5C", "TC6C"};

/* Optional external temperature source (config: temp_cmd=<shell command>).
 * The command must print a Celsius float. Lets non-Intel Macs (or anyone
 * with a preferred sensor tool) plug in their own reading. */
static char g_temp_cmd[512] = "";

static double cpu_temp(void) {
    if (g_temp_cmd[0]) {
        FILE *p = popen(g_temp_cmd, "r");
        if (!p)
            return -1;
        double t = -1;
        if (fscanf(p, "%lf", &t) != 1)
            t = -1;
        pclose(p);
        return (t > 1.0 && t < 130.0) ? t : -1;
    }
    double best = -1;
    for (size_t i = 0; i < sizeof(TEMP_KEYS) / sizeof(*TEMP_KEYS); i++) {
        double t = smc_read_temp(TEMP_KEYS[i]);
        if (t > 1.0 && t < 120.0 && t > best)
            best = t;
    }
    return best;
}

/* ─────────────────────── Razer Cooling Pad (HID) ────────────────────────── */

#define RZR_VID 0x1532
#define RZR_PID 0x0F43
#define RZR_REPORT_LEN 90 /* feature report body, excluding report ID 0 */
#define RZR_MIN_RPM 500
#define RZR_MAX_RPM 3200

static IOHIDManagerRef g_hid_mgr = NULL;
static IOHIDDeviceRef g_pad = NULL;

static int device_int_prop(IOHIDDeviceRef dev, CFStringRef key) {
    CFTypeRef ref = IOHIDDeviceGetProperty(dev, key);
    int val = -1;
    if (ref && CFGetTypeID(ref) == CFNumberGetTypeID())
        CFNumberGetValue((CFNumberRef)ref, kCFNumberIntType, &val);
    return val;
}

static void pad_close(void) {
    if (g_pad) {
        IOHIDDeviceClose(g_pad, kIOHIDOptionsTypeNone);
        CFRelease(g_pad);
        g_pad = NULL;
    }
}

/* Find and open the pad's control interface (the one with the 90/91-byte
 * feature report). Returns 1 on success. */
static int pad_open(void) {
    if (g_pad)
        return 1;
    if (!g_hid_mgr) {
        g_hid_mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
        int vid = RZR_VID, pid = RZR_PID;
        CFNumberRef vref = CFNumberCreate(NULL, kCFNumberIntType, &vid);
        CFNumberRef pref = CFNumberCreate(NULL, kCFNumberIntType, &pid);
        CFStringRef keys[] = {CFSTR(kIOHIDVendorIDKey), CFSTR(kIOHIDProductIDKey)};
        CFTypeRef vals[] = {vref, pref};
        CFDictionaryRef match = CFDictionaryCreate(
            NULL, (const void **)keys, (const void **)vals, 2,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        IOHIDManagerSetDeviceMatching(g_hid_mgr, match);
        CFRelease(match);
        CFRelease(vref);
        CFRelease(pref);
        IOHIDManagerOpen(g_hid_mgr, kIOHIDOptionsTypeNone);
    }

    CFSetRef devs = IOHIDManagerCopyDevices(g_hid_mgr);
    if (!devs)
        return 0;
    CFIndex n = CFSetGetCount(devs);
    if (n > 0) {
        IOHIDDeviceRef *list = calloc((size_t)n, sizeof(IOHIDDeviceRef));
        CFSetGetValues(devs, (const void **)list);
        for (CFIndex i = 0; i < n; i++) {
            int flen = device_int_prop(list[i], CFSTR(kIOHIDMaxFeatureReportSizeKey));
            if (flen >= RZR_REPORT_LEN &&
                IOHIDDeviceOpen(list[i], kIOHIDOptionsTypeNone) == kIOReturnSuccess) {
                g_pad = (IOHIDDeviceRef)CFRetain(list[i]);
                break;
            }
        }
        free(list);
    }
    CFRelease(devs);
    /* Without a scheduled run loop the manager never refreshes its device
     * set, so a hot-plugged pad would stay invisible. Recreate it on the
     * next attempt instead. */
    if (!g_pad) {
        IOHIDManagerClose(g_hid_mgr, kIOHIDOptionsTypeNone);
        CFRelease(g_hid_mgr);
        g_hid_mgr = NULL;
    }
    return g_pad != NULL;
}

static IOReturn pad_send(const uint8_t *report) {
    if (!g_pad)
        return kIOReturnNotOpen;
    IOReturn r = IOHIDDeviceSetReport(g_pad, kIOHIDReportTypeFeature, 0,
                                      report, RZR_REPORT_LEN);
    if (r != kIOReturnSuccess)
        pad_close(); /* device likely unplugged; reopen on next cycle */
    return r;
}

/* ── Generic Razer Chroma protocol ──
 * Report body: status, transaction_id, remaining[2], protocol, data_size,
 * class, command, args[80], crc (XOR of bytes 2..87), reserved. */

#define RZR_STATUS_OK 0x02

static void razer_build(uint8_t *buf, uint8_t txn, uint8_t cls, uint8_t cmd,
                        const uint8_t *args, uint8_t argc) {
    memset(buf, 0, RZR_REPORT_LEN);
    buf[1] = txn;
    buf[5] = argc;
    buf[6] = cls;
    buf[7] = cmd;
    if (argc)
        memcpy(buf + 8, args, argc);
    uint8_t crc = 0;
    for (int i = 2; i < 88; i++)
        crc ^= buf[i];
    buf[88] = crc;
}

/* Send a command and return the device's response status byte
 * (0x02 = success), or -1 on transport error. */
static int razer_cmd(uint8_t txn, uint8_t cls, uint8_t cmd,
                     const uint8_t *args, uint8_t argc) {
    uint8_t buf[RZR_REPORT_LEN];
    razer_build(buf, txn, cls, cmd, args, argc);
    if (pad_send(buf) != kIOReturnSuccess)
        return -1;
    usleep(80000);
    uint8_t resp[RZR_REPORT_LEN] = {0};
    CFIndex len = RZR_REPORT_LEN;
    if (!g_pad ||
        IOHIDDeviceGetReport(g_pad, kIOHIDReportTypeFeature, 0, resp, &len) !=
            kIOReturnSuccess)
        return -1;
    return resp[0];
}

/* Fan commands — class 0x0D, transaction 0x02 (as captured from Synapse). */
#define FAN_TXN 0x02

static IOReturn pad_set_rpm(int rpm) {
    if (rpm < RZR_MIN_RPM)
        rpm = RZR_MIN_RPM;
    if (rpm > RZR_MAX_RPM)
        rpm = RZR_MAX_RPM;
    int raw = (rpm + 25) / 50;
    uint8_t args[3] = {0x01, 0x05, 0}; /* mode, fan id, then rpm/50 LE16 */
    uint8_t buf[RZR_REPORT_LEN];
    razer_build(buf, FAN_TXN, 0x0D, 0x01, args, 3);
    buf[10] = (uint8_t)(raw & 0xFF); /* args[2..3] = rpm/50, little-endian */
    buf[11] = (uint8_t)(raw >> 8);
    uint8_t crc = 0;
    for (int i = 2; i < 88; i++)
        crc ^= buf[i];
    buf[88] = crc;
    return pad_send(buf);
}

static IOReturn pad_release(void) {
    uint8_t args[2] = {0x00, 0x06};
    uint8_t buf[RZR_REPORT_LEN];
    razer_build(buf, FAN_TXN, 0x0D, 0x10, args, 2);
    /* keep original 3-byte data size seen on the wire */
    buf[5] = 0x03;
    uint8_t crc = 0;
    for (int i = 2; i < 88; i++)
        crc ^= buf[i];
    buf[88] = crc;
    return pad_send(buf);
}

/* Returns the last commanded RPM (the pad has no tachometer), or -1. */
static int pad_read_rpm(void) {
    if (!g_pad)
        return -1;
    uint8_t buf[RZR_REPORT_LEN] = {0};
    CFIndex len = RZR_REPORT_LEN;
    IOReturn r = IOHIDDeviceGetReport(g_pad, kIOHIDReportTypeFeature, 0, buf, &len);
    if (r != kIOReturnSuccess)
        return -1;
    return (buf[10] | (buf[11] << 8)) * 50;
}

/* ── Lighting — Chroma extended matrix (class 0x0F) ──
 * The exact transaction id / LED id pair for this pad is probed at runtime
 * and remembered. */

static uint8_t g_led_txn = FAN_TXN;
static uint8_t g_led_id = 0x00;
static int g_led_probed = 0;

static int led_try(uint8_t txn, uint8_t led, uint8_t cmd, uint8_t *args,
                   uint8_t argc) {
    args[0] = 0x01; /* VARSTORE */
    args[1] = led;
    return razer_cmd(txn, 0x0F, cmd, args, argc);
}

/* Send a lighting command, probing (txn, led) combos on first use. */
static int led_send(uint8_t cmd, uint8_t *args, uint8_t argc) {
    if (g_led_probed)
        return led_try(g_led_txn, g_led_id, cmd, args, argc);

    const uint8_t txns[] = {0x02, 0x1F, 0x3F, 0x9F, 0xFF};
    const uint8_t leds[] = {0x00, 0x05, 0x01};
    for (size_t t = 0; t < sizeof(txns); t++) {
        for (size_t l = 0; l < sizeof(leds); l++) {
            int st = led_try(txns[t], leds[l], cmd, args, argc);
            if (st == RZR_STATUS_OK) {
                g_led_txn = txns[t];
                g_led_id = leds[l];
                g_led_probed = 1;
                fprintf(stderr, "led: using transaction 0x%02X, led id 0x%02X\n",
                        g_led_txn, g_led_id);
                return st;
            }
            if (st < 0)
                return st; /* transport error — stop probing */
        }
    }
    return -2; /* no combo accepted */
}

static int led_off(void) {
    uint8_t args[6] = {0}; /* effect 0x00 = none */
    return led_send(0x02, args, sizeof(args));
}

static int led_static(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t args[9] = {0, 0, 0x01, 0x00, 0x00, 0x01, r, g, b};
    return led_send(0x02, args, sizeof(args));
}

static int led_spectrum(void) {
    uint8_t args[6] = {0, 0, 0x03, 0, 0, 0};
    return led_send(0x02, args, sizeof(args));
}

static int led_wave(int direction) {
    uint8_t args[6] = {0, 0, 0x04, (uint8_t)direction, 0x28, 0};
    return led_send(0x02, args, sizeof(args));
}

static int led_breath(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t args[9] = {0, 0, 0x02, 0x00, 0x00, 0x01, r, g, b};
    return led_send(0x02, args, sizeof(args));
}

static int led_brightness(int percent) {
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    uint8_t args[3] = {0, 0, (uint8_t)(percent * 255 / 100)};
    return led_send(0x04, args, sizeof(args));
}

/* Map temperature onto a green→amber→red gradient for reactive lighting. */
static void temp_to_rgb(double t, uint8_t *r, uint8_t *g, uint8_t *b) {
    double x = (t - 55.0) / (95.0 - 55.0); /* 55°C green .. 95°C red */
    if (x < 0)
        x = 0;
    if (x > 1)
        x = 1;
    double h = 120.0 * (1.0 - x) / 60.0; /* hue in sextants: 2=green, 0=red */
    double f = h - (int)h;
    double R = 0, G = 0, B = 0;
    switch ((int)h) {
    case 0: R = 1; G = f; break;
    case 1: R = 1 - f; G = 1; break;
    default: G = 1; break;
    }
    *r = (uint8_t)(R * 255);
    *g = (uint8_t)(G * 255);
    *b = (uint8_t)(B * 255);
}

static int g_reactive_bucket = -1000;

/* Parse "static:RRGGBB" / "spectrum" / ... and apply. Returns status. */
static int led_apply_spec(const char *spec, const char *color_hex) {
    unsigned r = 0, g = 255, b = 0;
    const char *colon = strchr(spec, ':');
    if (colon)
        color_hex = colon + 1;
    if (color_hex && strlen(color_hex) == 6)
        sscanf(color_hex, "%02x%02x%02x", &r, &g, &b);

    if (!strncmp(spec, "off", 3))
        return led_off();
    if (!strncmp(spec, "static", 6))
        return led_static((uint8_t)r, (uint8_t)g, (uint8_t)b);
    if (!strncmp(spec, "spectrum", 8))
        return led_spectrum();
    if (!strncmp(spec, "wave2", 5))
        return led_wave(2);
    if (!strncmp(spec, "wave", 4))
        return led_wave(1);
    if (!strncmp(spec, "breath", 6))
        return led_breath((uint8_t)r, (uint8_t)g, (uint8_t)b);
    return -3; /* unknown spec */
}

/* ───────────────────────── System power (sleep/wake) ────────────────────── */

static volatile sig_atomic_t g_running = 1;
static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static io_connect_t g_pm_root_port = 0;
static IONotificationPortRef g_pm_notify_port = NULL;
static io_object_t g_pm_notifier = 0;
static volatile sig_atomic_t g_just_woke = 0;

static void power_callback(void *refcon, io_service_t service,
                           natural_t msg, void *arg) {
    (void)refcon;
    (void)service;
    switch (msg) {
    case kIOMessageCanSystemSleep:
        IOAllowPowerChange(g_pm_root_port, (long)arg);
        break;
    case kIOMessageSystemWillSleep:
        /* stop the pad so it doesn't spin at the last speed all night */
        if (g_pad)
            pad_release();
        IOAllowPowerChange(g_pm_root_port, (long)arg);
        break;
    case kIOMessageSystemHasPoweredOn:
        g_just_woke = 1;
        break;
    }
}

static void power_init(void) {
    g_pm_root_port = IORegisterForSystemPower(NULL, &g_pm_notify_port,
                                              power_callback, &g_pm_notifier);
    if (g_pm_root_port)
        CFRunLoopAddSource(CFRunLoopGetCurrent(),
                           IONotificationPortGetRunLoopSource(g_pm_notify_port),
                           kCFRunLoopDefaultMode);
}

/* Sleep while pumping the run loop so power notifications are delivered. */
static void idle_wait(double secs) {
    CFAbsoluteTime end = CFAbsoluteTimeGetCurrent() + secs;
    while (g_running && !g_just_woke && CFAbsoluteTimeGetCurrent() < end)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, false);
}

/* True when running on battery power. */
static int on_battery(void) {
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (!info)
        return 0;
    CFStringRef type = IOPSGetProvidingPowerSourceType(info);
    int batt = type && CFStringCompare(type, CFSTR(kIOPMBatteryPowerKey), 0) ==
                           kCFCompareEqualTo;
    CFRelease(info);
    return batt;
}

/* ─────────────────────────── Fan curve & config ─────────────────────────── */

typedef struct {
    double temp;
    double percent;
} CurvePoint;

#define MAX_POINTS 16
static CurvePoint g_curve[MAX_POINTS] = {{40, 0},  {60, 15}, {75, 25},
                                         {85, 40}, {93, 65}, {98, 100}};
static int g_curve_n = 6;
static double g_interval = 3.0;

/* LED settings from config (applied by the daemon) */
static char g_led_effect[16] = "";  /* empty = don't touch lighting */
static char g_led_color[8] = "00ff00";
static int g_led_bright = -1;

/* Fan modifiers from config */
static int g_battery_quiet = 0;   /* % to shave off the curve on battery */
static char g_override[8] = "";   /* "", "max", or "quiet" */

static double curve_percent(double temp) {
    if (temp <= g_curve[0].temp)
        return g_curve[0].percent;
    if (temp >= g_curve[g_curve_n - 1].temp)
        return g_curve[g_curve_n - 1].percent;
    for (int i = 0; i < g_curve_n - 1; i++) {
        double t0 = g_curve[i].temp, t1 = g_curve[i + 1].temp;
        if (temp >= t0 && temp <= t1) {
            double ratio = (temp - t0) / (t1 - t0);
            return g_curve[i].percent +
                   ratio * (g_curve[i + 1].percent - g_curve[i].percent);
        }
    }
    return g_curve[g_curve_n - 1].percent;
}

static int percent_to_rpm(double pct) {
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    double rpm = RZR_MIN_RPM + pct / 100.0 * (RZR_MAX_RPM - RZR_MIN_RPM);
    return (int)((rpm + 25) / 50) * 50;
}

static int parse_curve(const char *spec) {
    CurvePoint pts[MAX_POINTS];
    int n = 0;
    char *dup = strdup(spec);
    for (char *tok = strtok(dup, ","); tok && n < MAX_POINTS;
         tok = strtok(NULL, ",")) {
        double t, p;
        if (sscanf(tok, "%lf:%lf", &t, &p) != 2) {
            free(dup);
            return -1;
        }
        pts[n].temp = t;
        pts[n].percent = p;
        n++;
    }
    free(dup);
    if (n < 2)
        return -1;
    memcpy(g_curve, pts, sizeof(pts[0]) * (size_t)n);
    g_curve_n = n;
    return 0;
}

static char g_config_path[512];
static char g_status_path[] = "/tmp/razerpad-status.json";
static time_t g_config_mtime = 0;

/* Read key=value config. Returns 1 if the file was (re)loaded. */
static int config_poll(void) {
    struct stat st;
    if (stat(g_config_path, &st) != 0)
        return 0;
    if (st.st_mtime == g_config_mtime)
        return 0;
    FILE *f = fopen(g_config_path, "r");
    if (!f)
        return 0;
    g_config_mtime = st.st_mtime;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#')
            continue;
        *eq = 0;
        const char *key = line, *val = eq + 1;
        if (!strcmp(key, "interval")) {
            double v = atof(val);
            if (v >= 0.5 && v <= 60)
                g_interval = v;
        } else if (!strcmp(key, "curve")) {
            parse_curve(val);
        } else if (!strcmp(key, "led")) {
            snprintf(g_led_effect, sizeof(g_led_effect), "%s", val);
        } else if (!strcmp(key, "led_color")) {
            if (strlen(val) == 6)
                snprintf(g_led_color, sizeof(g_led_color), "%s", val);
        } else if (!strcmp(key, "brightness")) {
            g_led_bright = atoi(val);
        } else if (!strcmp(key, "battery_quiet")) {
            int v = atoi(val);
            g_battery_quiet = v < 0 ? 0 : v > 90 ? 90 : v;
        } else if (!strcmp(key, "override")) {
            snprintf(g_override, sizeof(g_override), "%s", val);
        } else if (!strcmp(key, "temp_cmd")) {
            snprintf(g_temp_cmd, sizeof(g_temp_cmd), "%s", val);
        }
    }
    fclose(f);
    return 1;
}

static int g_on_battery_now = 0;

static void status_write(double temp, int rpm, int connected) {
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_status_path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "{\"ts\":%ld,\"connected\":%s,\"temp\":%.1f,\"rpm\":%d,",
            (long)time(NULL), connected ? "true" : "false", temp, rpm);
    fprintf(f, "\"power\":\"%s\",\"battery_quiet\":%d,\"override\":\"%s\",",
            g_on_battery_now ? "battery" : "ac", g_battery_quiet, g_override);
    fprintf(f, "\"interval\":%.1f,\"led\":\"%s\",\"led_color\":\"%s\","
               "\"brightness\":%d,\"curve\":\"",
            g_interval, g_led_effect[0] ? g_led_effect : "unmanaged",
            g_led_color, g_led_bright);
    for (int i = 0; i < g_curve_n; i++)
        fprintf(f, "%s%g:%g", i ? "," : "", g_curve[i].temp, g_curve[i].percent);
    fprintf(f, "\"}\n");
    fclose(f);
    rename(tmp, g_status_path);
}

/* ── thermal history: one averaged CSV row (ts,temp,rpm) every 30s ── */

static char g_history_path[512];
static double g_hist_sum_t = 0;
static long g_hist_sum_r = 0;
static int g_hist_n = 0;
static time_t g_hist_flush = 0;

static void history_tick(double temp, int rpm) {
    if (!g_history_path[0] || temp < 0)
        return;
    g_hist_sum_t += temp;
    g_hist_sum_r += rpm;
    g_hist_n++;
    time_t now = time(NULL);
    if (!g_hist_flush)
        g_hist_flush = now;
    if (now - g_hist_flush < 30)
        return;
    FILE *f = fopen(g_history_path, "a");
    if (f) {
        fprintf(f, "%ld,%.1f,%d\n", (long)now, g_hist_sum_t / g_hist_n,
                (int)(g_hist_sum_r / g_hist_n));
        fclose(f);
    }
    g_hist_sum_t = 0;
    g_hist_sum_r = 0;
    g_hist_n = 0;
    g_hist_flush = now;
}

/* Keep the history file bounded: when it exceeds ~2MB, keep the last 1MB. */
static void history_trim(void) {
    struct stat st;
    if (stat(g_history_path, &st) != 0 || st.st_size < 2 * 1024 * 1024)
        return;
    FILE *f = fopen(g_history_path, "r");
    if (!f)
        return;
    long keep = 1024 * 1024;
    fseek(f, -keep, SEEK_END);
    char *buf = malloc((size_t)keep + 1);
    size_t got = fread(buf, 1, (size_t)keep, f);
    fclose(f);
    buf[got] = 0;
    char *start = strchr(buf, '\n'); /* drop the partial first line */
    if (start) {
        FILE *out = fopen(g_history_path, "w");
        if (out) {
            fputs(start + 1, out);
            fclose(out);
        }
    }
    free(buf);
}

/* Apply LED settings from config; called on config change / reconnect. */
static void led_apply_config(void) {
    if (!g_pad || !g_led_effect[0])
        return;
    if (!strcmp(g_led_effect, "reactive")) {
        g_reactive_bucket = -1000; /* the daemon loop drives the color */
    } else {
        int st = led_apply_spec(g_led_effect, g_led_color);
        if (st != RZR_STATUS_OK)
            fprintf(stderr, "led: effect '%s' -> status %d\n", g_led_effect, st);
    }
    if (g_led_bright >= 0) {
        int st = led_brightness(g_led_bright);
        if (st != RZR_STATUS_OK)
            fprintf(stderr, "led: brightness -> status %d\n", st);
    }
}

/* ────────────────────────────── Main / loop ─────────────────────────────── */

static void log_line(const char *fmt, ...) {
    char ts[32];
    time_t now = time(NULL);
    strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&now));
    printf("[%s] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

int main(int argc, char **argv) {
    int opt, fixed_rpm = -1, do_off = 0, do_temp = 0, do_read = 0, verbose = 0;
    const char *led_spec = NULL;
    int brightness = -1;

    const char *home = getenv("HOME");
    snprintf(g_config_path, sizeof(g_config_path), "%s/.config/razerpad/config",
             home ? home : "/tmp");

    while ((opt = getopt(argc, argv, "s:otri:C:L:B:vh")) != -1) {
        switch (opt) {
        case 's': fixed_rpm = atoi(optarg); break;
        case 'o': do_off = 1; break;
        case 't': do_temp = 1; break;
        case 'r': do_read = 1; break;
        case 'i': g_interval = atof(optarg); break;
        case 'v': verbose = 1; break;
        case 'L': led_spec = optarg; break;
        case 'B': brightness = atoi(optarg); break;
        case 'C':
            if (parse_curve(optarg) != 0) {
                fprintf(stderr, "Bad curve spec. Example: -C \"40:0,60:40,90:100\"\n");
                return 1;
            }
            break;
        default:
            fprintf(stderr,
                    "Usage: razerpad [-t | -r | -o | -s RPM | -L effect | -B pct]\n"
                    "                [-i secs] [-C temp:pct,...] [-v]\n"
                    "Effects: off, static:RRGGBB, spectrum, wave, wave2, breath:RRGGBB\n");
            return opt == 'h' ? 0 : 1;
        }
    }

    if (do_temp) {
        config_poll(); /* pick up temp_cmd if configured */
        if (!g_temp_cmd[0] && smc_open() != kIOReturnSuccess) {
            fprintf(stderr, "Cannot open SMC.\n");
            return 1;
        }
        double t = cpu_temp();
        if (t < 0) {
            fprintf(stderr, "No CPU temperature sensor found.\n");
            return 1;
        }
        printf("%.1f\n", t);
        return 0;
    }

    if (do_off || do_read || fixed_rpm > 0 || led_spec || brightness >= 0) {
        if (!pad_open()) {
            fprintf(stderr, "Razer Cooling Pad not found (VID 0x1532, PID 0x0F43).\n");
            return 1;
        }
        int rc = 0;
        if (do_read) {
            int rpm = pad_read_rpm();
            if (rpm < 0) {
                fprintf(stderr, "Failed to read from pad.\n");
                rc = 1;
            } else
                printf("%d\n", rpm);
        }
        if (fixed_rpm > 0 && !rc) {
            if (pad_set_rpm(fixed_rpm) != kIOReturnSuccess) {
                fprintf(stderr, "Failed to set RPM.\n");
                rc = 1;
            } else
                printf("Fan set to %d RPM.\n", ((fixed_rpm + 25) / 50) * 50);
        }
        if (led_spec && !rc && !strncmp(led_spec, "reactive", 8)) {
            fprintf(stderr, "reactive is a daemon mode: set led=reactive in "
                            "~/.config/razerpad/config\n");
            rc = 1;
        }
        if (led_spec && !rc) {
            int st = led_apply_spec(led_spec, NULL);
            if (st == RZR_STATUS_OK)
                printf("Lighting: %s applied (status OK).\n", led_spec);
            else {
                fprintf(stderr, "Lighting command not accepted (status %d).\n", st);
                rc = 1;
            }
        }
        if (brightness >= 0 && !rc) {
            int st = led_brightness(brightness);
            if (st == RZR_STATUS_OK)
                printf("Brightness set to %d%%.\n", brightness);
            else {
                fprintf(stderr, "Brightness command not accepted (status %d).\n", st);
                rc = 1;
            }
        }
        if (do_off && !rc) {
            if (pad_release() != kIOReturnSuccess) {
                fprintf(stderr, "Failed to send release command.\n");
                rc = 1;
            } else
                printf("Fan control released to pad firmware.\n");
        }
        pad_close();
        return rc;
    }

    /* ── Fan-curve daemon mode ── */
    config_poll(); /* load temp_cmd (and everything else) before probing */
    if (!g_temp_cmd[0] && smc_open() != kIOReturnSuccess) {
        fprintf(stderr,
                "Cannot open SMC — is this an Intel Mac? On other Macs, set\n"
                "temp_cmd=<command printing degrees C> in ~/.config/razerpad/config\n");
        return 1;
    }
    if (cpu_temp() < 0) {
        fprintf(stderr, "No readable CPU temperature sensor.\n");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    power_init();

    snprintf(g_history_path, sizeof(g_history_path),
             "%s/.config/razerpad/history.csv", home ? home : "/tmp");
    history_trim();

    config_poll();
    log_line("razerpad starting: interval=%.1fs, config=%s", g_interval,
             g_config_path);
    for (int i = 0; i < g_curve_n; i++)
        log_line("  %5.1f degC -> %5.1f%%", g_curve[i].temp, g_curve[i].percent);

    int last_rpm = -1;
    int was_connected = 0;
    int cycles_since_send = 0;

    while (g_running) {
        if (g_just_woke) {
            g_just_woke = 0;
            log_line("Woke from sleep — re-taking fan control");
            pad_close(); /* device refs may be stale after sleep */
            last_rpm = -1;
        }

        if (config_poll()) {
            log_line("config reloaded");
            last_rpm = -1; /* re-send fan speed */
            led_apply_config();
        }

        if (!g_pad) {
            if (pad_open()) {
                log_line("Cooling pad connected — taking over fan control");
                was_connected = 1;
                last_rpm = -1;
                led_apply_config();
            } else {
                if (was_connected) {
                    log_line("Cooling pad disconnected — waiting for it to return");
                    was_connected = 0;
                }
                double t = cpu_temp();
                status_write(t, 0, 0);
                history_tick(t, 0);
                idle_wait(5.0);
                continue;
            }
        }

        double temp = cpu_temp();
        if (temp >= 0) {
            double pct = curve_percent(temp);
            g_on_battery_now = on_battery();
            if (g_on_battery_now && g_battery_quiet > 0)
                pct = pct * (100 - g_battery_quiet) / 100.0;
            if (!strcmp(g_override, "max"))
                pct = 100;
            else if (!strcmp(g_override, "quiet") && pct > 25)
                pct = 25;
            int rpm = percent_to_rpm(pct);
            /* hysteresis: ignore single-step (50 RPM) wobble from sensor noise */
            if (last_rpm >= 0 && abs(rpm - last_rpm) <= 50)
                rpm = last_rpm;
            /* heartbeat: re-send unchanged RPM every 10 cycles so a silent
             * disconnect is noticed even when the temperature is steady */
            if (rpm != last_rpm || ++cycles_since_send >= 10) {
                int changed = rpm != last_rpm;
                if (pad_set_rpm(rpm) == kIOReturnSuccess) {
                    if (changed && !verbose)
                        log_line("%.1f degC -> %d RPM", temp, rpm);
                    last_rpm = rpm;
                    cycles_since_send = 0;
                } else {
                    log_line("Send failed — pad disconnected?");
                    last_rpm = -1;
                    continue;
                }
            }
            if (verbose)
                log_line("%.1f degC -> %.0f%% -> %d RPM", temp, pct, rpm);
            status_write(temp, last_rpm > 0 ? last_rpm : 0, 1);
            history_tick(temp, last_rpm > 0 ? last_rpm : 0);

            /* reactive lighting: color tracks temperature in 2°C steps */
            if (g_pad && !strcmp(g_led_effect, "reactive")) {
                int bucket = (int)(temp / 2.0);
                if (bucket != g_reactive_bucket) {
                    uint8_t lr, lg, lb;
                    temp_to_rgb(temp, &lr, &lg, &lb);
                    if (led_static(lr, lg, lb) == RZR_STATUS_OK)
                        g_reactive_bucket = bucket;
                }
            }
        }

        idle_wait(g_interval);
    }

    if (g_pad) {
        pad_release();
        log_line("Fan control released.");
    }
    status_write(-1, 0, 0);
    pad_close();
    return 0;
}
