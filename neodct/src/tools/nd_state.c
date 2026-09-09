/* nd-state -- where the phone is, as JSON.
 *
 * This is `dumpsys`, and it is what an agent reads to know where it is before
 * it presses anything. Everything here is a fact the phone already knows and
 * that was previously only reachable by reading a file over a serial console.
 *
 * One object on stdout, always. A caller parses it; nothing here is formatted
 * for a human, because `ndlink state` does that.
 *
 * ============ WHY THE BATTERY BLOCK IS BIGGER THAN THE REST ============
 *
 * Because "the battery is not working" has three completely different causes
 * and they are indistinguishable from the outside:
 *
 *   sim         there is no bus node at all -- QEMU, or a board without a
 *               gauge. The service invents 3.85 V so the UI has something.
 *   live        the gauge answered.
 *   unreadable  the node is THERE and the gauge could not be read.
 *
 * The third one is the dangerous one, and it has bitten this project already:
 * reporting an unreadable gauge as an absent one with a comfortable 3.85 V is
 * what disabled the low-battery warning and the protective shutdown on a real
 * phone (test_battery.c's header carries that story). So this prints the
 * source verdict, the fault string and the raw registers, and never collapses
 * them into a single number.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#include "nd_battery.h"
#include "nd_input.h"
#include "nd_paths.h"
#include "nd_settings.h"
#include "nd_types.h"

/* Print a JSON string value with the few escapes a path or a fault message
 * can actually contain. */
static void put_json_str(const char *s)
{
    (void)putchar('"');
    if (s != NULL) {
        for (; *s != '\0'; s++) {
            switch (*s) {
            case '"':
                (void)fputs("\\\"", stdout);
                break;
            case '\\':
                (void)fputs("\\\\", stdout);
                break;
            case '\n':
                (void)fputs("\\n", stdout);
                break;
            case '\t':
                (void)fputs("\\t", stdout);
                break;
            default:
                if ((unsigned char)*s >= 0x20u)
                    (void)putchar(*s);
                break;
            }
        }
    }
    (void)putchar('"');
}

/* First line of a file, trimmed. "" when it is not there -- absence is a
 * normal answer for most of these and is reported as such. */
static void read_line(const char *path, char *out, size_t out_sz)
{
    char resolved[ND_PATH_MAX];
    FILE *f;
    size_t n;

    out[0] = '\0';
    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return;
    f = fopen(resolved, "re");
    if (f == NULL)
        return;
    if (fgets(out, (int)out_sz, f) != NULL) {
        n = strlen(out);
        while (n > 0u && (out[n - 1u] == '\n' || out[n - 1u] == '\r' || out[n - 1u] == ' '))
            out[--n] = '\0';
    }
    (void)fclose(f);
}

/* A "key=value" out of a .prop file. */
static void read_prop(const char *path, const char *key, char *out, size_t out_sz)
{
    char resolved[ND_PATH_MAX];
    char line[512];
    size_t klen = strlen(key);
    FILE *f;

    out[0] = '\0';
    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return;
    f = fopen(resolved, "re");
    if (f == NULL)
        return;
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
            size_t n;

            (void)nd_strlcpy(out, line + klen + 1u, out_sz);
            n = strlen(out);
            while (n > 0u && (out[n - 1u] == '\n' || out[n - 1u] == '\r'))
                out[--n] = '\0';
            break;
        }
    }
    (void)fclose(f);
}

static const char *source_name(nd_battery_source s)
{
    switch (s) {
    case ND_BATT_SRC_LIVE:
        return "live";
    case ND_BATT_SRC_UNREADABLE:
        return "unreadable";
    case ND_BATT_SRC_SIM:
    default:
        return "sim";
    }
}

