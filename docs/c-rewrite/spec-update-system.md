# Update system + crypto — C port specification

*Scope: `neodct/overlay/NeoDCT/System/core/UpdateService/` (7 modules),
`neodct/overlay/NeoDCT/System/apps/Update/main.py`, `neodct/tools/mkupdate.py`,
`neodct/tools/mkbadupdate.py`, `neodct/initramfs/` (3 shell scripts), and the second
consumer `System/engineering/apps/Downgrade/main.py`.*

*Everything in this document was read out of the actual source. Every constant, string,
byte offset and error message quoted here is the real one. If it is written here, copy it
exactly; if it is not written here, go and read the Python before guessing.*

---

## What this does (plain English, for a reader who is not a C programmer)

The phone can replace its own operating system. This is the part that does it, and it is
the one part of NeoDCT that can turn the phone into a brick, so almost all of it is
checking rather than doing.

### The package

An update arrives as a single file called `UPDATE.ndsw`. Despite the name it is an
ordinary **zip file** with four things inside it:

| Member | What it is |
| --- | --- |
| `rootfs.squashfs` | The entire new operating system, about 51 MB. Not just NeoDCT — the whole root filesystem. |
| `manifest.json` | A small text file describing the update: version, build date, which hardware it is for, and the SHA-256 fingerprint of the big file. |
| `manifest.sig` | A digital signature over `manifest.json`. |
| `thumbnail.png` | Optional. A little square picture shown on the "an update is available" screen. |

The clever bit is that **only `manifest.json` is signed**. It does not need to be more
than that, because the manifest contains the fingerprint of the system image and the
fingerprint of the picture. If you can prove the manifest is genuine, and the image
matches the fingerprint the manifest gives, then the image is genuine too. One signature
covers everything.

### The signature

The project owner signs a release on their laptop with:

```
openssl dgst -sha256 -sign neodct-release.key -out manifest.sig manifest.json
```

The phone only ever *checks* signatures, never makes them. Checking an RSA signature is
one piece of arithmetic — take the signature as an enormous number, raise it to a small
power, take the remainder when divided by another enormous number — and then compare the
result against what it should have been. Python can do arithmetic on enormous numbers on
its own, so `signing.py` needs no crypto library at all. **C cannot do this.** C's biggest
built-in number is 64 bits and the numbers here are 4096 bits. So the one genuinely new
piece of C in this subsystem is either a small "big number" routine or a call into a
crypto library. That is spelled out precisely further down.

The check is deliberately paranoid. The correct answer is rebuilt in full — a specific
pattern of `00 01 FF FF FF … FF 00` followed by a 51-byte block — and compared as a whole.
The naive way (hunt for the fingerprint somewhere inside the answer) is what lets forged
signatures through, and there is a test in the suite that forges exactly that shape and
insists it is refused.

### Getting a package onto the phone

Two ways:

1. **From an SD card.** Put `UPDATE.ndsw` in a folder called `update` on a FAT32 card.
2. **Over the internet.** The phone asks GitHub for the list of releases of its own
   repository, finds the one built for its own hardware, and downloads it **onto the SD
   card**. It downloads onto the card because there is nowhere else: the phone's own
   writable partition is 8 MB and a package is nearly 60 MB.

The download can be interrupted and picked up again. The phone is on a mobile network,
through an antenna glued inside a plastic back cover, fetching 53 MB — the connection
drops constantly. Each attempt asks the server for "the rest, from byte N onwards", so
progress accumulates instead of starting over. Five attempts, waiting 5, then 10, then 20,
then 40 seconds between them.

### Installing it

The running system **cannot** write the new system over itself: the root filesystem is
mounted and the kernel is reading pages out of it as it runs. So installation is split in
two.

**Before the reboot,** the Update app:
- opens the zip and reads the manifest,
- refuses it if it is for other hardware or needs a newer kernel (**never** overridable —
  this is the case that bricks the phone),
- refuses it if the signature is missing or wrong (overridable only in engineering mode,
  after a second confirmation),
- shows one page: picture, version, size, date, whether it is signed, release notes,
- copies the contacts/messages databases onto the card as a courtesy backup,
- writes a small text file, `pending.prop`, on the writable partition saying "next boot,
  install the file called UPDATE.ndsw from the card, and here is the SHA-256 it must have",
- and reboots.

**During the next boot,** before anything is mounted, a small shell script inside the
initramfs (`ndsys-apply.sh`) reads `pending.prop`, finds the package on the card, hashes
the image *straight out of the zip before writing a single byte*, writes it to the system
partition, reads it back off the partition and hashes it again, and only then deletes the
pending record. If anything fails it leaves the record alone and tries again next boot,
three times, then gives up and boots the old system.

The initramfs has **no crypto** — there is no room for it — so it cannot check the
signature. That is why the record carries the SHA-256 that the *verified* manifest gave.
Swapping the card between staging and reboot fails the hash rather than installing
something nobody signed.

### dm-verity

After the update is applied, every boot builds a **dm-verity** device over the system
partition. dm-verity is a kernel feature that makes a read-only block device check itself:
every 4 KB block has a SHA-256 hash, those hashes are themselves hashed, and so on up to a
single "root hash". If any block on the flash goes bad or is tampered with, reading it
fails loudly instead of quietly returning wrong data.

The hash tree is built at *build* time by `mkupdate.py` and appended to the end of the
squashfs, so one file holds both the filesystem and the tree that authenticates it:

```
[ squashfs, padded to 4 KB ][ verity superblock, 4 KB ][ hash tree ]
                            ^ image_blocks * block_size
```

The root hash is written into `manifest.json` — so it is signed — and copied into
`installed.prop` when the update lands. Every later boot reads it back from there.

### Recovery

If the phone cannot boot at all — no system image, verity refused it, no `/sbin/init` —
the initramfs drops into a text menu on the phone's own screen offering "update system,
wipe user data, wipe system, reboot, shell". That path can install a package from a card
with no working system, which is the one thing a rescue shell cannot talk a
non-programmer through. It hashes before writing, like the applier, but it cannot check
signatures, so the confirmation screen says so out loud.

---

## Files and where they go in C

### Ported to C

| Python file | LOC | What it is | C destination |
| --- | --- | --- | --- |
| `System/core/UpdateService/__init__.py` | 48 | The four exception classes and the package-format docstring | `nd_update.h` — an `nd_update_err` enum plus a message buffer |
| `System/core/UpdateService/signing.py` | 113 | DER parsing + RSA PKCS#1 v1.5 / SHA-256 verification | `nd_rsa.c` / `nd_rsa.h` (+ `nd_bignum.c` if no crypto library) |
| `System/core/UpdateService/manifest.py` | 114 | `manifest.json` parsing, validation, compatibility rules | `nd_manifest.c` / `nd_manifest.h` (uses `nd_json.c`) |
| `System/core/UpdateService/package.py` | 172 | Opening `.ndsw` zips, signature check, streaming extraction | `nd_package.c` / `nd_package.h` (uses `nd_zip.c`) |
| `System/core/UpdateService/staging.py` | 344 | `KEY=value` pending/installed/result records on the user partition | `nd_staging.c` / `nd_staging.h` |
| `System/core/UpdateService/remote.py` | 321 | GitHub release discovery, resumable HTTPS download | `nd_remote.c` / `nd_remote.h` |
| `System/core/UpdateService/verity.py` | 160 | dm-verity hash-tree construction, superblock, dm table line | `nd_verity.c` / `nd_verity.h` (host tool + the table builder) |
| `System/apps/Update/main.py` | 467 | The Update app: every screen, every refusal, the flow | `apps/Update/app.c` + `apps/Update/update_online.c` |
| `System/engineering/apps/Downgrade/main.py` | 160 | Second consumer of `nd_remote`; installs any published release | `apps/Downgrade/app.c` |

**Total Python in scope for the C port: 1,899 LOC** (1,272 in `UpdateService/`, 467 in the
app, 160 in Downgrade).

### Deliberately NOT ported

| File | LOC | Why it stays as it is |
| --- | --- | --- |
| `neodct/initramfs/init` | 282 | busybox `ash`. Runs before any userspace exists, in a 1.65 MB initramfs that is freed at `switch_root` and therefore costs nothing against the 8 MB budget. Rewriting the one code path that can brick a phone buys zero RAM. |
| `neodct/initramfs/ndsys-apply.sh` | 426 | As above. It is *already* unit-tested on the host by 34 tests. Touching it is pure downside. |
| `neodct/initramfs/ndsys-recovery.sh` | 591 | As above, plus 37 host tests. |
| `neodct/tools/mkupdate.py` | 282 | Host build tool. Never runs on the phone. Keeping it in Python keeps the golden verity vectors and the `.ndsw` byte layout stable across the port. |
| `neodct/tools/mkbadupdate.py` | 226 | Host test tool. Same reasoning. |

**This does not make the initramfs out of scope.** Two hard contracts run across the
boundary and the C side owns both ends of them:

1. **The record format.** `nd_staging.c` writes `pending.prop` and `installed.prop`; the
   shell reads them with `sed`. The exact spelling of every key is a wire format.
2. **The dm-verity table.** `ndsys-apply.sh:verity_table()` and `nd_verity_dm_table()`
   must produce byte-identical strings. `test_initramfs_apply.py::
   test_the_verity_table_matches_what_the_python_side_computes` compares them today and
   must keep passing against the C.

`nd_verity.c` is only strictly needed if `mkupdate` is ever ported. Specify and build it
anyway — it is 300 lines, it is the reference for the table format, and it makes the
golden vectors testable from C.

### Where the C lives at runtime

This matters for the RAM budget and it is a real decision, not bookkeeping.

```
libneodct.so      nd_staging.c            (no dependencies beyond libc)
                  nd_json.c               (shared: app manifests need it too)

libndupdate.so    nd_rsa.c  nd_bignum.c
                  nd_manifest.c
                  nd_zip.c   nd_package.c
                  nd_remote.c
                  nd_verity.c (table builder only)

apps/Update/app.so      links libndupdate.so
apps/Downgrade/app.so   links libndupdate.so
```

**Keep the network stack and the crypto out of the core process.** `libcurl`/`mbedTLS`
and `zlib` are only ever needed while the Update or Downgrade app is on screen, and with
process-per-app those pages are unmapped the moment the app exits. Putting `nd_remote.c`
in `libneodct.so` would map a TLS stack into the core for the entire uptime of the phone
for the sake of a screen the owner visits once a month. `nd_staging.c` is the exception —
it is a few hundred lines of `KEY=value` file handling with no dependencies, and the Power
app (`boot_recovery` flag) and any future About screen want `STATE_DIR`.

---

## Behaviour that must be reproduced exactly

### 1. The `.ndsw` container

Member names, exactly:

```c
#define ND_NDSW_IMAGE      "rootfs.squashfs"
#define ND_NDSW_MANIFEST   "manifest.json"
#define ND_NDSW_SIGNATURE  "manifest.sig"
#define ND_NDSW_THUMBNAIL  "thumbnail.png"
```

Written by `mkupdate.write_ndsw()` in this order: `rootfs.squashfs`, `manifest.json`,
`[thumbnail.png]`, `[manifest.sig]`.

Compression: `rootfs.squashfs` is **ZIP_STORED (method 0)** — a squashfs is already
compressed and deflating 50 MB costs a minute of build time for nothing. Every other
member is **ZIP_DEFLATED (method 8)**. The test fixture
(`neodct/tests/update_fixtures.py:make_ndsw`) writes *all* members deflated, so **the C
zip reader must support both method 0 and method 8** or half the test suite fails.

#### What the C zip reader must do (`nd_zip.c`)

Python's `zipfile` does all of this and the C must match it, because the tests feed it
zips produced by Python.

- Find the **End Of Central Directory** record (signature `0x06054b50`, little-endian) by
  scanning backwards from EOF over at most `22 + 65535` bytes.
- Honour the **Zip64** EOCD locator (`0x07064b50`) and Zip64 EOCD (`0x06064b50`). A 51 MB
  package never needs it, but Python emits it if `allowZip64` triggers and a reader that
  chokes on it is a landmine.
- Parse the **central directory** (`0x02014b50`): name, compression method, CRC-32,
  compressed size, uncompressed size, local header offset. Honour the Zip64 extra field
  (`0x0001`) when any of those is `0xFFFFFFFF`.
- **Duplicate names: the last occurrence in the central directory wins.** Python builds
  `NameToInfo` by assignment in directory order.
- To read a member: seek to its local header (`0x04034b50`), and compute
  `data_offset = local_header_offset + 30 + local_name_len + local_extra_len` using the
  **local header's** name and extra lengths, **not** the central directory's. They
  routinely differ. This is the single most common bug in hand-written zip readers.
- Python compares the filename in the local header against the central directory entry and
  raises `BadZipFile("File name in directory %r and header %r differ")` if they disagree.
  Reproduce the check; map it to `InvalidUpdate`.
- Refuse encrypted members (general-purpose flag bit 0 set).
- **Verify the CRC-32 of every member as it is read**, at EOF. Python raises
  `BadZipFile("Bad CRC-32 for file %r")`. See Risks §R7 for the one place this leaks out
  of the error taxonomy in Python.
- `file_size` (`Package.image_size`) is the **uncompressed** size from the central
  directory.
- Streaming: `nd_zip_open_member()` must return a reader that inflates in bounded chunks.
  **Nothing may ever read `rootfs.squashfs` into memory.** The phone has 53 MB usable and
  the member is 51 MB.

Inflate: use `zlib`'s `inflateInit2(&s, -MAX_WBITS)` (raw deflate, no zlib header). `zlib`
is already in the image.

### 2. `manifest.json` (`nd_manifest.c`)

#### Required fields

```c
static const char *REQUIRED[]        = {"version","buildtime","platform","sha256","verity"};
static const char *REQUIRED_VERITY[] = {"root_hash","block_size","image_blocks"};
```

Optional: `changelog`, `min_kernel`, `thumbnail_sha256`, `verity.salt`.

#### Validation, in this exact order, with these exact messages

`nd_manifest_parse(const uint8_t *raw, size_t len, nd_manifest **out)`:

