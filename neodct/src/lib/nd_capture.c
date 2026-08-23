/* nd_capture.c -- the framebuffer that writes PNGs instead of driving a panel,
 * and the manifest that makes the result comparable with the Python's.
 *
 * This is the C half of the golden-frame oracle. goldenframe.py captures the
 * reference from the Python build; this captures the candidate from the C
 * build; goldenframe.py --compare judges them. For that to mean anything, two
 * things have to agree exactly and neither is negotiable:
 *
 *   THE DIGEST. sha256 over b"<w>,<h>|" and then tightly packed RGB rows --
 *   the pixels, not the PNG file. Two encoders write different bytes for the
 *   same picture; only one of those differences would be a port bug.
 *
 *   THE MANIFEST. compare() reads "frames" and, per frame, name/size/sha256.
 *   It also refuses any manifest whose "text_layout" is not "BASIC", on both
 *   sides, because a reference captured on a host with libraqm describes a
 *   phone that does not exist -- that mistake already cost this project 46
 *   wrong reference frames once.
 *
 * The rest of the file is the bookkeeping that lets a frame be recorded now
 * and named later, because shoot_docs.py drives an app for two hundred frames
 * and then saves frames[-1].
 *
 * SHA-256 is implemented here, statically, rather than pulled in from a
 * library or shared with the update system. It is ninety lines, it has no
 * configuration, and a private copy cannot collide with the one the signature
 * verifier will want to build differently.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "nd_capture.h"
#include "nd_fb.h"
#include "nd_fb_priv.h"
#include "nd_image.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_types.h"
#include "nd_vclock.h"

/* ------------------------------------------------------------------ *
 * SHA-256 (FIPS 180-4)
 * ------------------------------------------------------------------ */

typedef struct {
    uint32_t h[8];
    uint64_t len; /* message length in bytes, for the padding */
    size_t fill;
    uint8_t block[64];
} sha256_ctx;

static const uint32_t SHA256_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

static uint32_t rotr32(uint32_t v, unsigned int n)
{
    return (v >> n) | (v << (32u - n));
}

static void sha256_block(sha256_ctx *c, const uint8_t *p)
{
    uint32_t w[64];
    uint32_t a, b, cc, d, e, f, g, h;
    unsigned int i;

    for (i = 0u; i < 16u; i++)
        w[i] = ((uint32_t)p[i * 4u] << 24) | ((uint32_t)p[i * 4u + 1u] << 16) |
               ((uint32_t)p[i * 4u + 2u] << 8) | (uint32_t)p[i * 4u + 3u];
    for (i = 16u; i < 64u; i++) {
        uint32_t s0 = rotr32(w[i - 15u], 7u) ^ rotr32(w[i - 15u], 18u) ^ (w[i - 15u] >> 3);
        uint32_t s1 = rotr32(w[i - 2u], 17u) ^ rotr32(w[i - 2u], 19u) ^ (w[i - 2u] >> 10);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }

    a = c->h[0];
    b = c->h[1];
    cc = c->h[2];
    d = c->h[3];
    e = c->h[4];
    f = c->h[5];
    g = c->h[6];
    h = c->h[7];

    for (i = 0u; i < 64u; i++) {
        uint32_t S1 = rotr32(e, 6u) ^ rotr32(e, 11u) ^ rotr32(e, 25u);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + SHA256_K[i] + w[i];
        uint32_t S0 = rotr32(a, 2u) ^ rotr32(a, 13u) ^ rotr32(a, 22u);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = cc;
        cc = b;
        b = a;
        a = t1 + t2;
    }

    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
    c->h[5] += f;
    c->h[6] += g;
    c->h[7] += h;
}

static void sha256_init(sha256_ctx *c)
{
    c->h[0] = 0x6a09e667u;
    c->h[1] = 0xbb67ae85u;
    c->h[2] = 0x3c6ef372u;
    c->h[3] = 0xa54ff53au;
    c->h[4] = 0x510e527fu;
    c->h[5] = 0x9b05688cu;
    c->h[6] = 0x1f83d9abu;
    c->h[7] = 0x5be0cd19u;
    c->len = 0u;
    c->fill = 0u;
}

