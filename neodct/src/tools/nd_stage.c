/* nd-stage -- stage a .ndsw for the boot-time applier, from a shell.
 *
 *     nd-stage [--json] [--key PATH] PKG.ndsw
 *
 * Does exactly what the Update app does when the owner picks a package and
 * confirms: open it, verify its release signature, check it is for this phone,
 * and write /NeoDCT/User/.ndsys/pending.prop. The next boot's applier does the
 * rest. Nothing is copied -- the applier finds the package on the card by the
 * basename in the record.
 *
 * ============ WHY THIS IS NOT A SECOND UPDATE PATH ============
 *
 * It is the SAME path. Every function here comes from apps/Update: the package
 * reader, the signature check, the compatibility check and stage_package()
 * itself. This file parses argv and prints; it decides nothing.
 *
 * That matters more here than anywhere else in the tree. An update system with
 * two implementations of "is this package allowed" has two answers, and the
 * lenient one is the one an attacker uses. So there is one, and this is a
 * second CALLER of it rather than a second copy.
 *
 * The boundary underneath is unchanged either way: ndsys-apply.sh re-checks
 * the release signature at boot with the verifier and key built into the
 * KERNEL image, which nothing writable can reach. Its own header says the
 * quiet part -- "a process that can write /NeoDCT/User stages its own image"
 * is an anticipated case, not a hole -- which is precisely why staging from a
 * shell is a supported thing to do and not a trick.
 *
 * ============ AND WHY IT VERIFIES ANYWAY ============
 *
 * The applier would catch a bad package on its own, so this check is
 * redundant. It is here because the failure it prevents is expensive: a
 * package staged without checking is discovered to be wrong AFTER a reboot,
 * on a phone whose screen is the thing you were trying to reach. Checking now
 * turns that into an error message on the developer's terminal.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/utsname.h>

#include "nd_paths.h"
#include "nd_settings.h"
#include "nd_types.h"

#include "update_app.h"

static int g_json = 0;

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
            default:
                if ((unsigned char)*s >= 0x20u)
                    (void)putchar(*s);
                break;
            }
        }
    }
    (void)putchar('"');
}

static int bail(const char *why, int code)
{
    if (g_json) {
        (void)printf("{\"ok\":false,\"verb\":\"stage\",\"error\":");
        put_json_str(why);
        (void)printf(",\"code\":%d}\n", code);
    } else {
        (void)fprintf(stderr, "nd-stage: %s\n", why);
    }
    return code;
}

int main(int argc, char **argv)
{
    const char *pkg_path = NULL;
    const char *key_path = ND_UPDATE_RELEASE_KEY;
    nd_upd_package *pkg = NULL;
    const nd_upd_manifest *m = NULL;
    char why[320];
    char platform[64];
    const char *kernel = "";
    struct utsname host;
    int64_t image_bytes;
    nd_updsvc_err uerr;
    nd_err rc;
    int i;
    int ret = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0)
            g_json = 1;
        else if (strcmp(argv[i], "--key") == 0 && i + 1 < argc)
            key_path = argv[++i];
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            (void)printf("nd-stage [--json] [--key PATH] PKG.ndsw\n"
                         "  verify a package and record it for the boot-time applier\n");
            return 0;
        } else if (pkg_path == NULL)
            pkg_path = argv[i];
    }
    if (pkg_path == NULL)
        return bail("give a .ndsw package", 5);

    why[0] = '\0';
    if (!nd_upd_service_available())
        return bail("the update service is not in this build", 2);

    uerr = nd_upd_package_open(pkg_path, &pkg, why, sizeof why);
    if (uerr != ND_UPDSVC_OK || pkg == NULL)
        return bail(why[0] != '\0' ? why : "the package could not be opened", 6);

    m = nd_upd_package_manifest(pkg);
    if (m == NULL) {
        nd_upd_package_close(pkg);
        return bail("the package has no manifest", 6);
    }

    /* COMPATIBILITY BEFORE SIGNATURE, matching apps/Update/main.c, whose
     * comment records that three of the Python's tests depend on that
     * ordering. A tool that checked them the other way round would report a
     * different error for the same package than the app does. */
    (void)nd_settings_get_copy(ND_SET_OS_PLATFORM, "unknown", platform, sizeof platform);
    if (uname(&host) == 0)
        kernel = host.release;
    why[0] = '\0';
    if (nd_upd_manifest_check_compatible(m, platform, kernel, why, sizeof why) != ND_UPDSVC_OK) {
        nd_upd_package_close(pkg);
        return bail(why[0] != '\0' ? why : "this update is for another phone", 6);
    }

    /* VERIFY FIRST, THEN ASK. nd_upd_package_signed() is not "does this
     * package contain a signature" -- it is "has the signature been checked
     * and passed", and service.c only sets it inside verify_signature() on
     * success. Testing it beforehand therefore always answers false, which is
     * how this tool first refused a package it had just built and signed
     * itself. The Update app calls verify directly and uses `signed` only for
     * the badge it draws afterwards; this does the same.
     *
     * Unsigned is refused outright here rather than warned about. The app
     * shows the same warning and lets engineering mode click past it, because
     * there is a person present to decide; a tool that stages unattended has
     * nobody to ask, and an image nobody signed is how a phone ends up unable
     * to boot with no way to talk it out of that afterwards. */
    why[0] = '\0';
    if (nd_upd_package_verify_signature(pkg, key_path, why, sizeof why) != ND_UPDSVC_OK) {
        nd_upd_package_close(pkg);
        return bail(why[0] != '\0' ? why : "the release signature did not check out", 6);
    }
    if (!nd_upd_package_signed(pkg)) {
        nd_upd_package_close(pkg);
        return bail("the signature verified but was not recorded; refusing", 6);
    }

    image_bytes = nd_upd_package_image_size(pkg);
    if (image_bytes <= 0) {
        nd_upd_package_close(pkg);
        return bail("the package does not say how big its image is", 6);
    }

    rc = nd_upd_stage_package(m, pkg_path, image_bytes);
    if (rc != ND_OK) {
        nd_upd_package_close(pkg);
        return bail("the staging record could not be written", 6);
    }

    if (g_json) {
        (void)printf("{\"ok\":true,\"verb\":\"stage\",\"package\":");
        put_json_str(pkg_path);
        (void)printf(",\"version\":");
        put_json_str(m->version);
        (void)printf(",\"platform\":");
        put_json_str(platform);
        (void)printf(",\"image_bytes\":%lld", (long long)image_bytes);
        (void)printf(",\"record\":");
        put_json_str(ND_UPDATE_PENDING_RECORD);
        (void)printf(",\"applies_on\":\"next boot\"}\n");
    } else {
        (void)printf("staged %s\n", pkg_path);
        (void)printf("  version   %s\n", m->version);
        (void)printf("  image     %lld bytes\n", (long long)image_bytes);
        (void)printf("  record    %s\n", ND_UPDATE_PENDING_RECORD);
        (void)printf("It installs on the next boot.\n");
    }

    nd_upd_package_close(pkg);
    return ret;
}