| Check | Failure → `ND_UPDATE_INVALID` with message |
| --- | --- |
| JSON parses | `"manifest is not valid JSON: %s"` (parser detail appended) |
| top level is an object | `"manifest must be a JSON object"` |
| each of `REQUIRED` present | `"manifest is missing %s"` (first missing, in the order above) |
| `version` is a non-empty string | `"version must be a non-empty string"` |
| `platform` is a non-empty string | `"platform must be a non-empty string"` |
| `buildtime` is an integer, **not** a boolean, **not** a float | `"buildtime must be a unix timestamp"` |
| `sha256` is 64 hex digits | `"sha256 must be a hex string"` / `"sha256 is not valid hex"` / `"sha256 must be 64 hex digits"` |
| `thumbnail_sha256`, **if truthy**, is 64 hex digits | same three messages with `thumbnail_sha256` |
| `verity` is an object | `"verity must be a JSON object"` |
| each of `REQUIRED_VERITY` present | `"verity is missing %s"` |
| `verity.root_hash` is 64 hex digits | as for `sha256`, field `root_hash` |
| `verity.salt` (defaulting to `""`) is valid hex, any length | `"salt must be a hex string"` / `"salt is not valid hex"` |
| `verity.block_size` is an int, `>= 512`, and a power of two | `"block_size must be a power of two >= 512"` |
| `verity.image_blocks` is an int `>= 1` | `"image_blocks must be a positive integer"` |

The hex helper is `_hex(value, field, length)`:
- not a string → `"%s must be a hex string"`
- `bytes.fromhex()` raises → `"%s is not valid hex"`. Note Python's `bytes.fromhex`
  **skips ASCII whitespace** and **accepts uppercase**, and rejects odd length. Match that:
  skip `\t\n\v\f\r ` between digits, accept `[0-9a-fA-F]`, require an even count of digits.
- wrong length → `"%s must be %d hex digits"` where `%d` is `length * 2` (so `64`).

**The value stored is the original string, unmodified.** Uppercase hex stays uppercase.
Later comparisons against a computed digest (which is lowercase) are byte comparisons and
will therefore fail. Reproduce; do not normalise.

`verity["salt"]` is **written back** into the parsed object when it was absent, so
`manifest.verity["salt"]` is always present and is `""` when the package had none.
`staging` then writes `verity_salt=` (empty) into the record, and the shell's
`verity_table()` turns an empty salt into `-`.

#### Derived values

```c
uint64_t hash_offset = image_blocks * block_size;   /* identical to image_bytes */
uint64_t image_bytes = image_blocks * block_size;
```

Both are the *padded squashfs* size. They are **not** the size of the `rootfs.squashfs`
member, which also contains the verity hash area. `stage_package()` records the member
size, not this. Getting these two confused is how you truncate a system image.

#### Optional-field defaults

`changelog`, `min_kernel`, `thumbnail_sha256` all use `body.get(k) or ""` — so JSON
`null`, `false`, `0` and `""` all collapse to the empty string.

#### `check_compatible(platform, kernel)`

```
if manifest.platform != platform:
    IncompatibleUpdate("update is for %s, this is %s" % (manifest.platform, platform))
if manifest.min_kernel and kernel:
    if version_tuple(kernel) < version_tuple(manifest.min_kernel):
        IncompatibleUpdate("update needs kernel %s, running %s" % (min_kernel, kernel))
```

Platform comparison is an exact byte comparison. Real values in the tree: `qemu-aarch64`,
`luckfox-armv7`. They come from `system.os.platform` in
`/NeoDCT/System/version.prop`, generated by `post-build-system-metadata.sh` from
`BR2_ROOTFS_POST_BUILD_SCRIPT_ARGS`.

`_version_tuple("6.12.47")` → `(6, 12, 47)`:
- split on `.`
- for each chunk take the **leading run of ASCII digits**; if there are none, **stop
  entirely** (do not skip the chunk)
- so `"6.12.47-rt"` → `(6, 12, 47)`, `"6.x.3"` → `(6,)`, `""` → `()`

Comparison is Python tuple ordering: element by element; on the first difference that
decides it; if one is a prefix of the other the shorter is smaller, so `(6,12) < (6,12,47)`.

In C:

```c
typedef struct { uint32_t part[8]; uint8_t n; } nd_kver;
int nd_kver_cmp(const nd_kver *a, const nd_kver *b);   /* -1 / 0 / +1 */
```

Cap at 8 components; ignore the rest (no real kernel version has more, and a hostile
manifest must not overflow anything).

`min_kernel` is **never present in a shipped package**: `post-build-system-metadata.sh`
does not write `system.os.min_kernel`, so `mkupdate.read_target_version()` reads `""`. It
only appears via `--min-kernel` or `mkbadupdate --variant future-kernel`.

### 3. RSA PKCS#1 v1.5 / SHA-256 verification (`nd_rsa.c`) — SECURITY CRITICAL

**None of the checks in this section may be weakened, reordered into "search for the
DigestInfo", or replaced with a library call whose padding rules you have not read.**

#### Constants

```c
/* DigestInfo(SHA-256) DER prefix, RFC 8017 §9.2 note 1. 19 bytes. */
static const uint8_t ND_SHA256_DIGEST_INFO[19] = {
    0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,
    0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20
};
/* rsaEncryption OID 1.2.840.113549.1.1.1, 9 bytes. */
static const uint8_t ND_RSA_OID[9] = {
    0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01
};
#define ND_MIN_PADDING 8      /* RFC 8017: PS is at least 8 octets */
```

#### DER reader

`_der_read(buf, offset) -> (tag, value, next_offset)`:

```
tag    = buf[offset]
length = buf[offset+1]
offset += 2
if length & 0x80:
    count  = length & 0x7F
    length = big-endian integer of buf[offset : offset+count]
    offset += count
return tag, buf[offset : offset+length], offset+length
```

Python's slicing clamps silently on overrun. **C must bounds-check every read and return an
error instead** — the input here is a file on a dm-verity-protected read-only squashfs, so
it is not attacker-controlled in practice, but a parser that walks off the end of a buffer
is not acceptable regardless. Reject `count > 4`, reject `offset + count > len`, reject
`offset + length > len`, reject `length` that does not fit in `size_t`. Indefinite length
(`0x80`, `count == 0`) yields length 0 in Python — reproduce or reject; either is safe.

`_der_ints(buf)`: walk TLVs to the end of `buf`; every tag must be `0x02` (INTEGER) or
`ValueError("expected DER INTEGER, got tag 0x%02x")`; each value is read as a big-endian
unsigned integer (the leading `0x00` DER puts on a positive integer is absorbed
naturally).

#### `load_public_key(data)`

Accepts four shapes: PEM or DER, SPKI or bare PKCS#1.

1. If the bytes contain `"-----BEGIN"` anywhere: join every line that is non-empty and does
   **not** start with `"-----"`, then base64-decode. Line splitting follows Python
   `bytes.splitlines()` — `\n`, `\r`, and `\r\n` all split, so CRLF PEM works.
2. Read one TLV. Tag must be `0x30` or `ValueError("public key is not a DER SEQUENCE")`.
3. Read the first TLV inside it.
   - **Tag `0x02`** → this is a bare PKCS#1 `RSAPublicKey ::= SEQUENCE { n INTEGER,
     e INTEGER }`. Take `_der_ints(outer)[0]` and `[1]`.
   - **Otherwise** → SPKI. Check that `ND_RSA_OID` appears **as a byte substring** inside
     the first element's value, else `ValueError("public key is not an RSA key")`. Note
     this is a substring search, not an OID parse — reproduce it, it is more permissive
     than a strict parse and that permissiveness is what accepts every OpenSSL variant.
     Then read the next TLV at `next_offset`; tag must be `0x03` (BIT STRING) or
     `ValueError("expected a DER BIT STRING")`. Drop the first octet (the unused-bits
     count), read one TLV from the rest, and `_der_ints()` its value. **The inner tag is
     not checked**; reproduce that.
4. `key.size = (bit_length(n) + 7) / 8`.

The shipped key is 4096-bit → `size == 512`. The test key is 2048-bit → `size == 256`.
Both must work. `e` is 65537 in every key in the tree, but do not hard-code it.

Key path on the device: **`/NeoDCT/System/keys/neodct-release.pub`** (PEM, SPKI,
4096-bit, committed at `neodct/overlay/NeoDCT/System/keys/neodct-release.pub`).

#### `verify(data, signature, key, algorithm="sha256")`

```
if algorithm != "sha256":        raise ValueError("unsupported digest algorithm %r")
if not signature:                return False
if len(signature) != key.size:   return False
value = big-endian integer of signature
if value >= key.n:               return False
encoded = to_bytes(pow(value, key.e, key.n), key.size, "big")