static void sha256_update(sha256_ctx *c, const void *data, size_t n)
{
    const uint8_t *p = data;

    c->len += (uint64_t)n;
    while (n > 0u) {
        size_t take = 64u - c->fill;

        if (take > n)
            take = n;
        memcpy(c->block + c->fill, p, take);
        c->fill += take;
        p += take;
        n -= take;
        if (c->fill == 64u) {
            sha256_block(c, c->block);
            c->fill = 0u;
        }
    }
}

static void sha256_final(sha256_ctx *c, uint8_t out[32])
{
    uint64_t bits = c->len * 8u;
    uint8_t pad[72];
    size_t padlen;
    unsigned int i;

    /* 0x80, then zeros to 56 mod 64, then the length big-endian. */
    padlen = (c->fill < 56u) ? (56u - c->fill) : (120u - c->fill);
    memset(pad, 0, sizeof pad);
    pad[0] = 0x80u;
    for (i = 0u; i < 8u; i++)
        pad[padlen + i] = (uint8_t)((bits >> (56u - 8u * i)) & 0xFFu);
    sha256_update(c, pad, padlen + 8u);

    for (i = 0u; i < 8u; i++) {
        out[i * 4u] = (uint8_t)((c->h[i] >> 24) & 0xFFu);
        out[i * 4u + 1u] = (uint8_t)((c->h[i] >> 16) & 0xFFu);
        out[i * 4u + 2u] = (uint8_t)((c->h[i] >> 8) & 0xFFu);
        out[i * 4u + 3u] = (uint8_t)(c->h[i] & 0xFFu);
    }
}

/* ------------------------------------------------------------------ *
 * The digest goldenframe.py computes
 * ------------------------------------------------------------------ */

nd_err nd_capture_digest(const nd_image *img, char *out, size_t out_sz)
{
    sha256_ctx ctx;
    uint8_t digest[32];
    char header[32];
    nd_image *rgb = NULL;
    const nd_image *use;
    nd_err rc;
    int n;
    unsigned int i;
    static const char HEX[] = "0123456789abcdef";

    if (img == NULL || out == NULL)
        return ND_ERR_INVAL;
    if (out_sz < 65u)
        return ND_ERR_TOOLONG;

    if (img->fmt != ND_PIXFMT_RGB888) {
        /* frame_digest() converts to RGB first. Allocation is fine here: this
         * runs once per saved frame in a host tool, not in the render path. */
        rgb = nd_image_convert(img, ND_PIXFMT_RGB888);
        if (rgb == NULL)
            return ND_ERR_NOMEM;
        use = rgb;
    } else {
        use = img;
    }

    sha256_init(&ctx);

    n = snprintf(header, sizeof header, "%d,%d|", use->w, use->h);
    if (n < 0 || (size_t)n >= sizeof header) {
        rc = ND_ERR_TOOLONG;
        goto done;
    }
    sha256_update(&ctx, header, (size_t)n);

    /* Row by row rather than in one go: an image may carry stride padding,
     * and tobytes() is tightly packed by definition. */
    {
        int32_t y;
        for (y = 0; y < use->h; y++)
            sha256_update(&ctx, use->pixels + (size_t)y * use->stride, (size_t)use->w * 3u);
    }

    sha256_final(&ctx, digest);

    for (i = 0u; i < 32u; i++) {
        out[i * 2u] = HEX[(digest[i] >> 4) & 0x0Fu];
        out[i * 2u + 1u] = HEX[digest[i] & 0x0Fu];
    }
    out[64] = '\0';
    rc = ND_OK;

done:
    nd_image_free(rgb);
    return rc;
}

/* ------------------------------------------------------------------ *
 * The capture object
 * ------------------------------------------------------------------ */

