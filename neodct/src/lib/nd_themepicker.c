/* Theme selection is deferred to reboot. Preview images are safe to browse
 * without mixing a new palette with the current process's cached resources. */

#include <string.h>

#include "nd_app.h"
#include "nd_keycodes.h"
#include "nd_log.h"
#include "nd_settings.h"
#include "nd_theme.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

/* The body of one theme's page: what it is, who made it, and what it brings.
 *
 * Assembled rather than stored so that a theme file does not have to describe
 * itself twice -- the flags were worked out when the directory was read, and
 * an owner deciding between two themes wants to know which one carries icons
 * far more than they want the author's name. */
static void page_body(const nd_theme_info *t, bool active, char *out, size_t out_sz)
{
    char parts[ND_THEME_DESC_MAX + 160];
    size_t n = 0u;

    parts[0] = '\0';
    if (t->desc[0] != '\0')
        n = nd_strlcpy(parts, t->desc, sizeof parts);
    if (n < sizeof parts) {
        char brings[96];
        const char *items[4];
        size_t n_items = 0u;

        if (t->has_icons)
            items[n_items++] = "icons";
        if (t->has_font)
            items[n_items++] = "a typeface";
        if (t->has_wallpaper)
            items[n_items++] = "a wallpaper";
        if (t->has_img)
            items[n_items++] = "status icons";

        brings[0] = '\0';
        if (n_items == 0u) {
            (void)nd_strlcpy(brings, "Colours only.", sizeof brings);
        } else {
            size_t i;

            (void)nd_strlcpy(brings, "Brings ", sizeof brings);
            for (i = 0u; i < n_items; i++) {
                if (i > 0u)
                    (void)nd_strlcat(brings, (i + 1u == n_items) ? " and " : ", ", sizeof brings);
                (void)nd_strlcat(brings, items[i], sizeof brings);
            }
            (void)nd_strlcat(brings, ".", sizeof brings);
        }
        (void)nd_snprintf(out, out_sz, "%s%s%s%s%s%s%s%s", parts, parts[0] != '\0' ? "\n\n" : "",
                          brings, t->author[0] != '\0' ? "\nBy " : "", t->author,
                          t->version[0] != '\0' ? "\nVersion " : "", t->version,
                          active ? "\n\nThis is the look you are using." : "");
        return;
    }
    (void)nd_strlcpy(out, parts, out_sz);
}

/* "2/5", the same badge shape the installer's pages use. */
static void page_badge(size_t i, size_t n, char *out, size_t out_sz)
{
    (void)nd_snprintf(out, out_sz, "%zu/%zu", i + 1u, n);
}

nd_err nd_themepicker_init(nd_themepicker *p, nd_ui *ui)
{
    size_t i;

    if (p == NULL || ui == NULL)
        return ND_ERR_INVAL;
    memset(p, 0, sizeof *p);
    p->ui = ui;
    p->n = nd_theme_list(p->themes, ND_THEME_MAX_FOUND);
    if (p->n == 0u)
        return ND_ERR_NOTFOUND;

    /* What was on when we walked in, so that backing out restores it. The id
     * and not the index: the list is rebuilt on entry and a card pulled since
     * the last visit would make an index mean a different theme. */
    (void)nd_strlcpy(p->entry_id, nd_theme_active()->id, sizeof p->entry_id);
    p->sel = 0u;
    for (i = 0u; i < p->n; i++) {
        if (strcmp(p->themes[i].id, p->entry_id) == 0) {
            p->sel = i;
            break;
        }
    }
    return ND_OK;
}

/* Preview artwork uses its own image; the surrounding UI keeps its current look. */
static nd_err build_page(nd_themepicker *p, size_t i, nd_detailpage *page, char *body,
                         size_t body_sz, char *badge, size_t badge_sz)
{
    const nd_theme_info *t = &p->themes[i];
    char preview[ND_PATH_MAX];
    bool active = strcmp(t->id, p->entry_id) == 0;

    page_body(t, active, body, body_sz);
    page_badge(i, p->n, badge, badge_sz);
    if (!nd_theme_preview_path(t, preview, sizeof preview))
        preview[0] = '\0';

    /* Keeping the current theme also cancels a previously queued change. */
    return nd_detailpage_init(page, p->ui, t->name, NULL, body,
                              preview[0] != '\0' ? preview : NULL, badge, "Theme",
                              active ? "Keep" : "Use on reboot");
}

int32_t nd_themepicker_show(nd_themepicker *p)
{
    if (p == NULL || p->n == 0u)
        return ND_WIDGET_BACK;

    for (;;) {
        nd_detailpage page;
        char body[ND_THEME_DESC_MAX + 256];
        char badge[16];
        int32_t key;

        if (build_page(p, p->sel, &page, body, sizeof body, badge, sizeof badge) != ND_OK) {
            nd_log_err(ND_LOG_UI, "theme picker: could not lay out %s", p->themes[p->sel].name);
            goto restore;
        }

        /* * and # leave the page the way ENTER and C do, so that nd_detailpage
         * hands them back instead of swallowing them as scroll keys. The page
         * is then rebuilt around the new theme -- a rebuild and not a redraw,
         * because the body text and the picture are different. */
        page.accept_keys[page.n_accept++] = ND_KEY_STAR;
        page.accept_keys[page.n_accept++] = ND_KEY_HASH;

        key = nd_detailpage_show(&page);
        nd_detailpage_free(&page);

        if (nd_app_should_exit())
            goto restore;

        if (key == ND_KEY_STAR) {
            p->sel = (p->sel == 0u) ? p->n - 1u : p->sel - 1u;
            continue;
        }
        if (key == ND_KEY_HASH) {
            p->sel = (p->sel + 1u) % p->n;
            continue;
        }
        if (key == ND_KEY_ENTER) {
            const nd_theme_info *t = &p->themes[p->sel];

            if (nd_theme_select(t->id) != ND_OK) {
                nd_msgdialog error;

                nd_log_err(ND_LOG_UI, "theme picker: could not save %s", t->id);
                nd_msgdialog_init(&error, p->ui, "Could not save theme.\nCheck the memory card\nand try again.");
                (void)nd_msgdialog_show(&error);
                goto restore;
            }
            nd_log(ND_LOG_UI, "Theme queued for reboot: %s (%s).", t->name, t->id);
            return (int32_t)p->sel;
        }
        goto restore;
    }

restore:
    return ND_WIDGET_BACK;
}