digest_info = ND_SHA256_DIGEST_INFO || sha256(data)      /* 19 + 32 = 51 bytes */
padding = key.size - 51 - 3
if padding < 8:                  return False
expected = 0x00 0x01 (0xFF * padding) 0x00 || digest_info
return constant_time_equal(encoded, expected)
```

For a 4096-bit key: `padding == 458`. For 2048-bit: `padding == 202`.

Every one of these is load-bearing:

- **Length must be exactly `key.size`.** `test_rejects_a_signature_of_the_wrong_length`
  feeds both a byte short and a byte long.
- **`value >= n` is refused** rather than reduced.
- **The whole block is rebuilt and compared.** `test_rejects_short_padding_even_though_the_digest_is_right`
  builds `00 01 FF*8 00 DigestInfo 00 00 …` — a correct DigestInfo with sloppy padding, the
  Bleichenbacher'06 shape — and requires `False`. A verifier that searches for the
  DigestInfo inside the decrypted block accepts that forgery.
- **The OID must be the SHA-256 one.** `test_rejects_a_sha1_digest_info` builds a properly
  padded block carrying a SHA-1 DigestInfo and requires `False`. No algorithm substitution.
- **The comparison is `hmac.compare_digest`** — constant time. Everything here is public
  data so timing is not a real threat, but the property must survive: use an
  OR-accumulating loop over the full length, never `memcmp` with an early exit and never a
  short-circuit on the first differing byte.

```c
static bool ct_eq(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d = (uint8_t)(d | (a[i] ^ b[i]));
    return d == 0;
}
```

#### `verify_detached(path, signature_path, key_path)`

Reads all three files whole; **any** `OSError` or `ValueError` → `false`. Never raises.
Used by nothing on the device today (`package.verify_signature` opens the key itself), but
keep it — it is the obvious entry point for a future `nd-verify` tool and it is in
`__all__`.

### 4. `Package` (`nd_package.c`)

Constants:

```c
#define ND_MAX_THUMBNAIL_BYTES  (256 * 1024)   /* 262144 */
#define ND_EXTRACT_CHUNK        (256 * 1024)   /* 262144 */
#define ND_SPACE_MARGIN         (4 * 1024 * 1024)  /* 4194304 */
```

**`nd_package_open(path)`**
- zip open fails because the file is missing → `ND_UPDATE_INVALID`,
  `"no such update file: %s"`
- zip open fails for any other reason → `ND_UPDATE_INVALID`,
  `"not a readable zip archive: %s"`
- `manifest.json` absent → `"package has no manifest.json"`
- `rootfs.squashfs` absent → `"package has no rootfs.squashfs"`
  (checked in that order — manifest first)
- then parse the manifest from the member bytes, **keeping the raw bytes verbatim** for
  the signature check. Never re-serialise.
- `pkg->signed` starts `false`.

**`nd_package_verify_signature(pkg, key_path)`**
- `manifest.sig` absent → `ND_UPDATE_BADSIG`, `"update is not signed"`
- key file unreadable or unparseable → `ND_UPDATE_BADSIG`,
  `"cannot read the release key: %s"`
- verification fails → `ND_UPDATE_BADSIG`, `"signature does not match manifest.json"`
- success → `pkg->signed = true`

**`nd_package_read_thumbnail(pkg, uint8_t **out, size_t *len)`**
- `manifest.thumbnail_sha256` empty → `NULL`, `ND_OK` ("no thumbnail")
- `thumbnail.png` member absent → `NULL`, `ND_OK`
- **uncompressed size > 262144 → refuse before reading a byte**: `ND_UPDATE_INVALID`,
  `"thumbnail is %d bytes, over the %d byte limit"`
- read; `sha256_hex(data) != manifest.thumbnail_sha256` (byte comparison, case-sensitive)
  → `ND_UPDATE_INVALID`, `"thumbnail does not match the manifest"`
- **A thumbnail with no hash in the manifest is treated as absent.** Art nobody signed for
  is not art we display.

**`nd_package_extract_image(pkg, dest, progress_cb, user, int64_t free_bytes)`**
- `total = image_size` (uncompressed size of `rootfs.squashfs`)
- if `free_bytes < 0`, call `statvfs(dirname(dest))`; `f_bavail * f_frsize`. On failure,
  skip the check entirely (Python returns `None` and the `is not None` guard skips it).
- `free_bytes < total + 4194304` → `ND_UPDATE_NOSPACE`,
  `"image needs %d bytes, only %d free"`
- stream in 262144-byte chunks: write, update SHA-256, `progress(done, total)` after each
  chunk
- `fflush` + `fsync` before closing
- digest mismatch → `ND_UPDATE_INVALID`,
  `"image sha256 does not match the manifest (%s != %s)"`
- **on any failure whatsoever, unlink `dest` before returning.** A half-written image must
  never be left where the applier could find it.

`extract_image` is **not on the shipped install path any more** — the app uses
`stage_package`. It is still exercised by 5 tests and is the only code that can turn a
`.ndsw` into a bare image, so keep it.

### 5. Staging records (`nd_staging.c`) — the wire format with the initramfs

```c
#define ND_STATE_DIR        "/NeoDCT/User/.ndsys"
#define ND_PENDING_RECORD   "pending.prop"
#define ND_PENDING_IMAGE    "pending.img"
#define ND_INSTALLED_RECORD "installed.prop"
#define ND_RESULT_RECORD    "last_result.prop"
#define ND_MAX_ATTEMPTS     3
```

`/NeoDCT/User` is the only writable storage. Nothing about updates may ever be written
under `/NeoDCT/System` or `/etc` — both are read-only squashfs under dm-verity at runtime.
There is a test that asserts `STATE_DIR.startswith("/NeoDCT/User/")`.

#### Writing (`_write_record`) — atomic, in this exact order

1. **Refuse any value containing `\n` or `\r`** → `ValueError("%s contains a newline:
   records are one line per key")`. Records are one line per key and the shell reader
   would silently split. This is a real check with a test behind it.
2. `mkdir -p` the directory.
3. Write to `<path>.new`, keys in **ascending byte order of the key name**, one
   `"%s=%s\n"` per key.
4. `fflush`, `fsync(fd)`, `close`.
5. `rename(<path>.new, <path>)`.
6. `open(dir, O_RDONLY)`, `fsync(dirfd)`, `close` — errors ignored at every step. Without
   the directory fsync a power cut can lose the rename.

Values are stringified with `"%s"`, so integers become decimal with no padding and no
sign. `attempts=0` on a fresh record.

#### Reading (`_read_record`)

For each line: strip leading and trailing ASCII whitespace **from the whole line first**;
skip if empty, if it starts with `#`, or if it contains no `=`; split at the **first** `=`;
key is the left part with whitespace stripped; **value is the right part verbatim** (so
`"version = 1.2"` yields key `version` and value `" 1.2"` — the leading space survives).
Later lines overwrite earlier ones, so **the last occurrence of a duplicate key wins.**

⚠️ The shell reader disagrees: `getprop()` is
`sed -n "s/^KEY=//p" "$file" | head -n1`, which takes the **first** occurrence. This
divergence is exercised in `test_a_changelog_full_of_shell_metacharacters_cannot_run_commands`,
which appends a second `version=` line. Reproduce the Python side (last wins) in C; do not
"fix" it to match the shell.

Missing or unreadable file → `NULL` (not an error).

#### `stage(manifest, image_path, state_dir)` — the "copy an image" path

```
mkdir -p state_dir
image = state_dir/pending.img
unlink(state_dir/pending.prop)          # record first, so a crash reads as "nothing pending"
rename(image_path, image)               # MOVED, never copied
fsync(state_dir)
write_record(state_dir/pending.prop, {
    image               = <absolute path of image>,
    image_bytes         = st_size of image,
    sha256              = manifest.sha256,
    version             = manifest.version,
    buildtime           = manifest.buildtime,
    platform            = manifest.platform,
    verity_root_hash    = manifest.verity.root_hash,
    verity_block_size   = manifest.verity.block_size,
    verity_image_blocks = manifest.verity.image_blocks,
    verity_salt         = manifest.verity.salt (may be ""),
    attempts            = 0,
})
return read_pending(state_dir)
```

Sorted key order on disk: `attempts, buildtime, image, image_bytes, platform, sha256,
verity_block_size, verity_image_blocks, verity_root_hash, verity_salt, version`.

#### `stage_package(manifest, package_path, image_bytes, state_dir)` — the path the app actually uses

```
mkdir -p state_dir
unlink(state_dir/pending.prop)   # record first
unlink(state_dir/pending.img)
write_record(state_dir/pending.prop, {
    package             = basename(package_path),   # basename ONLY, never a path
    image_bytes         = image_bytes,              # size of the rootfs.squashfs MEMBER
    sha256              = manifest.sha256,
    version, buildtime, platform,
    verity_root_hash, verity_block_size, verity_image_blocks, verity_salt,
    attempts            = 0,
})
fsync(state_dir)
return read_pending(state_dir)
```

`image_bytes` here is the **zip member size** (padded squashfs **plus** the appended verity
hash area), which the caller reads from the zip. It is deliberately **not**
`manifest.image_bytes` (which is the squashfs alone). The recorded `sha256` covers the
member, and the applier hashes exactly that many bytes back off the device after writing.

Why this path exists, verbatim from the docstring, because it is the reason the whole
design changed: *"stage() cannot work on the hardware. It writes the whole system image to
the user partition first, and on the Luckfox that partition is 8 MiB against a 51 MiB
image — there is no card large enough to help, because the card is not where the copy
goes. QEMU builds userdata at 512 MiB, which is why the fault only ever appeared on a real
phone."*

Sorted key order: `attempts, buildtime, image_bytes, package, platform, sha256,
verity_*, version`. No `image` key.

#### `Record` accessors

| Accessor | Behaviour |
| --- | --- |
| `image` | `join(state_dir, basename(values["image"]) or "pending.img")`. **The recorded path is never trusted** — the phone has the partition at `/NeoDCT/User`, the initramfs has it at `/mnt/user`. This is what made a real staged update vanish as "incomplete". |
| `package` | `basename(values["package"])`, `""` if absent. Basename only, for the same reason. |
| `from_package` | `package != ""` |
| `image_bytes` | `int(values["image_bytes"])` |
| `attempts` | `int(values.get("attempts", 0))` |
| `exhausted` | `attempts >= 3` |
| `verity_block_size` / `verity_image_blocks` | `int(...)` |
| `hash_offset` | `verity_image_blocks * verity_block_size` |
| anything else | raw string from the dict, `AttributeError` if absent |

`int()` in Python accepts surrounding whitespace and a leading `+`/`-`. Use `strtoll` with
`errno` and end-pointer checking; treat trailing garbage as a parse failure.

#### `read_pending(state_dir)`

```
values = read_record(state_dir/pending.prop)
if not values:                                        return NULL
if any of ("image_bytes","sha256","version","platform",
           "verity_root_hash","verity_block_size",
           "verity_image_blocks") missing:            return NULL
if values has neither a non-empty "image" nor "package":  return NULL
record = Record(values, state_dir)
if record.from_package:                               return record     # no further checks
if not exists(record.image):                          return NULL
if any of verity_block_size / verity_image_blocks / image_bytes fails int():  return NULL
if size(record.image) != image_bytes:                 return NULL       # short = interrupted copy
return record
```

Note the asymmetry: a **package** record is returned even if the card is not in the phone.
Whether the file is still there is the applier's question at boot, not the reader's.

#### The others

- `clear_pending(state_dir)` — unlink `pending.prop` and `pending.img`, fsync the dir.
- `note_attempt(state_dir)` — read the record, `attempts = int(attempts or 0) + 1`, rewrite
  the whole record, return the new count. (Only the tests call it; the shell applier does
  its own increment with `sed`.)
- `record_installed(manifest, state_dir, image_bytes)` — writes `sha256, image_bytes
  (or "" when NULL), version, buildtime, platform, verity_root_hash, verity_block_size,
  verity_image_blocks, verity_salt`. No `attempts`, no `package`, no `image`.
- `read_installed(state_dir)` — a `Record` with no validation at all, or `NULL`.
- `record_result(state_dir, result, **fields)` — writes `result=<result>` plus every
  supplied field whose value is not `None`. The app and applier use `version` and `reason`.
  **Note the Python argument order: `record_result(state_dir, result, ...)`** — state first.
- `read_result(state_dir)` — the raw dict, or `NULL` if the file is missing/empty.
- `clear_result(state_dir)` — unlink.

### 6. The remote path (`nd_remote.c`)

```c
#define ND_DEFAULT_REPO        "BubbletopTag/neodct"
#define ND_REPO_ENV            "NEODCT_UPDATE_REPO"
#define ND_API_ALL             "https://api.github.com/repos/%s/releases?per_page=%d"
#define ND_USER_AGENT          "NeoDCT-Update/1.0 (+https://github.com/BubbletopTag/neodct)"
#define ND_CONNECT_TIMEOUT     20      /* seconds */
#define ND_DOWNLOAD_TIMEOUT    120     /* seconds */
#define ND_DOWNLOAD_ATTEMPTS   5
#define ND_RETRY_BACKOFF       5       /* seconds, doubling */
#define ND_RETRY_BACKOFF_MAX   60
#define ND_DOWNLOAD_CHUNK      (64 * 1024)     /* 65536 */
#define ND_REMOTE_SPACE_MARGIN (8 * 1024 * 1024)  /* 8388608 */
#define ND_RELEASES_LIMIT      30      /* per_page */
```

`repo()` = `getenv("NEODCT_UPDATE_REPO")` if non-empty, else `ND_DEFAULT_REPO`.

`asset_name(platform)` = `"UPDATE-%s.ndsw"` → e.g. `UPDATE-luckfox-armv7.ndsw`.

#### `_open(url, timeout, headers)` — the one place that touches the network

Always sends:
```
User-Agent: NeoDCT-Update/1.0 (+https://github.com/BubbletopTag/neodct)
Accept: application/vnd.github+json
```
(GitHub rejects requests with no `User-Agent`. The `Accept` header is sent on the asset
download too, which is harmless.)

**TLS is verified against the image's `ca-certificates` bundle
(`/etc/ssl/certs/ca-certificates.crt`), with hostname checking on.** `ssl.create_default_context()`
means `CERT_REQUIRED`, `check_hostname=True`, minimum TLS 1.2. From the module docstring:
*"An unverified fetch would make the signature check the only thing standing between the
phone and a hostile package, and one line of defence is not enough for something that
replaces the rootfs."* **Do not add an "insecure" flag. Do not make it configurable.**

Redirects are followed (urllib default: max 10, max 4 repeats of the same URL). GitHub's
`browser_download_url` redirects to `objects.githubusercontent.com`, so this is not
optional. The `Range` header must survive the redirect.

Error mapping:

| Condition | Result |
| --- | --- |
| HTTP status 404 | `NoRelease("no published release for %s" % repo())` |
| any other HTTP status >= 400 | `NetworkError("GitHub said %s %s" % (code, reason))` |
| DNS / connect / socket failure | `NetworkError("cannot reach GitHub: %s" % reason)` |
| other `OSError` / TLS error at connect | `NetworkError("network error: %s" % exc)` |

#### `all_releases(platform, limit=30)`

`GET https://api.github.com/repos/<repo>/releases?per_page=30`, read the whole body,
decode as UTF-8 with replacement on bad bytes, parse JSON.

- not valid JSON → `NetworkError("GitHub sent something that is not JSON")`
- not a JSON array → `NetworkError("GitHub did not send a list of releases")`
- for each entry, in the order GitHub returned them (newest published first):
  - `tag = entry["tag_name"] or ""`
  - scan `entry["assets"] or []` for the **first** asset whose `name` equals
    `asset_name(platform)`; if found, emit and stop scanning this entry
  - emitted record: `version = tag.lstrip("v")` (**strips every leading `v`**),
    `tag`, `url = asset["browser_download_url"]`, `size = int(asset["size"] or 0)`,
    `notes = entry["body"] or ""`, `prerelease = bool(entry["prerelease"])`
- nothing emitted → `NoRelease("no release carries %s" % wanted)`

A release with no asset for this platform is **skipped, not shown and then refused**.

#### `latest(platform)`

`max(all_releases(platform), key=version_key)`. Python's `max` keeps the **first** maximal
element, so on a tie the earlier entry in GitHub's list wins — scan in order with a strict
`>`.

**It must not use `/releases/latest`.** That endpoint ignores prereleases; every NeoDCT
release is a prerelease; the endpoint therefore answered 404 for every release ever made
and the phone reported "no published release". There is a test
(`test_the_phone_does_not_ask_for_the_latest_release`) that inspects the fetched URL.

Ordering by version rather than publication date is also deliberate: re-publishing an old
tag must not make it the newest thing on offer.

#### `version_key(text)` and `is_newer`

```
chunks = split(text.strip(), on any of  . - _)
for chunk:
    if chunk matches ^([0-9]+)([A-Za-z]*)$ :  emit (number, suffix)
    elif chunk non-empty:                     emit (-1, chunk)
    else:                                     emit nothing
```

Compare the resulting lists element by element: numbers first (integer compare), then
suffixes (byte compare); if all common elements are equal, the shorter list is smaller.

`is_newer(candidate, installed)` = `true` if `installed` is empty/NULL, else
`version_key(candidate) > version_key(installed)`.

Pinned by tests: `0.3.10a > 0.3.9a`, `0.4.0a > 0.3.99a`, `0.3.7a` is not newer than itself.

C shape:

```c
#define ND_VERKEY_MAX_PARTS 8
#define ND_VERKEY_MAX_SUFFIX 16
typedef struct { int64_t num; char suf[ND_VERKEY_MAX_SUFFIX]; } nd_verpart;
typedef struct { nd_verpart p[ND_VERKEY_MAX_PARTS]; uint8_t n; } nd_verkey;
void nd_verkey_parse(const char *text, nd_verkey *out);
int  nd_verkey_cmp(const nd_verkey *a, const nd_verkey *b);
```

#### `enough_space(directory, size)`

`statvfs`; return `f_bavail * f_frsize >= size + 8388608`. **On `statvfs` failure return
`true`** — "cannot tell; let the write fail honestly".

#### `download(url, destination, size, progress, attempts=5)`

```
partial = destination + ".part"
have = size_of(partial) or 0
if size and have > size:  unlink(partial); have = 0      # left over from a bigger package
if size and not enough_space(dirname(destination), max(0, size - have)):
    raise UpdateError("not enough room on the card for %d bytes" % size)

last = NULL
for attempt in 0 .. max(1, attempts) - 1:
    if attempt:  sleep(min(5 * (1 << (attempt-1)), 60))
    result = fetch_into(url, partial, have, size, progress)
    if result is NetworkError:
        last = result; have = size_of(partial) or 0; continue
    if result is a local I/O error:
        unlink(partial); raise UpdateError("could not write the download: %s" % err)
    done = result
    if size and done != size:
        last = NetworkError("download stopped early (%d of %d bytes)" % (done, size))
        have = done; continue
    rename(partial, destination); return done
raise last or NetworkError("download did not finish")
```

Backoff sequence with the default 5 attempts: **0, 5, 10, 20, 40** seconds. With 8
attempts: 0, 5, 10, 20, 40, 60, 60, 60. `test_attempts_wait_longer_each_time` asserts
`[5, 10, 20]` for `attempts=4`; `test_the_first_attempt_does_not_wait` asserts the first
attempt sleeps zero.

`_fetch_into(url, partial, have, size, progress)`:

```
headers = have ? {"Range": "bytes=<have>-"} : {}
response = _open(url, timeout=120, headers)
resuming = have && response.status == 206
if have && !resuming:  have = 0            # server ignored the Range; take it from the top
mode = resuming ? "ab" : "wb"
done = have
loop: read up to 65536; write; done += n; progress(done, size)
fflush; fsync
return done
```

**A server that answers 200 to a Range request is sending the whole file, and appending it
would write the first N bytes twice** — producing a package that fails its hash with
nothing to explain why. Check the status; do not guess. Test:
`test_a_server_that_ignores_range_is_started_over`.

The partial file survives a failed attempt. From the docstring: *"The old version deleted
its partial file on any network error, so every attempt began at zero and a link that could
not carry the whole package in one run could never carry it at all — it did not matter how
many times you pressed the button."* Test: `test_giving_up_still_leaves_the_progress`
asserts 300 bytes remain after three cut-at-100 attempts.

`rename(partial, destination)` only happens on a complete download, so a half-finished file
is never mistaken for an installable package.

##### Timeouts — read this before touching libcurl

Python's `urlopen(timeout=N)` sets a **socket timeout**: it applies to the connect and then
to *each individual recv*. It is **not** a total-transfer deadline. `CURLOPT_TIMEOUT` **is**
a total deadline and would abort a slow 53 MB download at 120 seconds. The correct mapping:

```c
curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 20L);       /* both call sites */
curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);       /* bytes/sec */
curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 120L);      /* 20L for the API call */
/* CURLOPT_TIMEOUT is NOT set. */
```

### 7. The Update app (`apps/Update/app.c`)

```c
#define UPDATE_ROOT_ID    12
#define UPDATE_RELEASE_KEY "/NeoDCT/System/keys/neodct-release.pub"
#define UPDATE_APP_ICON    "/NeoDCT/System/apps/Update/icon.png"
#define UPDATE_USER_DB_DIR "/NeoDCT/User/db"
#define UPDATE_HEADER      "SOFTWARE UPDATE"
#define UPDATE_COPY_HINT   "Do not remove the card"
#define KEY_ENTER 28
#define KEY_BACK  14
```

`apps/Update/manifest.json` (unchanged by the port):
`{"name": "Update", "id": "12", "icon": "icon.png", "exec": "main.py"}` — the `exec` field
becomes whatever the C launcher expects (`app.so`).

#### The three long help strings, verbatim

`NO_CARD_HELP`:
```
Updates are read from an SD card.

Format a card as FAT32, make a folder called "update" on it and copy UPDATE.ndsw into that folder.

Put the card in the phone and open Update again.
```

`NOT_READY_HELP`:
```
The card in the phone is not set up for NeoDCT.

Settings can prepare it for you: Settings, Memory card, Prepare card.

Preparing a card makes the folders NeoDCT uses. It does not erase what is already on it.
```

`NO_PACKAGE_HELP`:
```
There is nothing to install from the card.

To update, copy UPDATE.ndsw into the "update" folder on the card.

An update file is built by "make update" in the buildroot tree and fits one kind of hardware only -- a QEMU build will not install on a real phone, or the other way round.
```

(The blank lines are real `\n\n` paragraph breaks. `--` is two ASCII hyphens, not an em
dash. `"update"` and `"UPDATE.ndsw"` use plain ASCII double quotes.)

#### Helpers

```c
_format_size(n)   ->  "%.1f MB"  with  n / 1048576.0
_format_date(ts)  ->  strftime("%d %b %Y", gmtime(ts))    /* e.g. "27 Jul 2026" */
                      on any failure -> "unknown"
```

`_format_date` uses **`gmtime`, not `localtime`** — the build date must read the same on
every phone. `%b` is the C locale's abbreviated month, which is what Python's
`time.strftime` gives with the default locale. Set `LC_TIME=C` or format the month from a
static table to be certain.

`_size_detail(done, total)` → `"%s of %s"` exists in the Python but is **dead code** — no
caller. Port it or drop it; note it either way so the next reader is not confused.

`_engineering_mode(ui)`:
```
flag = ui.engineering_mode
if flag is not None:  return bool(flag)
return upper(strip(get_setting("system.ui.engineering_mode", "ON")))
       in {"ON","1","TRUE","YES","ENABLED"}
```
Default when the setting is missing is **`"ON"`**, i.e. engineering mode is on by default.

`_installed_version()` = `strip(get_setting("system.os.versionnumber", ""))`.

#### `_refuse(ui, message)`

`MessageDialog(ui, message, button_text="OK", cancel_keys=())`. **`cancel_keys=()` means C
does not dismiss it** — the only way out is the softkey. Every dead-end refusal uses this,
and three tests assert the empty tuple.

#### `_confirm(ui, message, button_text)`

`MessageDialog(ui, message, button_text=button_text).show() == 28`. Default `cancel_keys`
is `(14,)`, so C cancels.

#### `_install(ui, path)` — the whole flow, in order

```
1.  pkg = open_package(path)
        InvalidUpdate -> _refuse("INVALID UPDATE! UPDATE MAY BE CORRUPT!!"); return
                         (no override: nothing installable is in the file)

2.  manifest.check_compatible(platform = get_setting("system.os.platform", "unknown"),
                              kernel   = uname().release)
        IncompatibleUpdate -> _refuse("WRONG UPDATE FOR THIS PHONE!\n" + str(exc)); return
                         (NEVER overridable: this is the brick case)

3.  pkg.verify_signature("/NeoDCT/System/keys/neodct-release.pub")
        BadSignature ->
            _refuse("BAD SIGNATURE! UPDATE MAY BE CORRUPT!!")
            if not engineering_mode:                       return
            if not _confirm("Install Anyway?", "OK"):      return
        (pkg.signed stays false on the override path -> the page badge says "Not signed")

4.  if _update_page(ui, pkg) != ENTER:  return

5.  progress = ProgressScreen(ui, "Backing up data", header="SOFTWARE UPDATE",
                              hint="Do not remove the card")
    backed_up = _backup_user_data(progress)
    progress.set_step("Preparing update")

6.  if _stage(ui, pkg, progress):  _restart_page(ui, manifest, backed_up)
```

Note the ordering: **compatibility before signature**. Three tests depend on it.

The exact refusal strings, with the exclamation marks, are:
- `"INVALID UPDATE! UPDATE MAY BE CORRUPT!!"`
- `"WRONG UPDATE FOR THIS PHONE!\n%s"` where `%s` is the `IncompatibleUpdate` message
- `"BAD SIGNATURE! UPDATE MAY BE CORRUPT!!"`
- `"Install Anyway?"` with softkey `"OK"`

#### `_update_page(ui, pkg)`

```c
DetailPage(ui,
    title       = manifest.version,           /* the version alone — the hero column is narrow */
    subtitle    = "<size MB>\n<dd Mon yyyy>", /* two lines */
    badge       = pkg->signed ? "Verified" : "Not signed",
    body        = manifest.changelog[0] ? manifest.changelog
                                        : "No release notes came with this build.",
    image       = _thumbnail(pkg),
    header      = "SOFTWARE UPDATE",
    softkey_text= "Install")
```

`_thumbnail(pkg)`: call `read_thumbnail`; on `InvalidUpdate` or an I/O error treat it as
absent; if there are bytes, decode the PNG and return it; **on any decode failure fall back
to `UPDATE_APP_ICON`**. A picture that does not match the manifest is a broken attachment,
not a reason to refuse an update whose image hashes fine.

#### `_restart_page(ui, manifest, backed_up)`

```
title    = "Ready"
subtitle = "NeoDCT <version>"
body     = "The phone will restart to finish installing NeoDCT <version>. It takes about a
            minute and the screen stays dark for part of it."
   if not backed_up, append:
           "\n\nYour contacts and messages were not backed up to the card. They stay on the
            phone either way: user data is on its own partition and an update does not
            touch it."
image        = APP_ICON
softkey_text = "Restart"
cancel_keys  = ()          /* no way back from here */
```
then `_reboot(ui)`.

(The body strings above are wrapped for this document. In the source they are single
logical strings with single spaces; reproduce them as one line each.)

#### `_backup_user_data(progress)` — best effort, never blocks the install

```
target = Storage.folder("backup_db")           # NULL unless the card state is "ready"
if not target:  return false
names = sorted(entries of /NeoDCT/User/db ending in ".db")     # byte sort
if listing fails or names is empty:  return false
destination = target + "/" + strftime("%Y%m%d-%H%M%S")         # LOCAL time
mkdir -p destination
for i, name in enumerate(names):
    progress.draw(i, len(names))
    copy2(db_dir/name -> destination/name)     # contents + mtime/atime
progress.draw(len(names), len(names))
system("sync")
on any OSError:  return false
return true
```

`shutil.copy2` preserves mtime/atime and mode. On FAT32 the mode is meaningless but the
timestamps are not. `strftime` here is **local** time (`time.strftime`), unlike
`_format_date`.

#### `_stage(ui, pkg, progress)`

```
mkdir -p staging.STATE_DIR
   OSError -> _refuse("Cannot write to the user partition.\n%s"); return false
staging.stage_package(pkg.manifest, pkg.path, pkg.image_size, STATE_DIR)
   OSError -> _refuse("Could not stage the update.\n%s"); return false
system("sync")
return true
```

Nothing is copied. `pkg.image_size` is the uncompressed size of the `rootfs.squashfs`
member.

#### `_reboot(ui)`

`sync`, then try to spawn, in order, `["reboot"]`, `["/sbin/reboot"]`,
`["busybox","reboot"]` — first one that execs wins. Then `sleep(30)`: *"init takes a moment
to bring things down; sit here rather than returning to the launcher and looking like
nothing happened."* Same shape as `apps/Power/main.py`.

In C: `posix_spawnp`/`fork`+`execvp` per the coding standards' fork-then-exec rule.

#### `_choose_package(ui, packages)`

One package → use it without asking. Otherwise a `VerticalList(ui, "Updates", basenames,
app_id=12)` with `SoftKeyBar(ui).update("Select", present=False)`. A selection of `-1`
cancels.

`Storage.find_updates()` returns absolute paths for every entry in the card's `update/`
folder whose name ends (case-insensitively) in `.ndsw` and which is a regular file, sorted
by `(name != "UPDATE.ndsw", lowercase(name))` — i.e. **`UPDATE.ndsw` first**, everything
else alphabetically after it.

#### `_has_network()`

```
if getenv("NEODCT_STUB"):  return false          # the host stub's /proc is not the phone's
read /proc/net/route, skip the header line:
    fields = split(line);  if len(fields) > 2 and fields[1] == "00000000":  return true
read /proc/net/ipv6_route:
    fields = split(line);  if len(fields) > 1 and fields[0] == "0"*32 and fields[1] == "00":
        return true
return false
```

The IPv6 branch is the one that matters on the real phone: T-Mobile is IPv6-only. A default
route there is destination `::/0` — an all-zero 32-hex-digit prefix with a `00` prefix
length.

#### `_check_online(ui)`

```
platform  = get_setting("system.os.platform", "unknown")
installed = _installed_version()

ProgressScreen(ui, "Checking for updates", header=HEADER).draw(0, 1)
found = remote.latest(platform)
    NoRelease -> DetailPage("Nothing published", "for this phone",
        "There is no update for <platform> in the latest release.\n\nThat is normal while
         a release is still being uploaded. Try again shortly.", APP_ICON); return NULL
    NetworkError -> DetailPage("No connection", "Could not reach GitHub",
        "<exc>\n\nMobile data has to be working before the phone can look for updates.
         Updates can still be installed from the card.", APP_ICON); return NULL

if not is_newer(found.version, installed):
    DetailPage("Up to date", "NeoDCT <installed or '?'>",
        "The newest release is <found.version>, which is what this phone is already
         running.", APP_ICON); return NULL

if not _confirm("Download NeoDCT <version>?\n<size MB>", "Download"):  return NULL

folder = Storage.folder("update")
if not folder:  _refuse("The card has no update folder."); return NULL
destination = folder + "/" + asset_name(platform)

progress = ProgressScreen(ui, "Downloading <version>", header=HEADER)
remote.download(found.url, destination, size=found.size,
                progress = lambda done, total: progress.draw(done, total or found.size or 1))
    NetworkError -> _refuse("Download failed.\n<exc>\n\nWhat has downloaded so far is kept
                             on the card. Choosing it again carries on from there.")
                    return NULL
    UpdateError  -> _refuse("Download failed.\n<exc>"); return NULL
return destination
```

Note the progress lambda's denominator: `total or found["size"] or 1` — never zero.

#### `run(ui)` — the top level

```
_report_last_result(ui)

card = Storage.card()
if card.state == "absent":
    DetailPage("No SD card", "Updates come from a card", NO_CARD_HELP, APP_ICON); return
if card.state != "ready":
    DetailPage("Not ready", "The card is not set up", NOT_READY_HELP, APP_ICON); return

packages = Storage.find_updates()
if not packages:
    if _has_network() and _confirm("No update on the card.\nLook online?", "Check"):
        path = _check_online(ui)
        if path:  _install(ui, path)
        return
    version = _installed_version()
    DetailPage("Up to date",
               version ? "NeoDCT <version>" : "Nothing to install",
               NO_PACKAGE_HELP, APP_ICON)
    return

path = _choose_package(ui, packages)
if path:  _install(ui, path)
```

`_report_last_result(ui)`:
```
result = staging.read_result()                       # default STATE_DIR
if not result:  return
version = result.get("version", "")
if result.get("result") == "ok":
    DetailPage("Updated", "NeoDCT <version>",
        "Everything on the phone came across: your contacts, messages and settings live on
         their own partition and are untouched by an update.",
        APP_ICON, softkey_text="OK")
else:
    _refuse("Update to <version> failed.\n<result.reason or 'unknown reason'>")
staging.clear_result()
```

The result is shown **once** and then forgotten, whichever way it went.

#### Card states (from `System/core/Storage`)

`Storage.card().state` is one of `absent`, `ready`, `needs_setup`, `unformatted`, derived
from `/run/neodct/sdcard.prop` (`state=mounted|share|ready` plus all five folders present
→ `ready`; `state=unmountable|unformatted` → `unformatted`; anything else → `absent`).
Folders: `wallpapers, tones, backup_db, music, update`. Mount point:
`/NeoDCT/User/sdcard`. Only a `ready` card hands out paths.

### 8. dm-verity (`nd_verity.c`)

```c
static const uint8_t ND_VERITY_MAGIC[8] = {'v','e','r','i','t','y',0x00,0x00};
#define ND_VERITY_SUPERBLOCK_SIZE 512
#define ND_VERITY_HASH_TYPE       1     /* 0 = Chrome OS, 1 = normal */
```

#### Hash tree layout (matches `veritysetup` / cryptsetup, hash type 1)

- every digest is `sha256(salt || block)`
- level 0 holds the digests of the data blocks, packed contiguously into
  `hash_block_size`-sized blocks and zero-padded to a whole block
- each level above holds the digests of the level below
- **levels are stored top level FIRST, level 0 LAST** — the order the kernel's
  `verity_ctr()` `hash_level_block[]` loop walks them
- the root hash is `sha256(salt || top_block)`
- **with exactly one data block there is no tree at all**: `tree = ""`, `levels = 0`,
  `hash_blocks = 0`, and the root hash is that single data block's own digest

`build_hash_tree(image, salt, data_block_size=4096, hash_block_size=4096,
algorithm="sha256")`:
- image length not a multiple of `data_block_size` → `ValueError("image is %d bytes, not a
  multiple of the %d byte data block size")`
- zero blocks → `ValueError("image is empty")`

⚠️ `_hashes_per_block()` computes `1 << (bit_length(hash_block_size / digest_size) - 1)`
(i.e. rounded **down to a power of two**, because dm-verity indexes with shifts) — and then
**the result is never used.** `pack()` concatenates digests contiguously and pads to the
block. For SHA-256 at 4096 both give 128, so the two agree and everything is correct. For
SHA-1 (20 bytes) they would disagree (204 vs 128) and the tree would be wrong. Reproduce
the contiguous packing (that is what produced the golden vectors), and add
`assert(digest_size == 32 && hash_block_size == 4096)` so nobody discovers this the hard
way.

#### Superblock (512 bytes, little-endian, no padding)

Python format string `"<8sII16s32sIIQH6s256s168s"`:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | magic `"verity\0\0"` |
| 8 | 4 | version = 1 |
| 12 | 4 | hash_type = 1 |
| 16 | 16 | UUID (random if not supplied) |
| 32 | 32 | algorithm name, NUL-padded (`"sha256"`) |
| 64 | 4 | data_block_size |
| 68 | 4 | hash_block_size |
| 72 | 8 | data_blocks (u64) |
| 80 | 2 | salt_size |
| 82 | 6 | pad1 (zero) |
| 88 | 256 | salt, NUL-padded |
| 344 | 168 | pad2 (zero) |
| | **512** | |

`format_hash_area(tree, uuid)` = superblock **left-justified with zeros to
`hash_block_size`** (so 4096 bytes) followed by the tree. Salt longer than 256 bytes →
`ValueError("salt is %d bytes, max 256")`.

`parse_superblock(area)` reads it back; wrong magic →
`ValueError("no verity superblock (magic is %r)")`.

#### The dm table line — must match `ndsys-apply.sh` byte for byte

```
0 <sectors> verity 1 <device> <hash_device|device> <data_block_size> <hash_block_size>
  <data_blocks> <hash_start_block> <algorithm> <root_hash> <salt_hex|->
```
joined with single spaces, where

```
sectors          = data_blocks * data_block_size / 512
hash_start_block = hash_offset / hash_block_size + 1      /* +1 skips the superblock block */
```

`hash_offset` must be a multiple of `hash_block_size` or
`ValueError("hash offset %d is not hash-block aligned")`. An empty salt is written as a
literal `-`.

Worked example from the tests (129 blocks of 4096, salt `f8e7d6c5b4a3`, one device
`/dev/vdb`, `hash_offset = 129*4096`):

```
0 1032 verity 1 /dev/vdb /dev/vdb 4096 4096 129 130 sha256 <root> f8e7d6c5b4a3
```

The shell version reads the same numbers out of `installed.prop` and computes
`sectors = image_blocks * block_size / 512`, `hash_start = image_blocks + 1` — identical
because the image always has `hash_block_size == block_size` and the hash area starts
immediately after the data.

#### Golden vectors (from `veritysetup` 2.8.6) — keep these in the C test suite

Data is `pattern(blocks * 4096)` where `pattern` is repeated `sha256` chaining from the
seed `b"neodct"` (`h = sha256(h)`, concatenate, truncate).

| blocks | salt | root hash | hash_blocks | hash area bytes |
| --- | --- | --- | --- | --- |
| 1 | `f8e7d6c5b4a3` | `c5cc54356d1e31644008f7dd370fae60537bd0f1210bf2669401cef03fbe4cb0` | 0 | 4096 |
| 1 | (none) | `5680f46c66c60694d6d13b74989951e91701567b4562c0951db2b9b5ec90ee02` | 0 | 4096 |
| 2 | `f8e7d6c5b4a3` | `67787581d204cb872f5dfd49ff1b9d19b01ef03de1cc38b7d7b6a747fd0ad68f` | 1 | 8192 |
| 2 | (none) | `025577dd3cd28bf3727f2ea81096f9e1e0a3622f6415a5205a22be6ee343df4a` | 1 | 8192 |
| 128 | `f8e7d6c5b4a3` | `2acc16db780f2438051083c63619b72ca2cc26a45aa8ba3d21ed28d7e3915087` | 1 | 8192 |
| 128 | (none) | `f440b22a57a2294572b768f16bd4518a307f3e76af808940230e55da859255d9` | 1 | 8192 |
| 129 | `f8e7d6c5b4a3` | `53ec2f8e44c1225c7de66ea85cc746eca28723f8b49cda4ed0e1e18fa03b9d99` | 3 | 16384 |
| 129 | (none) | `41c4f9acad4084591d573abb22a562f1f09a33d8df01b979e41329b42adb6c13` | 3 | 16384 |
| 300 | `f8e7d6c5b4a3` | `9cd73ab972abc7c21475d94a0e82d6cf8fe68a3ed5ee8536e2695f3a15830ed3` | 4 | 20480 |
| 300 | (none) | `5dcdf1b3b4f6806cd2f6c52ad6af67b3ca797ec7c292b3fdfc205fc8ac85dd42` | 4 | 20480 |

### 9. The build side (`mkupdate.py`) — the format the C must be able to read

Even though `mkupdate.py` stays Python, the C reader is defined by what it writes.

```python
BLOCK_SIZE = 4096
SALT_BYTES = 32            # os.urandom(32) unless --salt
PNG_MAGIC  = b"\x89PNG\r\n\x1a\n"
```

- `build_system_image(squashfs)`: pad the squashfs with zeros to a 4096 boundary, build the
  hash tree over the **padded** bytes, return `padded + format_hash_area(tree)`.
- `manifest.sha256` = SHA-256 of the **whole** image (padded squashfs + superblock + tree).
- `manifest.verity.image_blocks` = number of **padded squashfs** blocks; `block_size` =
  4096; `salt` = hex of the 32 random bytes.
- `encode_manifest(body)` = `json.dumps(body, indent=2, sort_keys=True) + "\n"`, encoded
  UTF-8 with `ensure_ascii=True`. **These exact bytes are what gets signed and what gets
  stored.** Keys therefore appear alphabetically, one per line — which is what lets
  `ndsys-recovery.sh` pull fields out with a line-oriented `sed` and no JSON parser.
- `sign_manifest(raw, key)` shells out to `openssl dgst -sha256 -sign <key>` with the bytes
  on stdin. No key → the package is written unsigned, and the phone shows BAD SIGNATURE.
- Thumbnail: must start with the PNG magic and be at most `package.MAX_THUMBNAIL_BYTES`,
  else the build exits. Its SHA-256 is inserted into the manifest **inside `write_ndsw`**,
  before encoding, so the bytes written and the bytes signed cannot disagree.
- `changelog_section(text, version)` pulls the block of `CHANGELOG.txt` under a bare
  `version` heading and stops at the next line that is non-empty, starts with a digit, and
  contains no space.
- Without `--output`, a copy is archived at
  `<images>/packages/UPDATE-<platform>-<version>.ndsw` — the same name a release asset
  wants, which is what `remote.asset_name()` looks for.
- `--image-only` writes `system.img` + `system.manifest.json`, and with `--installed-prop
  DIR` also seeds `installed.prop` so a factory image can build its verity table on first
  boot.

`mkbadupdate.py` produces the 11 deliberately-broken variants used to exercise every
refusal by hand (`unsigned`, `wrong-key`, `tampered-manifest`, `corrupt-image`,
`truncated-image`, `no-manifest`, `no-image`, `not-a-zip`, `wrong-platform`,
`future-kernel`, `bad-root-hash`). `docs/TESTING_UPDATES.md` has the table of what each one
must show on screen. Keep both tools and keep the table true.

### 10. The initramfs applier (`ndsys-apply.sh`) — unchanged, documented because the C writes its input

Environment the caller sets before calling `apply_pending`:
`MNT_USER` (default `/mnt/user`), `STATE_DIR` (default `$MNT_USER/.ndsys`), `LOG_FILE`
(default `$MNT_USER/logs/update.log`), `DM_NAME` (default `ndsys`), `MAX_ATTEMPTS`
(default 3), `SYS_DEV`, `USER_MOUNTED`, `MNT_SDCARD` (default `/mnt/sdcard`),
`NDSYS_CARD_FSTYPES` (default `vfat exfat ext4 ext3 ext2`).

Device identification, in strict priority order (`find_system_device`):

1. **Disk serial `NDSYS`** — scan `/sys/block/*/serial`, exact match, node must exist in
   `/dev`. This outranks everything, including a squashfs found somewhere it does not
   belong (an SD card someone wrote an image onto), and it is *all there is* once recovery
   has wiped the image.
2. squashfs magic — the hint from `neodct.sys=`, then the scan glob
   (`/dev/vd[a-z] /dev/vd[a-z][0-9] /dev/mmcblk[0-9] /dev/mmcblk[0-9]p[0-9] /dev/sd[a-z]
   /dev/sd[a-z][0-9] /dev/ubiblock*`). The magic test is literally
   `[ "$(dd if=DEV bs=4 count=1)" = "hsqs" ]`.
3. a partition labelled `NDSYS` via `blkid`
4. the cmdline hint if it is a block device
5. `$SYS_DEV` if `installed.prop` exists and it is a block device

`find_user_device`: serial `NDUSER`, then `LABEL="NDUSER"`, then the hint if it is a block
device and **not** a squashfs, then a literal `ubiN:volume` spelling taken as-is.

`apply_pending()`:

```
[ -r $STATE_DIR/pending.prop ] || return 0
package     = basename(getprop package)
image       = $STATE_DIR/basename(getprop image)
image_bytes = getprop image_bytes
want_sha    = getprop sha256
version     = getprop version
attempts    = getprop attempts (default 0)

if image_bytes or want_sha empty:
    record_result failed "$version" "incomplete staging record"; clear_pending; return

if package:
    mount_card || { log "update $version waits for the card it is on"; return }   # NOT a failure
    image = find_package "$package"        # looks in $MNT_SDCARD/update then $MNT_SDCARD
    [ -z "$image" ] && { log "$package is not on this card; waiting"; umount_card; return }
elif image missing:
    record_result failed "$version" "incomplete staging record"; clear_pending; return

if attempts >= MAX_ATTEMPTS(3):
    record_result failed "$version" "gave up after $attempts attempts"; clear_pending; return

sed -i-style increment of attempts, then sync

actual_size = package ? unzip -l size of rootfs.squashfs : wc -c of image
if actual_size != image_bytes:
    record_result failed "$version" "staged image truncated"; umount_card; clear_pending; return

got_sha = package ? sha256 of (unzip -p pkg rootfs.squashfs)
                  : sha256 of the first image_bytes bytes of the file
if got_sha != want_sha:
    record_result failed "$version" "image sha256 mismatch before write"
    umount_card; clear_pending; return

write:  package ? unzip -p pkg rootfs.squashfs | dd of=$SYS_DEV bs=1M conv=fsync
                : dd if=$image of=$SYS_DEV bs=1M conv=fsync
    failure -> log "write to $SYS_DEV failed; retrying on the next boot"; return   # keep pending
sync; umount_card

read back: sha256 of the first image_bytes bytes of $SYS_DEV
    mismatch -> log "read-back mismatch on $SYS_DEV; retrying on the next boot"; return

write installed.prop { sha256, image_bytes, version, buildtime, platform,
                       verity_root_hash, verity_block_size, verity_image_blocks, verity_salt }
clear_pending
record_result ok "$version" "installed"
```

`hash_prefix FILE BYTES` is `dd bs=4096 count=$((BYTES/4096)) | sha256sum` — **integer
division**, so `image_bytes` must be a whole number of 4096-byte blocks or the read-back
hash covers less than was written. It always is: the image is padded squashfs + a hash area
that is a whole number of 4096-byte blocks.

Records are read with `sed`, **never sourced**, so a changelog full of backticks cannot
execute. There is a test that appends `version=0.3.2a$(rm -f canary)` and asserts the
canary survives. **This property must not be lost**: whatever the C writes, it must never
write a value containing a newline (already enforced) and the shell must never gain a
`source`/`eval`.

The `[ndsys]` log lines go to `/dev/console` and are appended to
`/mnt/user/logs/update.log` with a `[YYYY-MM-DD HH:MM:SS]` prefix.

### 11. The initramfs `init` and recovery — unchanged, summarised

`init` boot order: mount `/proc` `/sys` `/dev` → parse `neodct.sys=`, `neodct.user=`,
`neodct.verity=` (`enforce`|`permissive`|`off`, default `enforce`), `neodct.debug=`,
`neodct.recovery=`, `neodct.rectty=` → boot logo → resolve devices → mount the user
partition rw (ext4, or ubifs for a `ubiN:volume` spelling) → **recovery if requested** →
`apply_pending` → build the dm-verity device (`dmsetup create ndsys --readonly --table
"$(verity_table)"`) → mount the result read-only → write
`$STATE_DIR/verity_state.prop` (`state=`, `mode=`, `sys_device=`, `user_device=` — read
later by `System/hw/neodct-sdcard` so it cannot mistake the system image for a card) →
`mount --move` the user partition into `/NeoDCT/User` in the new root → move `/dev`,
`/proc`, `/sys` → `panel_stop` → `switch_root`.

`enforce` + verity failure → recovery. `permissive` → boots unverified with a warning and
`state=failed`. `off` → skips verity entirely.

⚠️ **This is an integrity guarantee, not an authenticity one.** The root hash comes from
`installed.prop` on the writable user partition. Anyone able to rewrite that partition
could rewrite the system partition too. The authenticity check is the signature, done by
the Update app **before** staging. The upgrade path is
`CONFIG_DM_VERITY_VERIFY_ROOTHASH_SIG` with the release certificate built into the kernel;
the comment in `init` says so. Do not let the C port make this sound stronger than it is.

Recovery (`ndsys-recovery.sh`): 30x10 text menu (8x16 font on the 240x175 panel) on
`/dev/tty1`, falling back to `/dev/console`, overridable with `neodct.rectty=`. Entered by
`neodct.recovery=1`, by the one-shot flag file `$STATE_DIR/boot_recovery` (written by the
Power app; **consumed as it is read** so recovery cannot loop), or automatically when the
system will not boot. Menu: `update system`, `wipe user data`, `wipe system`, `reboot`,
`shell`. Destructive choices default to "no".

`recovery_install_package NDSW SYSDEV [STATEDIR]`:
- pull `sha256`, `version`, `platform`, `buildtime`, `root_hash`, `block_size`,
  `image_blocks`, `salt` out of `manifest.json` with a line-oriented `sed` (which works
  only because `mkupdate` writes `indent=2, sort_keys=True`)
- `image_bytes` from `unzip -l`
- missing `sha256`, `root_hash` or `image_bytes` → refuse
- **pass 1**: hash the image straight out of the zip, before anything is written
- **pass 2**: `unzip -p | dd of=DEV bs=1M conv=fsync`
- **pass 3**: read back `hash_prefix DEV image_bytes` and compare
- write `installed.prop` (with `verity_block_size` defaulting to `4096` if the manifest
  did not say), and remove `pending.prop` / `pending.img`

**Recovery cannot check the signature** — no crypto in the initramfs. The confirmation
says so out loud: `"Install <name>? Signature is NOT checked here."` It is an integrity
check gated on physical possession of the phone. Do not weaken it into something that
looks like a signature check, and do not remove the wording.

`wipe user data` deletes everything under `/mnt/user` **except `.ndsys`** — deleting that
would take `installed.prop` with it, leaving the next boot with no root hash and landing
straight back in recovery. It then recreates `db logs .pycache sdcard tones wallpapers`.

`wipe system` zeroes the first 1 MiB of the system device (instant, and enough to make it
unmountable), which is exactly why the disk-serial lookup exists.

---

## Public interface (the functions other parts call)

```c
/* ---------------- nd_update.h : the shared error taxonomy ---------------- */

typedef enum {
    ND_UPDATE_OK = 0,
    ND_UPDATE_INVALID,        /* InvalidUpdate      — structurally broken. Dead end.      */
    ND_UPDATE_BADSIG,         /* BadSignature       — engineering mode may override.      */
    ND_UPDATE_INCOMPATIBLE,   /* IncompatibleUpdate — NEVER overridable. Brick case.      */
    ND_UPDATE_NOSPACE,        /* package.NotEnoughSpace                                   */
    ND_UPDATE_NETWORK,        /* remote.NetworkError                                      */
    ND_UPDATE_NORELEASE,      /* remote.NoRelease                                         */
    ND_UPDATE_ERROR,          /* bare UpdateError                                         */
} nd_update_err;

/* Every call that can fail fills this. Messages are the Python strings verbatim,
 * because they are shown to the user on the WRONG UPDATE / staging screens. */
#define ND_UPDATE_MSG_MAX 256
typedef struct { nd_update_err code; char msg[ND_UPDATE_MSG_MAX]; } nd_update_status;

const char *nd_update_strerror(const nd_update_status *st);

/* ---------------- nd_rsa.h : signing.py ---------------- */

typedef struct nd_rsa_pubkey nd_rsa_pubkey;      /* opaque; holds n, e, size */

nd_rsa_pubkey *nd_rsa_load_public_key(const uint8_t *data, size_t len);  /* PEM|DER, SPKI|PKCS#1 */
void           nd_rsa_pubkey_free(nd_rsa_pubkey *key);
size_t         nd_rsa_pubkey_size(const nd_rsa_pubkey *key);             /* modulus bytes */

/* True only if `sig` is a valid PKCS#1 v1.5 / SHA-256 signature over data. Never throws. */
bool nd_rsa_verify_sha256(const uint8_t *data, size_t data_len,
                          const uint8_t *sig,  size_t sig_len,
                          const nd_rsa_pubkey *key);

bool nd_rsa_verify_detached(const char *path, const char *sig_path, const char *key_path);

/* ---------------- nd_manifest.h : manifest.py ---------------- */

typedef struct {
    char     version[64];
    int64_t  buildtime;
    char     platform[64];
    char     sha256[65];              /* verbatim, case preserved */
    char    *changelog;               /* owned; "" never NULL */
    char     min_kernel[64];
    char     thumbnail_sha256[65];
    char     verity_root_hash[65];
    uint32_t verity_block_size;
    uint64_t verity_image_blocks;
    char     verity_salt[513];        /* hex, "" when absent */
    uint8_t *raw;  size_t raw_len;    /* owned; the exact signed bytes. NEVER re-encode. */
} nd_manifest;

nd_update_err nd_manifest_parse(const uint8_t *raw, size_t len,
                                nd_manifest **out, nd_update_status *st);
void          nd_manifest_free(nd_manifest *m);
uint64_t      nd_manifest_hash_offset(const nd_manifest *m);   /* == image_bytes */
uint64_t      nd_manifest_image_bytes(const nd_manifest *m);
nd_update_err nd_manifest_check_compatible(const nd_manifest *m, const char *platform,
                                           const char *kernel, nd_update_status *st);

/* ---------------- nd_package.h : package.py ---------------- */

typedef struct nd_package nd_package;
typedef void (*nd_progress_fn)(uint64_t done, uint64_t total, void *user);

nd_update_err nd_package_open(const char *path, nd_package **out, nd_update_status *st);
void          nd_package_close(nd_package *pkg);
const nd_manifest *nd_package_manifest(const nd_package *pkg);
const char   *nd_package_path(const nd_package *pkg);
uint64_t      nd_package_image_size(const nd_package *pkg);    /* uncompressed member size */
bool          nd_package_is_signed(const nd_package *pkg);

nd_update_err nd_package_verify_signature(nd_package *pkg, const char *key_path,
                                          nd_update_status *st);
/* *out == NULL with ND_UPDATE_OK means "this package has no thumbnail". Caller frees. */
nd_update_err nd_package_read_thumbnail(nd_package *pkg, uint8_t **out, size_t *len,
                                        nd_update_status *st);
nd_update_err nd_package_extract_image(nd_package *pkg, const char *dest,
                                       nd_progress_fn progress, void *user,
                                       int64_t free_bytes /* <0 = statvfs */,
                                       nd_update_status *st);

/* ---------------- nd_staging.h : staging.py ---------------- */

typedef struct nd_record nd_record;               /* parsed KEY=value + its state_dir */

nd_update_err nd_stage(const nd_manifest *m, const char *image_path,
                       const char *state_dir, nd_record **out);
nd_update_err nd_stage_package(const nd_manifest *m, const char *package_path,
                               uint64_t image_bytes, const char *state_dir,
                               nd_record **out);

nd_record *nd_read_pending(const char *state_dir);        /* NULL = nothing usable */
void       nd_clear_pending(const char *state_dir);
int        nd_note_attempt(const char *state_dir);        /* new total, -1 on failure */

nd_update_err nd_record_installed(const nd_manifest *m, const char *state_dir,
                                  int64_t image_bytes /* <0 -> "" */);
nd_record *nd_read_installed(const char *state_dir);

nd_update_err nd_record_result(const char *state_dir, const char *result,
                               const char *version, const char *reason);  /* NULL = omit */
nd_record *nd_read_result(const char *state_dir);
void       nd_clear_result(const char *state_dir);

void        nd_record_free(nd_record *r);
const char *nd_record_get(const nd_record *r, const char *key);  /* raw value or NULL */
const char *nd_record_image(const nd_record *r);       /* resolved against state_dir */
const char *nd_record_package(const nd_record *r);     /* basename, "" if none */
bool        nd_record_from_package(const nd_record *r);
uint64_t    nd_record_image_bytes(const nd_record *r);
int         nd_record_attempts(const nd_record *r);
bool        nd_record_exhausted(const nd_record *r);   /* attempts >= 3 */
uint64_t    nd_record_hash_offset(const nd_record *r);

/* ---------------- nd_remote.h : remote.py ---------------- */

typedef struct {
    char     version[64];
    char     tag[64];
    char     url[512];
    uint64_t size;
    char    *notes;        /* owned */
    bool     prerelease;
} nd_release;

const char *nd_remote_repo(void);                              /* env override */
void        nd_remote_asset_name(const char *platform, char *out, size_t n);

nd_update_err nd_remote_all_releases(const char *platform, int limit,
                                     nd_release **out, size_t *count,
                                     nd_update_status *st);
nd_update_err nd_remote_latest(const char *platform, nd_release *out, nd_update_status *st);
void          nd_release_list_free(nd_release *list, size_t count);

void nd_verkey_parse(const char *text, nd_verkey *out);
int  nd_verkey_cmp(const nd_verkey *a, const nd_verkey *b);
bool nd_remote_is_newer(const char *candidate, const char *installed);
bool nd_remote_enough_space(const char *directory, uint64_t size);

nd_update_err nd_remote_download(const char *url, const char *destination,
                                 uint64_t size, nd_progress_fn progress, void *user,
                                 int attempts, uint64_t *written, nd_update_status *st);

/* Test seam: the suite must be able to run without a network and without sleeping. */
void nd_remote_set_sleep_fn(void (*fn)(unsigned seconds));
void nd_remote_set_transport(const nd_remote_transport *t);    /* NULL = the real one */

/* ---------------- nd_verity.h : verity.py ---------------- */

typedef struct {
    char     root_hash[65];
    uint8_t *tree;  size_t tree_len;    /* owned; empty when data_blocks == 1 */
    uint32_t levels;
    uint64_t data_blocks;
    uint64_t hash_blocks;
    uint8_t  salt[256]; size_t salt_len;
    uint32_t data_block_size, hash_block_size;
    char     algorithm[16];
} nd_hash_tree;

nd_update_err nd_verity_build(const uint8_t *image, size_t len, const uint8_t *salt,
                              size_t salt_len, uint32_t dbs, uint32_t hbs,
                              nd_hash_tree **out);
void          nd_hash_tree_free(nd_hash_tree *t);
nd_update_err nd_verity_format_hash_area(const nd_hash_tree *t, const uint8_t uuid[16],
                                         uint8_t **out, size_t *len);
nd_update_err nd_verity_parse_superblock(const uint8_t *area, size_t len,
                                         nd_verity_sb *out);
/* The dm table line. MUST match ndsys-apply.sh verity_table() byte for byte. */
nd_update_err nd_verity_dm_table(const nd_hash_tree *t, const char *device,
                                 uint64_t hash_offset, const char *hash_device,
                                 char *out, size_t n);
```

### What the rest of the system calls into this subsystem

| Caller | Uses |
| --- | --- |
| `apps/Update/app.so` | everything |
| `apps/Downgrade/app.so` (engineering) | `nd_remote_all_releases`, `nd_remote_is_newer`, `nd_remote_download`, `nd_remote_asset_name`, and then the Update app's `_install` |
| `apps/Power/app.so` | `ND_STATE_DIR` only, to `touch` `boot_recovery` |
| `System/hw/neodct-sdcard` (shell) | reads `$STATE_DIR/verity_state.prop`, written by `init` |
| `etc/init.d/S00userdata` (shell) | creates `.ndsys` on a fresh user partition |
| `initramfs/ndsys-apply.sh` (shell) | reads `pending.prop`, writes `installed.prop`, `last_result.prop` |

### What this subsystem calls out to

| Needed from | What |
| --- | --- |
| UI framework | `DetailPage`, `MessageDialog`, `ProgressScreen`, `VerticalList`, `SoftKeyBar` with the exact constructor arguments listed in §7 — including `cancel_keys=()` on the dead-end dialogs and `present=false` on the softkey bar |
| Rasterizer | PNG decode for the thumbnail; the page scales it to 64x64 |
| `libneodct` settings | `nd_settings_get("system.os.platform")`, `"system.os.versionnumber"`, `"system.ui.engineering_mode"` |
| `libneodct` storage | `nd_storage_card()`, `nd_storage_folder("update"/"backup_db")`, `nd_storage_find_updates()` |
| libc | `statvfs`, `rename`, `fsync`, `uname`, `gmtime_r`, `localtime_r`, `strftime`, `posix_spawnp` |

The Downgrade app calls the Update app's `_install()` by loading
`/NeoDCT/System/apps/Update/main.py` with `importlib`. In C that becomes a direct call into
`libndupdate.so`: **hoist `_install` out of the app and into the shared library** as
`nd_update_install(ui, path)`, so both apps link the same code and nobody re-implements a
signature check. This is the same intent ("one signature check, one staging path, one
applier") expressed in a way C can actually do.

---

## External dependencies and their C replacements

| Python dependency | Used for | C replacement |
| --- | --- | --- |
| `int` bignum arithmetic (`pow(v, e, n)`) | RSA signature verification | **`mbedtls_mpi_exp_mod()`** — see below. Fallbacks: OpenSSL `BN_mod_exp()`, or a 250-line in-house Montgomery modexp. |
| `hashlib.sha256` | manifest/image/thumbnail digests, verity tree | `mbedtls_sha256()` / `mbedtls_sha256_starts/update/finish`, or OpenSSL `SHA256()` / `EVP_Digest`. Streaming is mandatory for the image. |
| `hmac.compare_digest` | signature block comparison | An OR-accumulating loop over the full length (`ct_eq()` above). Never `memcmp`. |
| `base64.b64decode` | PEM decode | `mbedtls_base64_decode()`, or ~40 lines in-house. |
| `zipfile` | reading `.ndsw` | **`nd_zip.c`** — hand-written central-directory reader (see §1). No libzip: it is another dependency for 500 lines of work, and it does not stream the way we need. |
| `zlib` (inside `zipfile`) | inflating deflated members | `zlib`'s `inflateInit2(&s, -MAX_WBITS)`. Already in the image. |
| `json` | `manifest.json`, GitHub's release list | **`nd_json.c`** — a small recursive-descent reader (see below). Shared with the app-manifest loader in the core loop. |
| `urllib.request` + `ssl` | HTTPS GET with headers, Range, redirects, timeouts | **`libcurl`** (`BR2_PACKAGE_LIBCURL=y` already) or **mbedTLS + hand-rolled HTTP/1.1**. See the recommendation below. |
| `ssl.create_default_context()` | certificate + hostname verification | `CURLOPT_SSL_VERIFYPEER=1`, `CURLOPT_SSL_VERIFYHOST=2`, `CURLOPT_CAINFO="/etc/ssl/certs/ca-certificates.crt"`; or `mbedtls_x509_crt_parse_file()` + `mbedtls_ssl_conf_authmode(MBEDTLS_SSL_VERIFY_REQUIRED)` + `mbedtls_ssl_set_hostname()`. **Never optional.** |
| `re` (`version_key`) | version chunk parsing | Hand-written scanner. The two patterns are `[.\-_]` for splitting and `^(\d+)([A-Za-z]*)$` per chunk — no regex engine needed. |
| `os.statvfs` | free-space checks | `statvfs(3)` |
| `os.replace` | atomic record/download commit | `rename(2)` |
| `os.fsync` on a directory fd | making the rename durable | `open(dir, O_RDONLY)` + `fsync` + `close` |
| `os.urandom(32)` | verity salt (build tool only) | `getrandom(2)` / `/dev/urandom` |
| `shutil.copy2` | database backup to the card | `open`/`read`/`write` loop + `utimensat` + `fchmod` |
| `subprocess.call(["sync"])` | flush before reboot | `sync(2)` — no process needed |
| `subprocess.Popen(["reboot"])` | reboot | `fork` + `execvp`, first of `reboot`, `/sbin/reboot`, `busybox reboot` that execs |
| `time.strftime`/`gmtime` | build date, backup folder name | `gmtime_r`+`strftime` (build date), `localtime_r`+`strftime` (backup folder) |
| `PIL.Image.open` (thumbnail) | decode the release picture | `nd_image_load_png()` from the rasterizer |
| `openssl dgst -sha256 -sign` (build host) | signing | unchanged — the host tool stays Python |
| `veritysetup` (test cross-check) | golden verity vectors | unchanged — host test only |

### The bignum decision, spelled out

`pow(value, key.e, key.n)` where `value` and `n` are 4096-bit and `e` is 65537.

**Recommended: mbedTLS.** Add `BR2_PACKAGE_MBEDTLS=y` (about 400 KB of `libmbedcrypto.so`
+ `libmbedtls.so`, trimmable with a config header). One dependency then covers the bignum,
SHA-256, base64 **and** the TLS for the download — which lets the update path avoid pulling
OpenSSL (≈3 MB of shared objects) into the process at all.

```c
mbedtls_mpi n, e, s, x;
mbedtls_mpi_init(&n); mbedtls_mpi_init(&e);
mbedtls_mpi_init(&s); mbedtls_mpi_init(&x);

mbedtls_mpi_read_binary(&n, modulus_be, modulus_len);
mbedtls_mpi_read_binary(&e, exponent_be, exponent_len);
mbedtls_mpi_read_binary(&s, signature,  key_size);

/* Python's `if value >= key.n: return False` — do this OURSELVES, before the modexp,
 * so the reject set is identical and does not depend on the library reducing. */
if (mbedtls_mpi_cmp_mpi(&s, &n) >= 0) { ok = false; goto done; }

if (mbedtls_mpi_exp_mod(&x, &s, &e, &n, NULL) != 0) { ok = false; goto done; }
mbedtls_mpi_write_binary(&x, encoded, key_size);   /* left zero-padded to key_size */
```

`mbedtls_mpi_write_binary` into a `key_size` buffer is exactly Python's
`.to_bytes(key.size, "big")`.

**Do NOT use `mbedtls_pk_parse_public_key()` + `mbedtls_pk_verify()`.** Two reasons: the
parser rejects the bare PKCS#1 `RSAPublicKey` form that `load_public_key()` accepts, and it
applies its own OID/parameter rules rather than the lax substring search the Python does —
so the set of keys accepted would change. Do the DER by hand and the padding check by hand,
exactly as written in §3.

**Alternative: OpenSSL.** `libcrypto` is already in the image (`BR2_PACKAGE_OPENSSH=y`
selects `BR2_PACKAGE_OPENSSL`). `BN_bin2bn`, `BN_cmp`, `BN_mod_exp(r, a, p, m, ctx)`,
`BN_bn2binpad(r, out, key_size)`, `SHA256()`. Same rule: no `RSA_verify`, no
`EVP_PKEY_verify` — do the reconstruct-and-compare ourselves. The downside is that
`libcrypto.so` is ~3 MB on disk and its RSS while mapped is well over a megabyte, which is
a lot to spend on 17 modular multiplications.

**Alternative: in-house.** ~250 lines. 32-bit limbs in a `uint32_t[132]`, `uint64_t`
accumulator, Montgomery reduction with `n0inv = -n[0]^{-1} mod 2^32` (four Newton
iterations from `x = 1`), `R = 2^(32k)`, `RR = R^2 mod n` computed by repeated doubling.
Square-and-multiply over the bits of `e` from the top. RSA-4096 verification is 16
squarings + 1 multiply of 128-limb numbers ≈ 500k 32-bit multiply-accumulates ≈ **a few
milliseconds on a 500 MHz Cortex-A7** — utterly irrelevant next to hashing a 51 MB image.
Everything here is public data, so constant-time behaviour is not required *for the
modexp*; it is still required for the final comparison. This is the zero-dependency option
and it keeps the "the phone needs no crypto library" property the Python docstring is proud
of. **Worth doing if the 400 KB of mbedTLS is judged too expensive**, and worth writing the
code so the backend is a compile-time choice behind `nd_rsa.c`.

### The HTTPS decision

**Recommended: libcurl**, because `BR2_PACKAGE_LIBCURL=y` is already set, redirects and
Range and chunked encoding are already correct, and hand-rolling HTTP/1.1 against GitHub's
CDN is a category of bug we do not need. Configure it exactly as in §6, and specifically
**do not set `CURLOPT_TIMEOUT`** — use `CURLOPT_LOW_SPEED_LIMIT`/`TIME`.

**Alternative: mbedTLS + ~350 lines of HTTP/1.1.** Smaller resident set, and it makes
mbedTLS the single crypto dependency. Must then handle: absolute-URI redirects (301, 302,
303, 307, 308) with a cap of 10, `Transfer-Encoding: chunked`, `Content-Length`, `206
Partial Content`, and SNI. Only take this if the RAM measurement says libcurl+OpenSSL is
too heavy while the Update app is live.

Either way, put the transport behind a two-function vtable so the test suite can substitute
a scripted responder — the existing 36 remote tests all work by replacing `remote._open`,
and the C tests must be able to do the same thing:

```c
typedef struct {
    /* Returns HTTP status, or <0 with *st filled. Body is streamed to sink(). */
    int (*get)(const char *url, int timeout_s, const char *range_header,
               bool (*sink)(const uint8_t *buf, size_t n, void *user), void *user,
               nd_update_status *st);
} nd_remote_transport;
```

### `nd_json.c` — shared, and someone else may already own it

Requirements from this subsystem:

- objects, arrays, strings, numbers, `true`/`false`/`null`
- **integers must be distinguishable from floats** (`buildtime` must reject `1785160800.0`
  and `"buildtime": "tuesday"`), and **booleans must be a distinct type** (`buildtime`
  explicitly rejects `true`)
- `\uXXXX` escapes decoded to UTF-8, including surrogate pairs (release notes come from
  GitHub and contain arbitrary text)
- duplicate keys: **the last one wins** (Python's `json` behaviour)
- **a hard input cap** — see Risks §R2
- no allocation per token beyond one arena the caller frees

App manifests (`apps/*/manifest.json`) need the same parser, so this belongs in
`libneodct.so` and should be agreed with whoever owns the core-loop spec rather than
written twice.

---

## Proposed C modules

| File | Contents | Est. LOC |
| --- | --- | --- |
| `include/nd_update.h` | `nd_update_err`, `nd_update_status`, member-name constants, `ND_STATE_DIR` and friends | 90 |
| `src/nd_bignum.c` / `.h` | Montgomery modexp over 32-bit limbs. Only built when `ND_CRYPTO_BACKEND=inhouse`. | 260 |
| `src/nd_rsa.c` / `.h` | DER TLV reader, `load_public_key` (PEM/DER × SPKI/PKCS#1), `verify_sha256`, `verify_detached`, `ct_eq`. Backend behind three macros. | 420 |
| `src/nd_json.c` / `.h` | Recursive-descent JSON reader with typed numbers and a size cap. **Shared with the core loop.** | 520 |
| `src/nd_manifest.c` / `.h` | Field validation in the exact order of §2, hex helper, `nd_kver`, `check_compatible` | 300 |
| `src/nd_zip.c` / `.h` | EOCD/Zip64 scan, central directory, local-header offset resolution, stored + deflate streaming readers, CRC-32 | 520 |
| `src/nd_package.c` / `.h` | `open`, `verify_signature`, `read_thumbnail`, `extract_image`, `statvfs` margin | 330 |
| `src/nd_staging.c` / `.h` | `KEY=value` read/write, atomic commit with directory fsync, `nd_record` accessors, `stage`, `stage_package`, `read_pending`, results | 380 |
| `src/nd_remote.c` / `.h` | transport vtable, libcurl backend, release listing, `nd_verkey`, resumable `download` with backoff | 560 |
| `src/nd_verity.c` / `.h` | hash tree, superblock pack/unpack, `dm_table` | 300 |
| `apps/Update/app.c` | every screen, every refusal, `run()`, `_install()` (exported as `nd_update_install`) | 620 |
| `apps/Update/update_online.c` | `_has_network`, `_check_online`, the download progress wiring | 220 |
| `apps/Downgrade/app.c` | the engineering release picker | 210 |
| `tests/unit/test_nd_*.c` | ports of the 156 Python tests listed below | ~1,400 |

**Estimated production C: ~4,730 LOC** (~4,000 excluding `nd_json.c` if the core-loop agent
supplies it). Plus ~1,400 LOC of tests.

Build placement, restated because it is the RAM decision:

```
libneodct.so   nd_staging.o  nd_json.o
libndupdate.so nd_rsa.o nd_bignum.o nd_manifest.o nd_zip.o nd_package.o
               nd_remote.o nd_verity.o
               -> DT_NEEDED: libz, and (libcurl | libmbedtls), libmbedcrypto
Update/app.so, Downgrade/app.so  -> DT_NEEDED: libneodct, libndupdate
core (nd-core)                   -> DT_NEEDED: libneodct only
```

**The core process must not have `libcurl`, `libssl`, `libcrypto` or `libmbedtls` in its
link map.** Verify with `readelf -d nd-core`. That is a build-time assertion worth adding to
CI.

---

## Risks

| # | Risk | Severity | Mitigation |
| --- | --- | --- | --- |
| R1 | **A weakened signature check.** The single worst outcome in the whole port: a C `verify()` that searches for the DigestInfo inside the decrypted block, accepts a short 0xFF run, accepts a SHA-1 OID, or short-circuits the comparison. Any of those makes forged updates installable and the phone would show "Verified". | **high** | Port `test_update_signing.py` first, before writing `nd_rsa.c`, including the Bleichenbacher and SHA-1-substitution forgeries and the fixed openssl-produced `SIG_HEX` vector. Rebuild the whole expected block; `ct_eq` over the full length. Code review by a second agent, specifically against §3. |
| R2 | **Unbounded allocation from network or package input.** `remote.all_releases()` reads GitHub's whole response into memory with no cap; `Package.read("manifest.json")` reads a member whose declared size the package controls; `Image.open` on a thumbnail could be a 10000×10000 PNG that decompresses to 300 MB. Python survives on a desktop and dies on a 53 MB phone. | **high** | Hard caps in C: HTTP response body ≤ 1 MiB (else `NetworkError`), `manifest.json` member ≤ 1 MiB (else `InvalidUpdate`), `manifest.sig` ≤ 1 KiB, thumbnail already capped at 256 KiB by `MAX_THUMBNAIL_BYTES` **plus** a decoded-dimension cap of 512×512 in the PNG loader (a failed decode already falls back to the stock icon, so behaviour is unchanged for every real package). These are deliberate hardening deviations — record them in `OPEN-QUESTIONS.md`. |
| R3 | **The whole system image getting into RAM.** 51 MB into 53 MB usable is an instant OOM kill. Python's `zipfile.open()` streams; a naive C `nd_zip_read_member()` that returns a malloc'd buffer would not. | **high** | `nd_zip.c` exposes **no** whole-member read for `rootfs.squashfs` — only a streaming reader with a caller-supplied buffer. Make `nd_zip_read_all()` refuse any member over 1 MiB. Add a test that opens a 64 MB package under a 16 MB `RLIMIT_AS` and still extracts it. |
| R4 | **`image_bytes` confusion.** `manifest.image_bytes` is the *padded squashfs* (`image_blocks * block_size`). The `pending.prop` `image_bytes` written by `stage_package()` is the *zip member size*, which includes the appended verity hash area. The applier hashes exactly `image_bytes` bytes back off the device. Swap them and the read-back hash covers the wrong range and every install fails — or worse, succeeds against a truncated image. | **high** | The comment in `stage_package()` says this explicitly; carry it into the C verbatim. Name the struct fields differently (`padded_squashfs_bytes` vs `member_bytes`) so they cannot be confused at a call site. `test_the_record_says_how_many_bytes_the_image_is` asserts `image_bytes == len(image) > hash_offset`. |
| R5 | **`CURLOPT_TIMEOUT` instead of `CURLOPT_LOW_SPEED_TIME`.** Python's `timeout=120` is a per-recv socket timeout. libcurl's `CURLOPT_TIMEOUT` is a total deadline and would abort every 53 MB download at two minutes on a carrier link, reproducing the exact bug the resume logic was written to fix. | **high** | Spelled out in §6. Add a test that transfers 5 MB at a throttled rate over more than 120 s through the transport seam and asserts it completes. |
| R6 | **Losing the "records are parsed, never sourced" property.** A future C writer that emits a multi-line value, or a future shell edit that uses `source`, turns a changelog into shell code running as root in the initramfs. | **high** | Keep the newline rejection in `nd_write_record` (it has a test). Keep `test_a_changelog_full_of_shell_metacharacters_cannot_run_commands` running against the C writer. Do not touch `ndsys-apply.sh`. |
| R7 | **Two Python behaviours that look like bugs, and one that is.** (a) A bad CRC-32 on `manifest.json` raises `BadZipFile` out of `open_package()`, past the `InvalidUpdate` mapping, and crashes the Update app instead of showing "INVALID UPDATE". (b) A transport error *mid-body* (TLS error, socket timeout, connection reset) is caught by `except OSError` in `download()`, which **discards the partial file and raises `UpdateError` with no retry** — defeating the resume logic for exactly the failure mode it was written for. Only a *clean* short read (`read()` returning `b""`) takes the retry path. (c) `http.client.IncompleteRead` escapes `download()` entirely. | **medium** | Reproducing (a) and (b) faithfully means shipping known-bad behaviour; fixing them silently breaks "1:1". Recommendation: map CRC failure on `manifest.json` to `ND_UPDATE_INVALID`, and treat *every* mid-body transport failure as `ND_UPDATE_NETWORK` (retry, keep the partial) — both are strictly better and neither changes any passing test. **Add all three to `OPEN-QUESTIONS.md` for the owner rather than deciding unilaterally.** |
| R8 | **Divergent duplicate-key handling.** Python's `_read_record` takes the **last** occurrence of a key; the shell's `getprop` takes the **first**. A record with two `version=` lines therefore means different things to the two readers. | **medium** | Reproduce the Python side exactly (last wins) and document it. The C writer never emits duplicates, so this only matters for hand-edited or appended records — which is precisely what the metacharacter test does. |
| R9 | **`_hashes_per_block()` is dead code and `pack()` disagrees with it for any digest that is not 32 bytes.** A future change to SHA-512 or a 512-byte hash block would produce a tree the kernel rejects, and the failure appears as an unbootable phone. | **medium** | Reproduce the contiguous packing (it is what made the golden vectors), and add a hard `assert`/error for `digest_size != 32 \|\| hash_block_size != 4096`. Keep all ten golden vectors, plus the live `veritysetup` cross-check when the tool is present. |
| R10 | **Locale-dependent date formatting.** `_format_date` uses `%b`. If the C process picks up a non-C locale the month abbreviation changes and every golden frame of the update page differs. | **medium** | `setlocale(LC_ALL, "C")` at process start, or format the month from a static `{"Jan",…}` table. The golden-frame test `test_the_update_page_shows_the_size_and_the_build_date` asserts `"2026"` in the subtitle; the pixel tests are stricter. |
| R11 | **Hex case sensitivity.** `manifest.sha256` is stored verbatim, so an uppercase hex digest in a manifest compares unequal to the lowercase digest the phone computes and the update is refused as corrupt. This is current behaviour. | **low** | Reproduce (do not normalise). Note it in the spec so nobody "fixes" it and changes which packages install. |
| R12 | **`e = 65537` assumed.** Every key in the tree uses it, but the code reads `e` from the DER and a hard-coded exponent would silently accept the wrong key. | **low** | Read `e` from the key. Reject `e < 3` or `e` even, and cap its size at 8 bytes, before the modexp. |
| R13 | **Non-atomic download commit across filesystems.** `rename(partial, destination)` is atomic only within one filesystem. Both are in the card's `update/` folder, so this holds — unless someone "improves" the destination path. | **low** | Assert both paths share a directory. Keep the `.part` suffix so `Storage.find_updates()` (which matches `*.ndsw`) cannot pick up a partial. |
| R14 | **Recovery's confirmation wording.** `"Install <name>? Signature is NOT checked here."` is the only thing telling the owner that this path has no authenticity guarantee. | **low** | It is in shell and stays in shell. Do not paraphrase it if the recovery UI is ever revisited. |

---

## Tests that cover this

**312 test functions across 14 files** (parametrised cases counted once), plus two shared
fixture modules. They import the real overlay code — `conftest.py` puts
`neodct/overlay/NeoDCT` on `sys.path` — so they are testing the shipped implementation, not
a copy.

| File | Tests | LOC | Covers | Usable as a port oracle? |
| --- | --- | --- | --- | --- |
| `test_update_signing.py` | 10 | 167 | DER/PEM key loading (2048 + 4096), a fixed openssl `SIG_HEX` vector, wrong data, wrong length, empty, garbage, **short-padding forgery**, **SHA-1 OID substitution**, live openssl interop | **Yes — the single most important oracle in the subsystem.** Port first, verbatim, before writing `nd_rsa.c`. The `N_HEX`/`D_HEX`/`SIG_HEX`/`PUB_PEM` constants are self-contained and copy straight into C. |
| `test_update_manifest.py` | 22 (+parametrised) | 174 | every required field, every hex rule, non-power-of-two block size, non-numeric buildtime, malformed JSON, non-object JSON, platform mismatch, kernel gate both ways, thumbnail hash rules, **raw bytes preserved verbatim** | **Yes.** Pure data in, exception out. One-to-one with `nd_manifest_parse`. |
| `test_update_package.py` | 19 | 209 | not-a-zip, no manifest, no image, missing file, correct signature, signature over different bytes, unsigned vs invalid distinction, missing key file, extraction + hash, progress monotonicity, tampered image leaves nothing behind, `NotEnoughSpace`, thumbnail present/absent/undeclared/mismatched/oversized | **Yes.** Needs the C to be able to build a `.ndsw` in a test; port `update_fixtures.make_ndsw` to a C helper (~120 LOC) or shell out to Python in the host test. |
| `test_update_staging.py` | 20 | 236 | every recorded field, image moved not copied, record-is-the-commit-point ordering, truncated record ignored, clear, re-stage, attempt counting, 3-attempt exhaustion, installed record, result record, `STATE_DIR` under `/NeoDCT/User/`, **shell metacharacters round-trip**, **newlines refused**, **image path resolved against `state_dir`** | **Yes.** Filesystem in, filesystem out. Direct port. |
| `test_update_remote.py` | 36 | 527 | asset naming, platform selection, `v`-stripping, JSON garbage, non-list reply, `is_newer` incl. 0.3.10a > 0.3.9a, complete/truncated download, partial kept, progress, space refusal, repo override, `all_releases`, **not `/releases/latest`**, prereleases counted, newest *version* not newest publication, **resume with Range**, server ignoring Range, over-long partial discarded, giving up keeps progress, backoff `[5,10,20]`, backoff cap, first attempt does not wait, DNS failure retried | **Yes, with a transport seam.** Every test replaces `remote._open`; the C must expose `nd_remote_set_transport()` for the same trick. Also replaces `remote._sleep` — hence `nd_remote_set_sleep_fn()`. |
| `test_update_verity.py` | 9 (+10 parametrised goldens) | 151 | the ten golden root hashes, single-block special case, non-block-multiple refusal, hash area layout, superblock round-trip, **live `veritysetup` byte-for-byte cross-check**, dm table, dash for an empty salt | **Yes — the strongest oracle.** The goldens came from `veritysetup` 2.8.6; copy them into the C test verbatim. |
| `test_update_flow.py` | 12 | 215 | The whole install on **real drawn pixels** through `uistub.py`: two key presses to install, two screens and a bar, one screen with no card, one when up to date, the release thumbnail reaching the screen, notes scrolling, no text across the progress bar, nothing spilling into the softkey bar, databases backed up, backing out installs nothing, unsigned + engineering mode, unsigned blocked without it | **Yes for logic, and it becomes the golden-frame source.** Capture these frames from the Python build **before** any C is written; the C must reproduce them pixel for pixel. |
| `test_update_ui.py` | 32 | 449 | The widgets themselves on real pixels — progress bar geometry and fill, label above / reading below, step changes without a new screen, no repaint for the same percentage, DetailPage hero row, scrolling, scrollbar, centring, ellipsised long titles | Belongs to the **UI framework** spec, not this one, but this subsystem is its only real consumer. Coordinate. |
| `test_systemupdate_app.py` | 48 | 749 | App policy with the widgets recorded rather than drawn: every refusal string, every softkey label, every `cancel_keys=()`, engineering-mode override paths, the update page contents, backup behaviour, progress screen count, the no-card / not-ready / empty-card screens, last-result reporting, **the online-check offer gated on `_has_network()`**, and — critically — **that nothing image-sized is written to the user partition** and **that the record names the package, not a copy**. Ends with a genuine end-to-end test that stages with the app and installs with the real busybox applier. | **Yes.** The `Recorder` pattern (fake widget classes capturing constructor arguments) ports directly to C with function pointers, and it is exactly how the C app tests should be written. |
| `test_initramfs_apply.py` | 34 | 575 | The shell applier with regular files standing in for block devices: write, clear, record installed, report success, no-op when nothing is staged, hash mismatch, truncated image, failed write keeps the pending update, retry succeeds, attempt counting, give up after three, **shell metacharacters inert**, verity table layout, **verity table == the Python `dm_table()`**, dash salt, device identification by magic and by serial (including after a wipe), image path resolution, and the whole install-from-card path incl. missing card and a swapped card | **Yes, unchanged.** These test shell that is not being ported — but `test_the_verity_table_matches_what_the_python_side_computes` must be re-pointed at the C `nd_verity_dm_table()`, and the staging tests must be re-pointed at the C writer. Both are one-line changes. |
| `test_initramfs_recovery.py` | 37 | 540 | Recovery: good install, records for the next boot, corrupt image refused without touching the device, no manifest, not a zip, unsigned installable, failed write records nothing, pending cleared, manifest fields read out of **real `mkupdate` output**, member size from the zip listing, flag file entry and consumption, tty selection, menu navigation and wrapping, digit selection, where it draws, byte-stream key parsing, `stty` handling, real-VT behaviour | Unchanged. Confirms the shell contract the C writes into. |
| `test_initramfs_applets.py` | 3 | 124 | Every command the initramfs scripts call has a busybox symlink in `mkinitramfs.APPLETS` | Unchanged. |
| `test_mkupdate.py` | 17 | 270 | The producer side, closing the loop: tree appended after the squashfs, padding, empty image refused, manifest points where the tree actually is, **a package `mkupdate` writes is one the phone accepts**, signed/unsigned/wrong-key, changelog section extraction, version read from the target tree, thumbnail travels and is signed with everything else, non-PNG and oversized refused | **Yes.** Even with `mkupdate.py` staying Python, these must be re-run against the **C** reader — that is what stops the producer and consumer drifting apart. |
| `test_mkbadupdate.py` | 17 | 235 | Each of the 11 broken variants still triggers its specific error in the real `UpdateService` code | **Yes**, re-pointed at the C. This is the refusal-taxonomy regression suite. |

Fixtures: `update_fixtures.py` (154 LOC — the throwaway 2048-bit test key with its private
exponent, `raw_sign`, `sign`, `pattern`, `build_image`, `png`, `make_ndsw`) and
`update_ui_fixtures.py` (182 LOC — a whole staged phone with a card, a package and the
matching public key, driving the real UI through `uistub.py`). Both need C equivalents; a
faithful `make_ndsw` in C is the single highest-value test helper in this subsystem.

Golden frames: run the Python build with `NEODCT_UI_SHOTS=/some/dir python -m pytest
neodct/tests/test_update_flow.py neodct/tests/test_update_ui.py` **before writing any C**.
That produces the reference PNGs (`install-00`…, `page-with-release-art`, `progress-45`,
`page-hero`, `unsigned-blocked`, …) that make "one-to-one" something a machine checks.

---

## How this could be split across agents

The subsystem divides cleanly along dependency lines. **Everything below the app can be
built and tested in parallel by five agents on day one**, because every module's interface
is fully specified above and none of them call each other except downward.

### Wave 1 — five independent agents, no shared code

| Agent | Owns | Depends on | Oracle |
| --- | --- | --- | --- |
| **A — crypto** | `nd_bignum.c`, `nd_rsa.c` | libc only (or mbedTLS) | `test_update_signing.py`, all 10 tests, ported verbatim. Self-contained vectors. |
| **B — containers** | `nd_zip.c` | zlib | Round-trip against Python-produced zips; a fuzz target on the central directory. |
| **C — JSON + manifest** | `nd_json.c`, `nd_manifest.c` | libc only | `test_update_manifest.py`, 22 tests, pure data in / error out. **Coordinate `nd_json.h` with the core-loop agent before writing it** — app manifests need the same parser. |
| **D — records** | `nd_staging.c` | libc only | `test_update_staging.py` (20) plus the staging half of `test_initramfs_apply.py`, which drives the real busybox applier against the C writer. |
| **E — verity** | `nd_verity.c` | a SHA-256 (agree the header with A on day one) | `test_update_verity.py`, 10 golden vectors + the live `veritysetup` cross-check. Also owns the assertion that `nd_verity_dm_table()` matches `ndsys-apply.sh:verity_table()`. |

Agent A and Agent E both need SHA-256. Settle that in the first hour: one
`nd_sha256_init/update/final` in `nd_rsa.h` or its own two-file module, chosen by whichever
crypto backend is picked. Do not let two agents write it.

### Wave 2 — two agents, once Wave 1's headers exist

| Agent | Owns | Depends on |
| --- | --- | --- |
| **F — package** | `nd_package.c` | B (zip), C (manifest), A (rsa) |
| **G — network** | `nd_remote.c`, the transport vtable and its libcurl backend | C (json). Nothing else — it deliberately stops at "there is a file on the card". |

G is the largest single test file (36 tests) and is entirely mockable, so it can start the
moment `nd_json.h` is agreed, without waiting for the parser to be finished.

### Wave 3 — one agent, and it needs the UI framework

| Agent | Owns | Depends on |
| --- | --- | --- |
| **H — the app** | `apps/Update/app.c`, `update_online.c`, `apps/Downgrade/app.c`, and hoisting `_install` into `libndupdate.so` as `nd_update_install()` | F, G, D, plus `DetailPage` / `MessageDialog` / `ProgressScreen` / `VerticalList` / `SoftKeyBar` from the UI framework agent |

H is the only part that cannot start early, because every screen is specified in terms of
widgets that do not exist yet. It is also the part with the most tests (48 policy tests +
12 pixel tests), and the `Recorder` fake-widget pattern in `test_systemupdate_app.py` means
**H can write and pass all 48 policy tests against fake widgets before the real ones
exist** — only the 12 golden-frame tests need the real UI. Recommend exactly that split.

### What must not be split

- **`nd_rsa.c`'s `verify()` and the tests that attack it are one unit of work.** Do not let
  one agent write the verifier and another write its tests; the forgery tests are the
  specification.
- **`nd_staging.c`'s record format and `ndsys-apply.sh` are one contract.** Whoever owns D
  owns keeping `test_initramfs_apply.py` green, including the metacharacter test.
- **`nd_verity_dm_table()` and `verity_table()` are one string.** Agent E owns both ends.

### Suggested order if there is only one agent

1. `nd_rsa.c` + its 10 tests (highest risk, fully self-contained, and finishing it retires
   the scariest part of the port)
2. `nd_verity.c` + the 10 golden vectors (self-contained, and it validates the SHA-256)
3. `nd_json.c` + `nd_manifest.c` + 22 tests
4. `nd_zip.c` + `nd_package.c` + 19 tests
5. `nd_staging.c` + 20 tests, then re-point `test_initramfs_apply.py` at the C
6. `nd_remote.c` + 36 tests behind the transport seam
7. `apps/Update/app.c` against fake widgets + 48 policy tests
8. Golden frames, once the UI framework lands