typedef struct {
    char name[ND_CAPTURE_NAME_MAX];
    int32_t w;
    int32_t h;
    char sha[65];
} cap_entry;

struct nd_capture {
    nd_fb *fb; /* the sink framebuffer handed to the UI */

    char dir[ND_PATH_MAX];

    /* The frame ring. Buffers are allocated on first use at the size the UI
     * actually draws and then reused, so a two-hundred-frame Koki capture
     * allocates eight images in total. */
    nd_image **ring;
    size_t ring_cap;
    size_t ring_len;  /* how many slots hold a frame          */
    size_t ring_next; /* where the next frame goes            */

    uint64_t drawn;

    int64_t budget; /* < 0 means unlimited */
    int64_t budget_drawn;
    bool exhausted;

    cap_entry entries[ND_CAPTURE_FRAMES_MAX];
    size_t n_entries;
};

/* mkdir -p, on the ND_ROOT-resolved path.
 *
 * The whole library resolves, including nd_image_save_png(), so a capture
 * directory that did not would be written in one place and its PNGs in
 * another. With NEODCT_ROOT unset -- which is how nd-shoot runs unless a test
 * harness says otherwise -- resolving is a plain copy and --out means exactly
 * what it says. */
static nd_err ensure_dir(const char *path)
{
    char buf[ND_PATH_MAX];
    size_t i;
    size_t len;
    nd_err rc;

    rc = nd_path_resolve(buf, sizeof buf, path);
    if (rc != ND_OK)
        return rc;
    len = strlen(buf);
    if (len == 0u)
        return ND_ERR_INVAL;

    /* Trailing slashes would make the final mkdir a no-op on an empty name. */
    while (len > 1u && buf[len - 1u] == '/')
        buf[--len] = '\0';

    for (i = 1u; i <= len; i++) {
        char saved;

        if (buf[i] != '/' && buf[i] != '\0')
            continue;
        saved = buf[i];
        buf[i] = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
            nd_log_err(ND_LOG_FB, "mkdir %s: %s", buf, strerror(errno));
            return ND_ERR_IO;
        }
        buf[i] = saved;
    }
    return ND_OK;
}

/* The sink. uistub.CapturingFramebuffer.update(), including the copy: the UI
 * redraws onto one long-lived canvas, so keeping a reference would leave every
 * captured frame showing the final screen. */
static nd_err capture_sink(void *ctx, const nd_image *src)
{
    nd_capture *cap = ctx;
    nd_image *slot;

    if (cap == NULL || src == NULL)
        return ND_ERR_INVAL;

    if (cap->budget >= 0) {
        if (cap->budget_drawn >= cap->budget) {
            /* ScriptExhausted. The frame is refused, nothing is recorded, and
             * the clock does NOT advance -- goldenframe.instrument() ticks
             * after a successful update and this was not one. */
            cap->exhausted = true;
            return ND_ERR_BUSY;
        }
        cap->budget_drawn++;
    }

    slot = cap->ring[cap->ring_next];
    if (slot != NULL && (slot->w != src->w || slot->h != src->h)) {
        nd_image_free(slot);
        slot = NULL;
        cap->ring[cap->ring_next] = NULL;
    }
    if (slot == NULL) {
        /* owned by the ring slot; freed by nd_capture_close() or when the UI
         * changes frame size. 240x175 RGB888 is 126,000 bytes. */
        slot = nd_image_new(src->w, src->h, ND_PIXFMT_RGB888);
        if (slot == NULL)
            return ND_ERR_NOMEM;
        cap->ring[cap->ring_next] = slot;
    }

    if (src->fmt == ND_PIXFMT_RGB888) {
        int32_t y;
        for (y = 0; y < src->h; y++)
            memcpy(slot->pixels + (size_t)y * slot->stride, src->pixels + (size_t)y * src->stride,
                   (size_t)src->w * 3u);
    } else {
        /* convert("RGB"), which for RGBA drops alpha without compositing --
         * the same thing Framebuffer.update() does to a non-RGB source. */
        nd_image *rgb = nd_image_convert(src, ND_PIXFMT_RGB888);
        int32_t y;

        if (rgb == NULL)
            return ND_ERR_NOMEM;
        for (y = 0; y < rgb->h; y++)
            memcpy(slot->pixels + (size_t)y * slot->stride, rgb->pixels + (size_t)y * rgb->stride,
                   (size_t)rgb->w * 3u);
        nd_image_free(rgb);
    }

    cap->ring_next = (cap->ring_next + 1u) % cap->ring_cap;
    if (cap->ring_len < cap->ring_cap)
        cap->ring_len++;
    cap->drawn++;

    nd_vclock_advance();
    return ND_OK;
}

