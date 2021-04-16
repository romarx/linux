/*
 *  linux/drivers/video/gcon.c -- A dummy console driver
 *
 *  To be used if there's no other console driver (e.g. for plain VGA text)
 *  available, usually until fbcon takes console over.
 */

#include <linux/types.h>
#include <linux/kdev_t.h>
#include <linux/console.h>
#include <linux/vt_kern.h>
#include <linux/screen_info.h>
#include <linux/init.h>
#include <linux/module.h>

#define BLANK                   0x0020
#define AH_BASE                 0x19000000
/*
  Offsets relative to AH_BASE
*/
#define AH_PNTRQ_ADDR           0x0
#define AH_HVTOT_REG_ADDR       0x8
#define AH_HVACT_REG_ADDR       0x10
#define AH_HVFRONT_REG_ADDR     0x18
#define AH_HVSYNC_REG_ADDR      0x20
#define AH_PWR_REG_ADDR         0x28
#define AH_CURR_PNTR_ADDR       0x30
#define AH_TEXT_PARAM_ADDR      0x38
#define AH_CURSOR_PARAM_ADDR    0x40
#define AH_FG_COLOR_START_ADDR  0x400
#define AH_BG_COLOR_START_ADDR  0x800


/*
  hardcode video format like a bad boy (SVGA60)
*/
#define GCON_VIDEO_LINES 600
#define GCON_VIDEO_COLS 800
#define GCON_HTOT 1056
#define GCON_VTOT 628
#define GCON_HFRONT 40
#define GCON_VFRONT 1
#define GCON_HSYNC 128
#define GCON_VSYNC 4
/* log2 of #frames of blink interval */
#define GCON_BLINK_T 5
/* font hardcoded for now */
#define GCON_FONTW 8
#define GCON_TEXT_ROWS 37
#define GCON_TEXT_COLS 100


/* set by Kconfig. Use 80x25 for 640x480 and 160x64 for 1280x1024 
#define DUMMY_COLUMNS	CONFIG_DUMMY_CONSOLE_COLUMNS
#define DUMMY_ROWS	CONFIG_DUMMY_CONSOLE_ROWS
*/

#ifdef CONFIG_FRAMEBUFFER_CONSOLE_DEFERRED_TAKEOVER
/* These are both protected by the console_lock */
static RAW_NOTIFIER_HEAD(dummycon_output_nh);
static bool dummycon_putc_called;

void dummycon_register_output_notifier(struct notifier_block *nb)
{
	raw_notifier_chain_register(&dummycon_output_nh, nb);

	if (dummycon_putc_called)
		nb->notifier_call(nb, 0, NULL);
}

void dummycon_unregister_output_notifier(struct notifier_block *nb)
{
	raw_notifier_chain_unregister(&dummycon_output_nh, nb);
}

static void dummycon_putc(struct vc_data *vc, int c, int ypos, int xpos)
{
	dummycon_putc_called = true;
	raw_notifier_call_chain(&dummycon_output_nh, 0, NULL);
}

static void dummycon_putcs(struct vc_data *vc, const unsigned short *s,
			   int count, int ypos, int xpos)
{
	int i;

	if (!dummycon_putc_called) {
		/* Ignore erases */
		for (i = 0 ; i < count; i++) {
			if (s[i] != vc->vc_video_erase_char)
				break;
		}
		if (i == count)
			return;

		dummycon_putc_called = true;
	}

	raw_notifier_call_chain(&dummycon_output_nh, 0, NULL);
}

static int dummycon_blank(struct vc_data *vc, int blank, int mode_switch)
{
	/* Redraw, so that we get putc(s) for output done while blanked */
	return 1;
}
#else
static void dummycon_putc(struct vc_data *vc, int c, int ypos, int xpos) { }
static void dummycon_putcs(struct vc_data *vc, const unsigned short *s,
			   int count, int ypos, int xpos) { }
static int dummycon_blank(struct vc_data *vc, int blank, int mode_switch)
{
	return 0;
}
#endif

static const char *dummycon_startup(void)
{
	pr_info("Entered gcon_startup\n");
    return "gcon device";
}

static void dummycon_init(struct vc_data *vc, int init)
{
	pr_info("Entered gcon_init\n");
    vc->vc_can_do_color = 1;
    if (init) {
	vc->vc_cols = GCON_TEXT_COLS;
	vc->vc_rows = GCON_TEXT_ROWS;
    } else
	vc_resize(vc, GCON_TEXT_COLS, GCON_TEXT_ROWS);
}

static void dummycon_deinit(struct vc_data *vc) { }
static void dummycon_clear(struct vc_data *vc, int sy, int sx, int height,
			   int width) { }
static void dummycon_cursor(struct vc_data *vc, int mode) { }

static bool dummycon_scroll(struct vc_data *vc, unsigned int top,
			    unsigned int bottom, enum con_scroll dir,
			    unsigned int lines)
{
	return false;
}

static int dummycon_switch(struct vc_data *vc)
{
	return 0;
}

static int dummycon_font_set(struct vc_data *vc, struct console_font *font,
			     unsigned int flags)
{
	return 0;
}

static int dummycon_font_default(struct vc_data *vc,
				 struct console_font *font, char *name)
{
	return 0;
}

static int dummycon_font_copy(struct vc_data *vc, int con)
{
	return 0;
}

/*
 *  The console `switch' structure for the dummy console
 *
 *  Most of the operations are dummies.
 */

const struct consw gcon = {
	.owner =		THIS_MODULE,
	.con_startup =	dummycon_startup,
	.con_init =		dummycon_init,
	.con_deinit =	dummycon_deinit,
	.con_clear =	dummycon_clear,
	.con_putc =		dummycon_putc,
	.con_putcs =	dummycon_putcs,
	.con_cursor =	dummycon_cursor,
	.con_scroll =	dummycon_scroll,
	.con_switch =	dummycon_switch,
	.con_blank =	dummycon_blank,
	.con_font_set =	dummycon_font_set,
	.con_font_default =	dummycon_font_default,
	.con_font_copy =	dummycon_font_copy,
};
EXPORT_SYMBOL_GPL(gcon);
