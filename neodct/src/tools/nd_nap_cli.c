/* nd-nap -- install, inspect, list and remove .nap packages from a shell.
 *
 *     nd-nap inspect PKG.nap     what the phone would see, and would it accept it
 *     nd-nap install PKG.nap     unpack it onto the card
 *     nd-nap list                what is installed
 *     nd-nap remove DIR          take one out
 *     nd-nap arch                what this phone calls itself
 *
 * ============ WHY A CLI AT ALL ============
 *
 * The .nap reader has existed since packages did, and every line of it is in
 * lib/nd_nap.c -- but its only caller was Settings, so the one way to install
 * an app was to copy the file onto a card, walk to the phone, and drive a
 * menu. That is a poor fit for the thing ndlink exists to make possible: an
 * agent, or a developer at a laptop, changing an app and seeing it run.
 *
 * So this is deliberately THIN. It parses argv, calls nd_nap_inspect() and
 * nd_nap_install(), and prints. Arch matching, name sanitising, the
 * replace-and-keep-data behaviour, the id-conflict band, the rollback when a
 * replacement fails half way -- all of that stays in nd_nap.c, where Settings
 * and this tool both get it. A second implementation of any of it would be a
 * second set of rules for what a package may do, which is the one thing a
 * package format must not have.
 *
 * Output is JSON when --json is given, because ndlink parses it.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nd_nap.h"
#include "nd_paths.h"
#include "nd_types.h"

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

static void fail_out(const char *verb, const char *why, int code)
{
    if (g_json) {
        (void)printf("{\"ok\":false,\"verb\":");
        put_json_str(verb);
        (void)printf(",\"error\":");
        put_json_str(why);
        (void)printf(",\"code\":%d}\n", code);
    } else {
        (void)fprintf(stderr, "nd-nap: %s\n", why);
    }
    exit(code);
}

static void print_info(const char *verb, const nd_nap_info *info, const char *arch)
{
    if (g_json) {
        (void)printf("{\"ok\":true,\"verb\":");
        put_json_str(verb);
        (void)printf(",\"name\":");
        put_json_str(info->name);
        (void)printf(",\"dir\":");
        put_json_str(info->dir);
        (void)printf(",\"version\":");
        put_json_str(info->version);
        (void)printf(",\"author\":");
        put_json_str(info->author);
        (void)printf(",\"id\":%ld", (long)info->id);
        (void)printf(",\"files\":%zu,\"bytes\":%llu", info->n_files,
                     (unsigned long long)info->bytes);
        (void)printf(",\"phone_arch\":");
        put_json_str(arch);
        (void)printf(",\"arch_ok\":%s",
                     nd_nap_info_has_arch(info, arch) ? "true" : "false");
        (void)printf(",\"needs_restart\":%s", info->needs_restart_to_appear ? "true" : "false");
        (void)printf("}\n");
    } else {
        (void)printf("name        %s\n", info->name);
        (void)printf("directory   %s\n", info->dir);
        (void)printf("version     %s\n", info->version[0] != '\0' ? info->version : "(none)");
        (void)printf("author      %s\n", info->author[0] != '\0' ? info->author : "(none)");
        (void)printf("menu id     %ld\n", (long)info->id);
        (void)printf("files       %zu (%llu bytes)\n", info->n_files,
                     (unsigned long long)info->bytes);
        (void)printf("this phone  %s\n", arch);
        (void)printf("runs here   %s\n", nd_nap_info_has_arch(info, arch) ? "yes" : "NO");
        if (info->needs_restart_to_appear)
            (void)printf("note        installed, but the menu shows it after a restart\n");
    }
}

/* What is on the card. Reads the directory the menu scans rather than any
 * record of its own -- an app that was deleted by hand is gone, and a list
 * built from a manifest of installs would still be claiming it. */
static int cmd_list(const char *apps_dir)
{
    char resolved[ND_PATH_MAX];
    DIR *d;
    struct dirent *e;
    int n = 0;

    if (nd_path_resolve(resolved, sizeof resolved, apps_dir) != ND_OK)
        fail_out("list", "cannot resolve the apps directory", 1);
    d = opendir(resolved);
    if (d == NULL) {
        /* No card, or no apps yet. Both are "nothing installed", not errors. */
        if (g_json)
            (void)printf("{\"ok\":true,\"verb\":\"list\",\"apps\":[]}\n");
        else
            (void)printf("no installed apps (%s)\n", resolved);
        return 0;
    }
    if (g_json)
        (void)printf("{\"ok\":true,\"verb\":\"list\",\"apps\":[");
    while ((e = readdir(d)) != NULL) {
        char manifest[ND_PATH_MAX];
        struct stat st;

        if (e->d_name[0] == '.')
            continue;
        if (snprintf(manifest, sizeof manifest, "%s/%s/manifest.json", resolved, e->d_name) < 0)
            continue;
        /* A directory with no manifest is a dead install, not an app -- the
         * same rule nd_nap_is_installed() uses. */
        if (stat(manifest, &st) != 0)
            continue;
        if (g_json) {
            if (n > 0)
                (void)putchar(',');
            put_json_str(e->d_name);
        } else {
            (void)printf("%s\n", e->d_name);
        }
        n++;
    }
    (void)closedir(d);
    if (g_json)
        (void)printf("]}\n");
    else if (n == 0)
        (void)printf("no installed apps\n");
    return 0;
}