nd_err nd_capture_open(nd_capture **out, const char *dir, size_t ring_frames)
{
    nd_capture *cap = NULL;
    nd_err rc;

    if (out == NULL || dir == NULL)
        return ND_ERR_INVAL;
    *out = NULL;

    if (ring_frames == 0u)
        ring_frames = ND_CAPTURE_RING_DEFAULT;

    /* owned by the caller; freed by nd_capture_close() */
    cap = calloc(1u, sizeof *cap);
    if (cap == NULL)
        return ND_ERR_NOMEM;

    cap->budget = -1;
    cap->ring_cap = ring_frames;
    /* owned by cap; freed by nd_capture_close() */
    cap->ring = calloc(ring_frames, sizeof *cap->ring);
    if (cap->ring == NULL) {
        rc = ND_ERR_NOMEM;
        goto done;
    }

    if (nd_strlcpy(cap->dir, dir, sizeof cap->dir) >= sizeof cap->dir) {
        rc = ND_ERR_TOOLONG;
        goto done;
    }

    rc = ensure_dir(cap->dir);
    if (rc != ND_OK)
        goto done;

    /* The geometry is the panel's, exactly as uistub's CapturingFramebuffer
     * reports panel_size for xres/yres and 32 for bpp. Nothing is packed at
     * that depth -- the sink takes the source band whole -- but a caller that
     * asks the capture framebuffer how big the screen is should get the same
     * answer a real one gives. */
    rc = nd_fb_open_sink(&cap->fb, ND_CAPTURE_PANEL_W, ND_CAPTURE_PANEL_H, 32, capture_sink, cap);
    if (rc != ND_OK)
        goto done;

    *out = cap;
    cap = NULL;
    rc = ND_OK;

done:
    if (cap != NULL) {
        free(cap->ring);
        free(cap);
    }
    return rc;
}

void nd_capture_close(nd_capture *cap)
{
    size_t i;

    if (cap == NULL)
        return;

    nd_fb_close(cap->fb);
    if (cap->ring != NULL) {
        for (i = 0u; i < cap->ring_cap; i++)
            nd_image_free(cap->ring[i]);
        free(cap->ring);
    }
    free(cap);
}

nd_fb *nd_capture_fb(nd_capture *cap)
{
    return cap != NULL ? cap->fb : NULL;
}

uint64_t nd_capture_frames_drawn(const nd_capture *cap)
{
    return cap != NULL ? cap->drawn : 0u;
}

const nd_image *nd_capture_recent(const nd_capture *cap, size_t back)
{
    size_t idx;

    if (cap == NULL || back >= cap->ring_len)
        return NULL;

    /* ring_next points at the slot the NEXT frame will use, so the most
     * recent is one before it. */
    idx = (cap->ring_next + cap->ring_cap - 1u - back) % cap->ring_cap;
    return cap->ring[idx];
}

