/* modem_probe.h -- the /proc, /sys, /etc and /dev readers behind the Modem
 * app's DATA page, plus the two string helpers its other pages share.
 *
 * Split out because main.py says so about its own row builders -- "kept
 * drawing-free so they can be bench-tested" -- and because these are the only
 * parts of the app a host test can drive against staged files. Every path
 * goes through nd_path_resolve(), as the whole modem subsystem does
 * (OPEN-QUESTIONS.md M-9): in production ND_ROOT is empty and it is a plain
 * copy, and in a test it is what lets a fake /sys/class/net exist.
 */

#ifndef ND_MODEM_PROBE_H_INCLUDED
#define ND_MODEM_PROBE_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* main.py's module constants. */
#define ND_MODEMAPP_BOOT_STATUS_FILE "/tmp/modem.status"
#define ND_MODEMAPP_DEFAULTS_FILE    "/etc/default/modem"
#define ND_MODEMAPP_RESOLV_FILE      "/etc/resolv.conf"
#define ND_MODEMAPP_IF_INET6         "/proc/net/if_inet6"
#define ND_MODEMAPP_NET_DIR          "/sys/class/net"
#define ND_MODEMAPP_DEV_DIR          "/dev"
#define ND_MODEMAPP_DEFAULT_APN      "fast.t-mobile.com"
#define ND_MODEMAPP_QMI_DRIVER       "qmi_wwan"

/* An interface name. IFNAMSIZ is 16; 32 costs nothing and cannot truncate. */
#define ND_MODEMAPP_IFNAME_MAX 32

/* One AT reply line as this app handles it. ND_MODEM_LINE_MAX (512) is what
 * the engine collects; this is the working copy _first_content() strips. */
#define ND_MODEM_LINE_TEXT_MAX 512

/* _shorten(text, limit=24)'s default limit, and the size a shortened result
 * needs: 11 + 2 + 11 + NUL. */
#define ND_MODEMAPP_SHORTEN_LIMIT 24

/* _shorten(text, limit): unchanged when it fits, else the first (limit-2)//2
 * characters, "..", and the last (limit-2)//2. Note the two halves are
 * (limit-2)//2 EACH, so an odd limit produces a result one character short of
 * it -- 24 gives 11 + 2 + 11 = 24, and 25 would also give 24. Returns out. */
const char *nd_modemapp_shorten(const char *text, size_t limit, char *out, size_t out_sz);

/* _first_content(lines, prefix): the first non-blank reply line, stripped,
 * with a leading "+PREFIX:" removed when it is there. The comparison is
 * case-insensitive and ASCII-only, as Python's str.upper() is not -- no AT
 * reply is anything else. false when every line was blank. */
bool nd_modemapp_first_content(const char *const *lines, size_t n_lines, const char *prefix,
                               char *out, size_t out_sz);

/* Python's str.strip() over ASCII whitespace, in place. */
void nd_modemapp_strip(char *s);

/* _read_file(path): the file's contents with str.strip() applied. false when
 * it cannot be read, which is Python's None. */
bool nd_modemapp_read_file(const char *path, char *out, size_t out_sz);

/* Sorted ttyUSB* entries of /dev, joined with ",". Empty when there are none,
 * which is what makes the app print "no ttyUSB nodes!". */
size_t nd_modemapp_ttyusb_list(char *out, size_t out_sz);

/* _wwan_interface(): the qmi_wwan-bound interface if there is one, else the
 * first name starting "ww", then "rmnet", then "usb". false when /sys/class/net
 * holds none of them. */
bool nd_modemapp_wwan_interface(char *out, size_t out_sz);

/* _iface_up(name): bit 0 of /sys/class/net/<name>/flags, read as hex. */
bool nd_modemapp_iface_up(const char *name);

/* _global_ipv6(ifname): the first global-scope address on that interface from
 * /proc/net/if_inet6, formatted by inet_ntop. */
bool nd_modemapp_global_ipv6(const char *ifname, char *out, size_t out_sz);

/* _configured_apn(): MODEM_APN from /etc/default/modem, or the default. */
void nd_modemapp_configured_apn(char *out, size_t out_sz);

/* _dns_row(): the first IPv6 nameserver in /etc/resolv.conf, else the first
 * nameserver of any kind, else "--". */
void nd_modemapp_dns_row(char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* ND_MODEM_PROBE_H_INCLUDED */
