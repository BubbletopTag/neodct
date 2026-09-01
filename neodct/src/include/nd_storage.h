/* nd_storage.h -- is there an SD card, is it one of ours, and where do the
 * update files live.
 *
 * A shell helper (neodct-sdcard) mounts the card and writes what it found to
 * /run/neodct/sdcard.prop. Nothing here mounts anything; it reads that file
 * and decides which of four states the card is in.
 *
 * The state machine, from Storage.card(), in this order:
 *
 *   reported "unmountable" or "unformatted"  -> ND_CARD_UNFORMATTED, keeping
 *                                               device/fstype/label
 *   reported anything else that is not
 *   "mounted", "share" or "ready"            -> ND_CARD_ABSENT, and
 *                                               device/fstype/label are BLANKED
 *   otherwise, all five folders present      -> ND_CARD_READY
 *   otherwise                                -> ND_CARD_NEEDS_SETUP
 *
 * `removable` is computed as (fstype != "virtiofs") BEFORE any of those
 * returns, so an absent card still reports removable == true. Port that
 * ordering; test_storage.py asserts a virtiofs share is not removable.
 */

#ifndef ND_STORAGE_H_INCLUDED
#define ND_STORAGE_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ND_SD_MOUNT_POINT      "/NeoDCT/User/sdcard"
#define ND_SD_STATE_FILE       "/run/neodct/sdcard.prop"

/* The arrival partition, when the card has one.
 *
 * SECURITY-PLAN.md section 1: a NeoDCT card is two FAT32 partitions, because
 * a FAT filesystem has no ownership of its own and mount options apply to
 * the whole of one. p1 belongs to ndusr and holds what the OWNER copied on;
 * p2 belongs to ndusr_ut, is mounted noexec, and holds what ARRIVED --
 * downloads and MMS attachments.
 *
 * It is mounted INSIDE p1, which works because p1's dmask makes its
 * directories 0751: ndusr_ut traverses "sdcard" to reach "untrusted" without
 * being able to list the owner's music.
 *
 * A foreign card has no such partition and nd_storage_untrusted_dir() then
 * says so -- which is the right answer for "where do I put this download",
 * not a degraded one. */
#define ND_SD_UNTRUSTED_NAME  "untrusted"
#define ND_SD_UNTRUSTED_MOUNT ND_SD_MOUNT_POINT "/" ND_SD_UNTRUSTED_NAME
#define ND_SD_UPDATE_SUFFIX    ".ndsw"
#define ND_SD_PREFERRED_UPDATE "UPDATE.ndsw"

/* ORDER MATTERS -- setup_folders() creates them in this order and stops at
 * the first failure, leaving the earlier ones created. */
#define ND_SD_FOLDER_COUNT 5
extern const char *const ND_SD_FOLDERS[ND_SD_FOLDER_COUNT];
/* { "wallpapers", "tones", "backup_db", "music", "update" } */

typedef enum {
    ND_CARD_ABSENT = 0,
    ND_CARD_READY,
    ND_CARD_NEEDS_SETUP,
    ND_CARD_UNFORMATTED,
    /* A NeoDCT card from before 0.5.0b: FAT32, and mounted, and perfectly
     * good for the owner's music -- but it cannot hold an installed app.
     *
     * FAT records no ownership, so every permission on a FAT mount comes from
     * uid=/gid=/fmask=/dmask= applied to the whole filesystem. There is no way
     * to say "an app may read its own app.so but not write it" on one, and no
     * way to give downloads a different regime from music without a second
     * partition. ext4 stores owner, group and mode per inode and says both.
     *
     * Distinct from NEEDS_SETUP because the remedy is different in kind.
     * NEEDS_SETUP is five missing folders and the fix creates them. This is a
     * REFORMAT, which destroys everything on the card, so it is the owner's to
     * accept and the phone will not do it uninvited. */
    ND_CARD_LEGACY_FORMAT
} nd_card_state;

typedef struct {
    nd_card_state state;
    char device[64]; /* "/dev/vdc"; empty when absent  */
    char fstype[32]; /* "vfat"; empty when absent      */
    char label[64];
    char mountpoint[128]; /* always ND_SD_MOUNT_POINT       */
    /* Where the arrival partition is mounted, or empty when the card has
     * none -- a foreign card, a card too small to have been partitioned, or
     * the QEMU virtiofs share, which is one filesystem by construction.
     * Published by neodct-sdcard as `untrusted=` in the state file. */
    char untrusted[128];
    bool removable;       /* fstype != "virtiofs"           */
} nd_card;

/* Never fails; an unreadable state file reads as an absent card. */
void nd_storage_card(nd_card *out);

bool nd_storage_is_ready(void);

/* "<mount>/<name>", but only when the card is ready. Note it does NOT check
 * that name is one of the five folders -- the Python does not either.
 * false means "no card"; out is untouched. */
bool nd_storage_folder(const char *name, char *out, size_t out_sz);

/* mkdir -p all five. false on the FIRST failure; folders created before it
 * stay created. */
bool nd_storage_setup_folders(void);

/* Where a download or an MMS attachment goes: the card's arrival partition.
 *
 * false when there is not one, and the caller's answer to that must be to
 * REFUSE rather than to fall back to the user partition. /NeoDCT/User is
 * 8 MiB on the phone and the settings, the message databases and the call
 * log all write there, so a browser that fills it takes the rest of the
 * system down with it -- which is the failure the two-partition design
 * exists to make impossible.
 *
 * Deliberately NOT gated on the card being "ready": the five NeoDCT folders
 * live on the media partition and have nothing to say about whether the
 * arrival one is mounted. */
bool nd_storage_untrusted_dir(char *out, size_t out_sz);

/* Where to look for wallpapers, tones or music: the stock system directory
 * first, then the card's folder, keeping only directories that exist.
 * STOCK CONTENT ALWAYS COMES FIRST and the apps rely on that ordering.
 * Returns how many entries were written. */
#define ND_STORAGE_PATH_MAX 256
size_t nd_storage_media_dirs(const char *kind, const char *system_dir,
                             char out[][ND_STORAGE_PATH_MAX], size_t max);

/* Every *.ndsw file in the card's update folder, absolute, with UPDATE.ndsw
 * first and the rest case-insensitively alphabetical.
 *
 * The Python does two sorts -- a plain ascending sort by name, then a STABLE
 * sort by (name != "UPDATE.ndsw", name.lower()). One comparator reproduces it:
 * UPDATE.ndsw first, then strcasecmp, then strcmp as the final tie-break so
 * the result is total and the order is reproducible. */
size_t nd_storage_find_updates(char out[][ND_STORAGE_PATH_MAX], size_t max);

/* Test hook. NULL restores the real paths. */
void nd_storage_set_paths(const char *mount_point, const char *state_file);

#ifdef __cplusplus
}
#endif

#endif /* ND_STORAGE_H_INCLUDED */