nd_image *nd_capture_device_frame(const nd_capture *cap, size_t back, int32_t panel_w,
                                  int32_t panel_h)
{
    const nd_image *band = nd_capture_recent(cap, back);
    nd_image *panel;

    if (band == NULL)
        return NULL;

    /* owned by the caller; free with nd_image_free(). 240x240 RGB888 is
     * 172,800 bytes. */
    panel = nd_image_new_filled(panel_w, panel_h, ND_PIXFMT_RGB888, ND_BLACK);
    if (panel == NULL)
        return NULL;

    /* Centred, which is what uistub does. The real panel is bottom-aligned by
     * neodct_displayd's --yoff; both are correct for their own purpose and
     * neither is to be changed. Golden comparisons happen at band level. */
    if (nd_image_blit(panel, band, (panel_w - band->w) / 2, (panel_h - band->h) / 2) != ND_OK) {
        nd_image_free(panel);
        return NULL;
    }
    return panel;
}

void nd_capture_set_budget(nd_capture *cap, int64_t frames)
{
    if (cap == NULL)
        return;
    cap->budget = frames;
    cap->budget_drawn = 0;
    cap->exhausted = false;
}

void nd_capture_clear_budget(nd_capture *cap)
{
    if (cap == NULL)
        return;
    cap->budget = -1;
    cap->budget_drawn = 0;
    cap->exhausted = false;
}

bool nd_capture_exhausted(const nd_capture *cap)
{
    return cap != NULL && cap->exhausted;
}

size_t nd_capture_count(const nd_capture *cap)
{
    return cap != NULL ? cap->n_entries : 0u;
}

/* ------------------------------------------------------------------ *
 * Saving
 * ------------------------------------------------------------------ */

static bool name_ok(const char *name)
{
    size_t i;

    if (name == NULL || name[0] == '\0')
        return false;
    for (i = 0u; name[i] != '\0'; i++) {
        if (name[i] == '/' || name[i] == '\\')
            return false;
    }
    return i < ND_CAPTURE_NAME_MAX;
}

nd_err nd_capture_save(nd_capture *cap, const char *name, const nd_image *img)
{
    char path[ND_PATH_MAX];
    nd_image *rgb = NULL;
    const nd_image *use;
    cap_entry *e;
    nd_err rc;
    size_t i;
    int n;

    if (cap == NULL || img == NULL)
        return ND_ERR_INVAL;
    if (!name_ok(name)) {
        nd_log_err(ND_LOG_FB, "capture: bad frame name");
        return ND_ERR_INVAL;
    }
    if (cap->n_entries >= ND_CAPTURE_FRAMES_MAX) {
        nd_log_err(ND_LOG_FB, "capture: more than %d frames", ND_CAPTURE_FRAMES_MAX);
        return ND_ERR_TOOLONG;
    }
    for (i = 0u; i < cap->n_entries; i++) {
        if (strcmp(cap->entries[i].name, name) == 0) {
            nd_log_err(ND_LOG_FB, "capture: duplicate frame '%s'", name);
            return ND_ERR_INVAL;
        }
    }

    if (img->fmt != ND_PIXFMT_RGB888) {
        rgb = nd_image_convert(img, ND_PIXFMT_RGB888);
        if (rgb == NULL)
            return ND_ERR_NOMEM;
        use = rgb;
    } else {
        use = img;
    }

    n = snprintf(path, sizeof path, "%s/%s.png", cap->dir, name);
    if (n < 0 || (size_t)n >= sizeof path) {
        rc = ND_ERR_TOOLONG;
        goto done;
    }

    rc = nd_image_save_png(use, path);
    if (rc != ND_OK) {
        nd_log_err(ND_LOG_FB, "capture: cannot write %s", path);
        goto done;
    }

    e = &cap->entries[cap->n_entries];
    (void)nd_strlcpy(e->name, name, sizeof e->name);
    e->w = use->w;
    e->h = use->h;
    rc = nd_capture_digest(use, e->sha, sizeof e->sha);
    if (rc != ND_OK)
        goto done;

    cap->n_entries++;

done:
    nd_image_free(rgb);
    return rc;
}

nd_err nd_capture_save_recent(nd_capture *cap, const char *name, size_t back)
{
    const nd_image *frame = nd_capture_recent(cap, back);

    if (frame == NULL) {
        nd_log_err(ND_LOG_FB, "capture: no frame %zu back for '%s'", back,
                   name != NULL ? name : "(null)");
        return ND_ERR_NOTFOUND;
    }
    return nd_capture_save(cap, name, frame);
}

