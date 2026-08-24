/*
 * Copyright 2012 Vincent Sanders <vince@simtec.co.uk>
 *
 * This file is part of libnsfb, http://www.netsurf-browser.org/
 * Licenced under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>

#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>


#include "libnsfb.h"
#include "libnsfb_event.h"
#include "libnsfb_plot.h"
#include "libnsfb_plot_util.h"

#include "nsfb.h"
#include "plot.h"
#include "surface.h"
#include "cursor.h"

#include "linux_evdev.h"



#define UNUSED(x) ((x) = (x))

#define FB_NAME "/dev/fb0"

#define MAX_INPUT_DEVS 8

struct lnx_priv {
    struct fb_fix_screeninfo FixInfo;
    struct fb_var_screeninfo VarInfo;
    int fd;
    struct pollfd input[MAX_INPUT_DEVS];
    int ninput;
};

/* test whether an opened event device reports keys */
static bool evdev_is_keyboard(int fd)
{
    unsigned long evbits = 0;

    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), &evbits) < 0)
	return false;

    return (evbits & (1UL << EV_KEY)) != 0;
}

/*
 * Open the input devices delivering key events.
 *
 * NSFB_INPUT_DEV in the environment names a single device to use
 * instead of scanning (the NeoDCT keypad on target hardware).
 */
static void evdev_open_inputs(struct lnx_priv *lstate)
{
    const char *override = getenv("NSFB_INPUT_DEV");
    char path[32];
    int idx, fd;

    lstate->ninput = 0;

    if (override != NULL) {
	fd = open(override, O_RDONLY | O_NONBLOCK);
	if (fd >= 0) {
	    lstate->input[0].fd = fd;
	    lstate->input[0].events = POLLIN;
	    lstate->ninput = 1;
	}
	return;
    }

    for (idx = 0; idx < 32 && lstate->ninput < MAX_INPUT_DEVS; idx++) {
	snprintf(path, sizeof(path), "/dev/input/event%d", idx);
	fd = open(path, O_RDONLY | O_NONBLOCK);
	if (fd < 0)
	    continue;
	if (!evdev_is_keyboard(fd)) {
	    close(fd);
	    continue;
	}
	lstate->input[lstate->ninput].fd = fd;
	lstate->input[lstate->ninput].events = POLLIN;
	lstate->ninput++;
    }
}

static int linux_set_geometry(nsfb_t *nsfb, int width, int height, enum nsfb_format_e format)
{
    if (nsfb->surface_priv != NULL) {
        return -1; /* if we are already initialised fail */
    }

    nsfb->width = width;
    nsfb->height = height;
    nsfb->format = format;

    /* select default sw plotters for bpp */
    if (select_plotters(nsfb) != true) {
	return -1;
    }

    return 0;
}

static enum nsfb_format_e
format_from_lstate(struct lnx_priv *lstate) 
{
    enum nsfb_format_e fmt = NSFB_FMT_ANY;

    switch(lstate->VarInfo.bits_per_pixel) {
    case 32:
	if (lstate->VarInfo.transp.length == 0)
	    fmt = NSFB_FMT_XBGR8888;
	else
	    fmt = NSFB_FMT_ABGR8888;
	break;

    case 24:
	fmt = NSFB_FMT_RGB888;
	break;

    case 16:
	fmt = NSFB_FMT_RGB565;
	break;

    case 8:
	fmt = NSFB_FMT_I8;
	break;

    case 1:
	fmt = NSFB_FMT_RGB565;
	break;

    }


    return fmt;
}

static int linux_initialise(nsfb_t *nsfb)
{
    int iFrameBufferSize;
    struct lnx_priv *lstate;
    enum nsfb_format_e lformat;

    if (nsfb->surface_priv != NULL)
	return -1;

    lstate = calloc(1, sizeof(struct lnx_priv));
    if (lstate == NULL) {
	return -1;
    }

    /* Open the framebuffer device in read write */
    lstate->fd = open(FB_NAME, O_RDWR);
    if (lstate->fd < 0) {
	printf("Unable to open %s.\n", FB_NAME);
	free(lstate);
	return -1;
    }

    /* Do Ioctl. Retrieve fixed screen info. */
    if (ioctl(lstate->fd, FBIOGET_FSCREENINFO, &lstate->FixInfo) < 0) {
	printf("get fixed screen info failed: %s\n",
	       strerror(errno));
	close(lstate->fd);
	free(lstate);
	return -1;
    }

    /* Do Ioctl. Get the variable screen info. */
    if (ioctl(lstate->fd, FBIOGET_VSCREENINFO, &lstate->VarInfo) < 0) {
	printf("Unable to retrieve variable screen info: %s\n",
	       strerror(errno));
	close(lstate->fd);
	free(lstate);
	return -1;
    }

    /* Calculate the size to mmap */
    iFrameBufferSize = lstate->FixInfo.line_length * lstate->VarInfo.yres;

    /* Now mmap the framebuffer. */
    nsfb->ptr = mmap(NULL, iFrameBufferSize, PROT_READ | PROT_WRITE,
			 MAP_SHARED, lstate->fd, 0);
    if (nsfb->ptr == NULL) {
	printf("mmap failed:\n");
	close(lstate->fd);
	free(lstate);
	return -1;
    }

    nsfb->linelen = lstate->FixInfo.line_length;

    nsfb->width = lstate->VarInfo.xres;
    nsfb->height = lstate->VarInfo.yres;
    
    lformat = format_from_lstate(lstate);

    if (nsfb->format != lformat) {
	nsfb->format = lformat;

	/* select default sw plotters for format */
	if (select_plotters(nsfb) != true) {
	    munmap(nsfb->ptr, 0);
	    close(lstate->fd);
	    free(lstate);
	    return -1;
	}
    }

    evdev_open_inputs(lstate);

    nsfb->surface_priv = lstate;

    return 0;
}