static int cmd_remove(const char *apps_dir, const char *dir)
{
    char resolved[ND_PATH_MAX];
    char target[ND_PATH_MAX];
    char cmd[ND_PATH_MAX + 32];
    char safe[ND_NAP_DIR_MAX];

    /* Sanitise through the same function that decided the name at install
     * time, so "remove" cannot be handed a path. */
    if (!nd_nap_dir_from_name(dir, safe, sizeof safe))
        fail_out("remove", "that is not an app directory name", 5);
    if (nd_path_resolve(resolved, sizeof resolved, apps_dir) != ND_OK)
        fail_out("remove", "cannot resolve the apps directory", 1);
    if (snprintf(target, sizeof target, "%s/%s", resolved, safe) < 0)
        fail_out("remove", "path too long", 1);
    if (!nd_nap_is_installed(apps_dir, safe))
        fail_out("remove", "no such app is installed", 5);

    /* rm -rf through the shell rather than an nftw() here: this is a developer
     * tool removing a directory it just validated, and a hand-rolled recursive
     * delete is a much worse thing to get wrong than a fork. */
    if (snprintf(cmd, sizeof cmd, "rm -rf '%s'", target) < 0)
        fail_out("remove", "path too long", 1);
    if (system(cmd) != 0)
        fail_out("remove", "could not remove it", 1);

    if (g_json) {
        (void)printf("{\"ok\":true,\"verb\":\"remove\",\"dir\":");
        put_json_str(safe);
        (void)printf("}\n");
    } else {
        (void)printf("removed %s\n", safe);
    }
    return 0;
}

static void usage(FILE *to)
{
    (void)fprintf(to,
                  "nd-nap -- .nap packages from a shell\n"
                  "\n"
                  "  nd-nap [--json] inspect PKG.nap\n"
                  "  nd-nap [--json] install PKG.nap\n"
                  "  nd-nap [--json] list\n"
                  "  nd-nap [--json] remove DIR\n"
                  "  nd-nap [--json] arch\n"
                  "\n"
                  "Apps install into %s.\n",
                  ND_PATH_USER_APPS_DIR);
}

int main(int argc, char **argv)
{
    const char *verb = NULL;
    const char *arg = NULL;
    const char *arch;
    const char *apps_dir = ND_PATH_USER_APPS_DIR;
    nd_nap_info info;
    char why[256];
    nd_err rc;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) {
            g_json = 1;
        } else if (strcmp(argv[i], "--apps-dir") == 0 && i + 1 < argc) {
            apps_dir = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (verb == NULL) {
            verb = argv[i];
        } else if (arg == NULL) {
            arg = argv[i];
        }
    }

    if (verb == NULL) {
        usage(stderr);
        return 5;
    }

    arch = nd_nap_phone_arch();
    memset(&info, 0, sizeof info);
    why[0] = '\0';

    if (strcmp(verb, "arch") == 0) {
        if (g_json) {
            (void)printf("{\"ok\":true,\"verb\":\"arch\",\"arch\":");
            put_json_str(arch);
            (void)printf("}\n");
        } else {
            (void)printf("%s\n", arch);
        }
        return 0;
    }
    if (strcmp(verb, "list") == 0)
        return cmd_list(apps_dir);
    if (strcmp(verb, "remove") == 0) {
        if (arg == NULL)
            fail_out("remove", "remove needs an app directory name", 5);
        return cmd_remove(apps_dir, arg);
    }
    if (strcmp(verb, "inspect") == 0) {
        if (arg == NULL)
            fail_out("inspect", "inspect needs a package", 5);
        rc = nd_nap_inspect(arg, &info, why, sizeof why);
        if (rc != ND_OK)
            fail_out("inspect", why[0] != '\0' ? why : "the package was refused", 6);
        print_info("inspect", &info, arch);
        /* Refusing to run here is not a failure to inspect: the package is
         * fine, it is for another phone. Say so and exit 1 so a script can
         * tell "bad package" from "wrong phone". */
        return nd_nap_info_has_arch(&info, arch) ? 0 : 1;
    }
    if (strcmp(verb, "install") == 0) {
        if (arg == NULL)
            fail_out("install", "install needs a package", 5);
        rc = nd_nap_install(arg, apps_dir, arch, &info, why, sizeof why);
        if (rc != ND_OK)
            fail_out("install", why[0] != '\0' ? why : "the install was refused", 6);
        print_info("install", &info, arch);
        return 0;
    }

    fail_out(verb, "unknown verb", 5);
    return 5;
}
