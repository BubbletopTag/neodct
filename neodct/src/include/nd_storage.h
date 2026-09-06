/* nd_storage.h -- is there an SD card, is it one of ours, and where do the
 * update files live.
 *
 * A shell helper (neodct-sdcard) mounts the card and writes what it found to
 * /run/neodct/sdcard.prop. Nothing here mounts anything; it reads that file
 * and decides which of the states below the card is in.
 *
 * The state machine, from Storage.card(), in this order:
 *
 *   the state file exists and cannot be read -> ND_CARD_UNKNOWN, and a line
 *                                               in the log saying why
 *   reported "legacy"                        -> ND_CARD_LEGACY_FORMAT
 *   reported "foreign"                       -> ND_CARD_FOREIGN, keeping
 *                                               device/fstype/label
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
     * accept and the phone will not do it uninvited.
     *
     * IT IS NOT "NO CARD". Everything the owner keeps on a card -- music,
     * ringtones, wallpapers, an .ndsw update -- is an ordinary file and reads
     * off FAT exactly as it read off FAT in 0.4.x. See
     * nd_storage_media_available(), which is the question the media apps
     * should be asking and for a while was not. */
    ND_CARD_LEGACY_FORMAT,
    /* The state file is THERE and this process cannot read it.
     *
     * ============ WHY THIS IS NOT ABSENT ============
     *
     * It used to be, and that is how a phone with a card in it came to say
     * "No memory card." nd_props_parse_lenient() answers every failure it can
     * have -- missing, EACCES, EIO, a decode fault -- with the same empty map,
     * so "there is no card in the slot" and "somebody wrote this file with a
     * umask I cannot read past" arrived here as the same empty string and left
     * as the same sentence.
     *
     * That stopped being a theoretical distinction in 0.5.0b. The file is
     * written by root from three contexts with three different umasks (init's
     * 0022 at boot, udevd's 0022 on an insertion, and the broker's inherited
     * 0027 on a format) and READ by a core that is no longer root. An
     * unreadable state file is now a routine outcome of a perfectly ordinary
     * action, and it self-heals on the next boot because /run is a tmpfs --
     * which is exactly the shape of fault nobody can chase from a bug report.
     *
     * The remedy is not in this enum: the writer was taught to chmod 0644.
     * What is here is the ability to SAY so, on the panel and in the log,
     * instead of blaming an empty slot.
     *
     * Callers that only want "is there a card I can use" get false for this
     * as they did before, because everything below is still unknown. It is
     * the screens that report a card's condition that must tell it apart. */
    ND_CARD_UNKNOWN,
    /* A card that was made on somebody else's computer.
     *
     * ============ WHY IT IS NOT "NEEDS SETUP" ============
     *
     * An ext card carries real numeric ownership, and neodct-sdcard mounts it
     * deliberately WITHOUT uid=/gid= (see its try_mount) so that the ownership
     * on the card is the ownership the phone honours. A card that mkfs.ext4
     * left as root:root 0755 -- which is every card made on a PC -- is
     * therefore one this uid can read nothing out of and write nothing into.
     *
     * That is not damage and it is not "not laid out yet". NEEDS_SETUP's
     * remedy is five mkdirs and it keeps everything on the card; this card's
     * only remedy on the phone is a REFORMAT, which destroys everything on
     * it, so it is the owner's to accept -- the same shape of answer as
     * LEGACY_FORMAT, for a different reason.
     *
     * In 0.4.x the core was root and every card was writable, so this state
     * could not arise. The privilege drop turned "Set up" into an offer the
     * phone could not keep, and what it reported when the offer failed was
     * "It may be locked or damaged", which is neither.
     *
     * Published by neodct-sdcard as `foreign` once it has mounted an ext card
     * whose root the phone's own user cannot read. nd_storage_card_is_writable()
     * is the same question asked from inside a process about itself, and stays
     * the authority for a card the helper classified before this state existed.
     *
     * Appended rather than slotted in beside LEGACY_FORMAT so that no existing
     * value is renumbered. */
    ND_CARD_FOREIGN
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

/* Never fails. A state file that is missing reads as an absent card; one that
 * is present and unreadable reads as ND_CARD_UNKNOWN and is logged once. */
void nd_storage_card(nd_card *out);

/* "A card this phone owns, laid out, with real ownership on it." The question
 * to ask before WRITING to a card: installing an app, landing a download,
 * anything that depends on a file's owner meaning something. */
bool nd_storage_is_ready(void);

/* "There is a mounted card here whose files I can read."
 *
 * ============ WHY THIS IS NOT nd_storage_is_ready() ============
 *
 * READY is an ext4 NeoDCT card. LEGACY_FORMAT is the same card in the FAT32
 * layout every NeoDCT card had before 0.5.0b -- mounted, readable, with the
 * owner's music, ringtones, wallpapers and updates on it. The two differ in
 * exactly one respect, that FAT cannot record who owns a file, and that
 * matters to precisely one thing: an installed app.
 *
 * Everything else read the ready flag anyway, because before 0.5.0b there was
 * nothing else to read. So the owner's own card -- the physical card in the
 * phone, the one 0.4.x formatted -- went from working to invisible in an
 * update: no music, no tones, no wallpapers, no updates offered, and a
 * Settings screen whose help text promised that all four still worked. It was
 * reported as "it refuses to recognize an sdcard at all", and from the owner's
 * side that is precisely what it was.
 *
 * MEDIA READS ASK THIS ONE. Ownership-dependent writes ask is_ready().
 *
 * FOREIGN is false here even though it, too, is a mounted card: its root is
 * not readable by this uid, which is the whole of what makes it foreign, so
 * handing out a path into it would produce EACCES at every open instead of an
 * honest "no card I can read". */
bool nd_storage_media_available(void);

/* "<mount>/<name>", but only when the card's files are readable -- READY or
 * LEGACY_FORMAT, i.e. nd_storage_media_available(). Note it does NOT check
 * that name is one of the five folders -- the Python does not either.
 * false means "no card"; out is untouched. */
bool nd_storage_folder(const char *name, char *out, size_t out_sz);

/* mkdir -p all five. false on the FIRST failure; folders created before it
 * stay created. */
bool nd_storage_setup_folders(void);

/* Can THIS process create something at the top of the card?
 *
 * The difference between a card that is damaged and a card that was made on
 * somebody else's computer, and the two want different sentences on the panel.
 * An ext card carries real numeric ownership and is deliberately mounted
 * without uid=/gid= (see neodct-sdcard's try_mount), so a card mkfs.ext4 left
 * as root:root 0755 is one the ndusr core cannot write a single byte to. In
 * 0.4.x the core was root and every card was writable; the privilege drop
 * turned "Set up" into an offer the phone could not keep, and the failure it
 * reported -- "locked or damaged" -- was neither.
 *
 * access(2) against the REAL uid, which for nd-core is the same as its
 * effective one. It is advisory: it answers for this process at this instant
 * and a card can be remounted read-only underneath it. */
bool nd_storage_card_is_writable(void);

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