/* ------------------------------------------------------------------ *
 * The manifest
 * ------------------------------------------------------------------ */

static int entry_cmp(const void *a, const void *b)
{
    const cap_entry *x = a;
    const cap_entry *y = b;

    return strcmp(x->name, y->name);
}

nd_err nd_capture_write_manifest(const nd_capture *cap)
{
    char logical[ND_PATH_MAX];
    char path[ND_PATH_MAX];
    cap_entry *sorted = NULL;
    FILE *fh = NULL;
    nd_err rc = ND_OK;
    size_t i;
    int n;

    if (cap == NULL)
        return ND_ERR_INVAL;

    n = snprintf(logical, sizeof logical, "%s/manifest.json", cap->dir);
    if (n < 0 || (size_t)n >= sizeof logical)
        return ND_ERR_TOOLONG;
    rc = nd_path_resolve(path, sizeof path, logical);
    if (rc != ND_OK)
        return rc;

    if (cap->n_entries > 0u) {
        /* owned locally; freed before returning. Sorted into a copy so the
         * capture's own insertion order is preserved for a caller that wants
         * to know what it saved in what order. */
        sorted = malloc(cap->n_entries * sizeof *sorted);
        if (sorted == NULL)
            return ND_ERR_NOMEM;
        memcpy(sorted, cap->entries, cap->n_entries * sizeof *sorted);
        qsort(sorted, cap->n_entries, sizeof *sorted, entry_cmp);
    }

    fh = fopen(path, "wb");
    if (fh == NULL) {
        nd_log_err(ND_LOG_FB, "cannot write %s: %s", path, strerror(errno));
        rc = ND_ERR_IO;
        goto done;
    }

    /* json.dump(..., indent=2, sort_keys=True): two-space indent, keys in
     * ASCII order (epoch, frames, seed, text_layout, tick), a space after
     * every colon, and no trailing newline. Matching it exactly is not
     * required by compare() -- it parses -- but a byte-identical manifest is
     * one less thing to wonder about when a diff turns up. */
    fprintf(fh, "{\n");
    fprintf(fh, "  \"epoch\": %.1f,\n", ND_VCLOCK_EPOCH);
    fprintf(fh, "  \"frames\": [");
    if (cap->n_entries == 0u) {
        fprintf(fh, "],\n");
    } else {
        fprintf(fh, "\n");
        for (i = 0u; i < cap->n_entries; i++) {
            fprintf(fh, "    {\n");
            fprintf(fh, "      \"name\": \"%s\",\n", sorted[i].name);
            fprintf(fh, "      \"sha256\": \"%s\",\n", sorted[i].sha);
            fprintf(fh, "      \"size\": [\n");
            fprintf(fh, "        %d,\n", sorted[i].w);
            fprintf(fh, "        %d\n", sorted[i].h);
            fprintf(fh, "      ]\n");
            fprintf(fh, "    }%s\n", i + 1u < cap->n_entries ? "," : "");
        }
        fprintf(fh, "  ],\n");
    }
    fprintf(fh, "  \"seed\": %u,\n", ND_VCLOCK_SEED);
    /* compare() rejects anything else, on the reference side and on ours. */
    fprintf(fh, "  \"text_layout\": \"BASIC\",\n");
    fprintf(fh, "  \"tick\": %.1f\n", ND_VCLOCK_TICK);
    fprintf(fh, "}");

    if (ferror(fh) != 0) {
        nd_log_err(ND_LOG_FB, "short write on %s", path);
        rc = ND_ERR_IO;
    }

done:
    if (fh != NULL && fclose(fh) != 0 && rc == ND_OK) {
        nd_log_err(ND_LOG_FB, "cannot close %s: %s", path, strerror(errno));
        rc = ND_ERR_IO;
    }
    free(sorted);
    return rc;
}
