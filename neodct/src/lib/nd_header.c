/* nd_header.c -- HeaderWidget, the "1-4" breadcrumb in the top-right corner.
 *
 * "1" is which app you are in, "4" is which item of its list you are on. It is
 * two lists' worth of chrome and nothing else: VerticalList and PagedList are
 * the only callers, and no app ever constructs one.
 *
 * ============ THE ROOT ID IS A STRING ============
 *
 * Callers pass both plain integers (VerticalList's app_id, default 99) and
 * compound ids that were never numbers ("5-5" from the Call Log, "1-6" from
 * the phonebook's sub-menus). Python's "%s" formats both without padding, so
 * the C keeps a char[16] and formats the sub-index with %d.
 *
 * ============ WHY width() EXISTS AT ALL ============
 *
 * The breadcrumb is right-aligned on the same row as a list's title and
 * nothing used to stop the title running underneath it -- "Remote Shell"
 * against a "9007-7" counter came out as "Remote Sh<overlap>7-7". width()
 * reports the ink width PLUS its 5 px right margin so a caller can reserve the
 * space; the 5 is part of the number on purpose.
 */

#include <stdio.h>

#include "nd_draw.h"
#include "nd_font.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

void nd_header_init(nd_header *h, nd_ui *ui, const char *root_id)
{
    if (h == NULL)
        return;
    h->ui = ui;
    (void)nd_strlcpy(h->root_id, root_id != NULL ? root_id : "", sizeof h->root_id);
}

void nd_header_init_int(nd_header *h, nd_ui *ui, int32_t root_id)
{
    char buf[16];

    /* Python's "%s" % 99 -- no padding, no sign for positives. */
    (void)snprintf(buf, sizeof buf, "%d", (int)root_id);
    nd_header_init(h, ui, buf);
}

void nd_header_text_for(const nd_header *h, int32_t sub_index, char *out, size_t out_sz)
{
    if (out == NULL || out_sz == 0u)
        return;
    out[0] = '\0';
    if (h == NULL)
        return;

    /* A negative sub_index is C's spelling of Python's None. PagedList's empty
     * state passes it; every other call site passes selected_index + 1, which
     * is always >= 1. */
    if (sub_index >= 0)
        (void)snprintf(out, out_sz, "%s-%d", h->root_id, (int)sub_index);
    else
        (void)nd_strlcpy(out, h->root_id, out_sz);
}

int32_t nd_header_width(const nd_header *h, int32_t sub_index)
{
    char text[32];
    int32_t w = 0;

    if (h == NULL || h->ui == NULL)
        return 5;

    nd_header_text_for(h, sub_index, text, sizeof text);
    nd_text_size(h->ui->font_n, text, &w, NULL);
    return 5 + w;
}

void nd_header_draw(const nd_header *h, int32_t sub_index)
{
    char text[32];
    int32_t w = 0;

    if (h == NULL || h->ui == NULL || h->ui->draw == NULL)
        return;

    nd_header_text_for(h, sub_index, text, sizeof text);
    nd_text_size(h->ui->font_n, text, &w, NULL);
    /* y = 5, unlike the list title beside it, which sits at y = 0. */
    (void)nd_draw_text(h->ui->draw, nd_ui_width(h->ui) - 5 - w, 5, text, h->ui->font_n, ND_WHITE);
}