int main(int argc, char **argv)
{
    struct sysinfo si;
    char version[128];
    char platform[64];
    char card[128];
    char bl_power[32];
    char bl_bright[32];
    nd_battery *bat = NULL;
    nd_battery_snap snap;
    bool have_snap = false;
    double vcell = 0.0;
    bool have_vcell = false;
    long uptime = 0;
    long freeram_kb = 0;
    long totalram_kb = 0;

    ND_UNUSED(argc);
    ND_UNUSED(argv);

    memset(&snap, 0, sizeof snap);

    if (sysinfo(&si) == 0) {
        uptime = (long)si.uptime;
        /* mem_unit is 1 on every kernel this runs on, but multiplying is free
         * and a 64 MB phone reporting nonsense free RAM would be the kind of
         * thing nobody checks. */
        freeram_kb = (long)((unsigned long long)si.freeram * si.mem_unit / 1024ull);
        totalram_kb = (long)((unsigned long long)si.totalram * si.mem_unit / 1024ull);
    }

    read_prop(ND_PATH_VERSION_PROP, "system.os.versionnumber", version, sizeof version);
    read_prop(ND_PATH_VERSION_PROP, "system.os.platform", platform, sizeof platform);
    read_line(ND_PATH_SDCARD_STATE, card, sizeof card);
    read_line("/sys/class/backlight/backlight/bl_power", bl_power, sizeof bl_power);
    read_line("/sys/class/backlight/backlight/brightness", bl_bright, sizeof bl_bright);

    /* -1/-1 means "read the bus and address from settings", which is the only
     * way to be sure this reports the gauge the CORE would have opened rather
     * than one this tool guessed at. */
    if (nd_battery_open(&bat, -1, -1) == ND_OK && bat != NULL) {
        (void)nd_battery_poll(bat, true);
        have_vcell = nd_battery_vcell(bat, &vcell);
        have_snap = nd_battery_debug_snapshot(bat, &snap);
    }

    (void)printf("{");
    (void)printf("\"version\":");
    put_json_str(version);
    (void)printf(",\"platform\":");
    put_json_str(platform);
    (void)printf(",\"uptime_s\":%ld", uptime);
    (void)printf(",\"mem_free_kb\":%ld,\"mem_total_kb\":%ld", freeram_kb, totalram_kb);

    (void)printf(",\"card\":");
    put_json_str(card);
    (void)printf(",\"backlight\":{\"bl_power\":");
    put_json_str(bl_power);
    (void)printf(",\"brightness\":");
    put_json_str(bl_bright);
    (void)printf("}");

    (void)printf(",\"devkey_socket\":%s",
                 access(ND_PATH_DEVKEY_SOCK, F_OK) == 0 ? "true" : "false");
    (void)printf(",\"devenv_marker\":%s",
                 access(ND_PATH_DEVENV_MARKER, F_OK) == 0 ? "true" : "false");

    (void)printf(",\"battery\":{");
    if (bat != NULL) {
        (void)printf("\"source\":");
        put_json_str(source_name(nd_battery_source_of(bat)));
        (void)printf(",\"level\":%d", (int)nd_battery_level(bat));
        (void)printf(",\"has_hardware\":%s", nd_battery_has_hardware(bat) ? "true" : "false");
        if (have_vcell)
            (void)printf(",\"vcell\":%.4f", vcell);
        else
            (void)printf(",\"vcell\":null");
        (void)printf(",\"fault\":");
        put_json_str(nd_battery_fault(bat));
        if (have_snap) {
            (void)printf(",\"bus\":%d,\"addr\":%d", snap.bus, snap.addr);
            (void)printf(",\"raw_vcell\":%u,\"raw_soc\":%u,\"raw_config\":%u",
                         (unsigned)snap.raw_vcell, (unsigned)snap.raw_soc,
                         (unsigned)snap.raw_config);
            (void)printf(",\"ic_version\":%d", snap.ic_version);
        }
    } else {
        (void)printf("\"source\":\"unknown\",\"fault\":\"nd_battery_open failed\"");
    }
    (void)printf("}");

    (void)printf("}\n");

    nd_battery_close(bat);
    return 0;
}