static int linux_finalise(nsfb_t *nsfb)
{
    struct lnx_priv *lstate = nsfb->surface_priv;
    int i;

    if (lstate != NULL) {
	for (i = 0; i < lstate->ninput; i++)
	    close(lstate->input[i].fd);
	munmap(nsfb->ptr, 0);
	close(lstate->fd);
	free(lstate);
    }

    return 0;
}

static bool linux_input(nsfb_t *nsfb, nsfb_event_t *event, int timeout)
{
    struct lnx_priv *lstate = nsfb->surface_priv;
    struct input_event ev;
    enum nsfb_key_code_e key;
    ssize_t rd;
    int i;

    if (lstate == NULL || lstate->ninput == 0) {
	/* no input devices; still honour the wait so the caller's
	 * event loop does not spin */
	if (timeout != 0)
	    poll(NULL, 0, timeout);
	return false;
    }

    while (true) {
	/* drain already-buffered events before polling */
	for (i = 0; i < lstate->ninput; i++) {
	    while ((rd = read(lstate->input[i].fd, &ev, sizeof(ev))) ==
		   (ssize_t)sizeof(ev)) {
		if (ev.type != EV_KEY)
		    continue;

		key = nsfb_linux_evdev_to_nsfb(ev.code);
		if (key == NSFB_KEY_UNKNOWN)
		    continue;

		/* value: 0 release, 1 press, 2 autorepeat */
		event->type = (ev.value == 0) ?
		    NSFB_EVENT_KEY_UP : NSFB_EVENT_KEY_DOWN;
		event->value.keycode = key;
		return true;
	    }
	}

	if (poll(lstate->input, lstate->ninput, timeout) <= 0)
	    return false;

	/* poll consumed the wait; do not wait again if the readable
	 * events all turn out to be uninteresting */
	timeout = 0;
    }
}

static int linux_claim(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    struct nsfb_cursor_s *cursor = nsfb->cursor;

    if ((cursor != NULL) && 
        (cursor->plotted == true) && 
        (nsfb_plot_bbox_intersect(box, &cursor->loc))) {

        nsfb->plotter_fns->bitmap(nsfb, 
                                  &cursor->savloc,  
                                  cursor->sav, 
                                  cursor->sav_width, 
                                  cursor->sav_height, 
                                  cursor->sav_width, 
                                  false);
        cursor->plotted = false;
    }
    return 0;
}

static int linux_cursor(nsfb_t *nsfb, struct nsfb_cursor_s *cursor)
{
    nsfb_bbox_t sclip;

    if ((cursor != NULL) && (cursor->plotted == true)) {
        sclip = nsfb->clip;

        nsfb->plotter_fns->set_clip(nsfb, NULL);

        nsfb->plotter_fns->bitmap(nsfb, 
                                  &cursor->savloc,  
                                  cursor->sav, 
                                  cursor->sav_width, 
                                  cursor->sav_height, 
                                  cursor->sav_width, 
                                  false);

        nsfb_cursor_plot(nsfb, cursor);

        nsfb->clip = sclip;
    }
    return true;
}


static int linux_update(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    struct nsfb_cursor_s *cursor = nsfb->cursor;

    UNUSED(box);

    if ((cursor != NULL) && (cursor->plotted == false)) {
        nsfb_cursor_plot(nsfb, cursor);
    }

    return 0;
}

const nsfb_surface_rtns_t linux_rtns = {
    .initialise = linux_initialise,
    .finalise = linux_finalise,
    .input = linux_input,
    .claim = linux_claim,
    .update = linux_update,
    .cursor = linux_cursor,
    .geometry = linux_set_geometry,
};

NSFB_SURFACE_DEF(linux, NSFB_SURFACE_LINUX, &linux_rtns)

/*
 * Local variables:
 *  c-basic-offset: 4
 *  tab-width: 8
 * End:
 */
