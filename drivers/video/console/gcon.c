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
#include <linux/ioport.h>
#include <asm/io.h>
#include <linux/slab.h>

#define BLANK 0x0020
#define AH_BASE 0x19000000

/*
  Offsets relative to AH_BASE
*/
#define AH_PNTRQ_ADDR 0x0
#define AH_HVTOT_REG_ADDR 0x8
#define AH_HVACT_REG_ADDR 0x10
#define AH_HVFRONT_REG_ADDR 0x18
#define AH_HVSYNC_REG_ADDR 0x20
#define AH_PWR_REG_ADDR 0x28
#define AH_CURR_PNTR_ADDR 0x30
#define AH_TEXT_PARAM_ADDR 0x38
#define AH_CURSOR_PARAM_ADDR 0x40
#define AH_FG_COLOR_START_ADDR 0x400
#define AH_BG_COLOR_START_ADDR 0x800

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

static bool gcon_init_done = 0;
static void *mapped_base = NULL;

static int font_factor;

static unsigned short *blank_buf = NULL;
static unsigned short *text_buf = NULL;

/* --------------------------------
   Internal Functions
   -------------------------------- */

static void write_ah(int offset, u32 data);
static void write_ah64(int offset, u64 data);
static u32 read_ah(int offset);
static u64 read_ah64(int offset);
static u64 read_current_p_ah(void);
static void write_text_p_ah(u64 p);
static u32 gen_textparam_reg(u16 cols, u16 rows);
static u32 gen_cursorparam_reg(u16 col, u16 row, u8 start, u8 end, u8 font_fac, u8 enable, u8 blink_t);
static void set_cursor_size(u32 *cur, u8 start, u8 end);
static void set_xy_cursor(u32 *cur, int col, int row);

/* --------------------------------
   Debug Functions
   -------------------------------- 
static void printregs(void);
*/

/* --------------------------------
   Console Functions
   -------------------------------- */

static void gcon_putc(struct vc_data *vc, int c, int ypos, int xpos) {
}

static void gcon_putcs(struct vc_data *vc, const unsigned short *s, int count, int ypos, int xpos) {
}

static int gcon_blank(struct vc_data *vc, int blank, int mode_switch) {
	pr_info("Entered gcon_blank!\n");
	switch (blank) {
	case 0: /*unblank*/
		if (text_buf) {
			write_text_p_ah((u64)virt_to_phys((volatile void *)vc->vc_origin)); //is this okay?
		}
		return 1;
	case 1:
	default: /*blank*/
		if (blank_buf) {
			write_text_p_ah((u64)virt_to_phys(blank_buf));
		} else {
			printk(KERN_ALERT "Blanking attempted with NULL blank_buf; Skipping\n");
			return 0;
		}
		return 1;
	}
	return 0;
}

static const char *gcon_startup(void) {
	const char *display_desc = "AXI_HDMI Text Mode Console";
	u32 hvsync;
	u8 max_fontfac_w, max_fontfac_h, max_fontfac;
	u8 fontfac_param;
	if (gcon_init_done) {
		return display_desc;
	}

	// request memory mapped IO for PAPER
	if (!request_mem_region(AH_BASE, 4096, "G Console AXI2HDMI driver")) {
		printk(KERN_ALERT "Failed to reserve A2H IO Address Space!\n");
		return NULL;
	}
	//pr_info("requesting memory region succeeded!\n");

	// get a base for memory mapped IO, size of PAPER's IO is 4kb
	if (!(mapped_base = ioremap((resource_size_t)AH_BASE, 4096))) {
		printk(KERN_ALERT "IOremap failed\n");
		mapped_base = NULL;
		return NULL;
	}
	//pr_info("ioremap worked, mapped base: %p\n", mapped_base);

	max_fontfac_w = GCON_VIDEO_COLS / (GCON_TEXT_COLS * GCON_FONTW);
	max_fontfac_h = GCON_VIDEO_LINES / (GCON_TEXT_ROWS * 2 * GCON_FONTW);
	max_fontfac = (max_fontfac_w < max_fontfac_h) ? max_fontfac_w : max_fontfac_h;
	fontfac_param = 0;

	switch (max_fontfac) {
	case 2:
		fontfac_param = 1;
		break;
	case 3:
		fontfac_param = 1;
		break;
	case 4:
		fontfac_param = 2;
		break;
	default:
		break;
	}

	// to allocate memory for the buffers
	if (!(blank_buf = kzalloc(GCON_TEXT_COLS * GCON_TEXT_ROWS * sizeof(u16), GFP_USER))) {
		pr_info("Failed to allocate blank buffer memory with kzalloc.\n");
		return NULL;
	}
	//pr_info("kzalloc worked for blank buf\n");

	if (!(text_buf = kzalloc(GCON_TEXT_COLS * GCON_TEXT_ROWS * sizeof(u16), GFP_USER))) {
		pr_info("Failed to allocate text buffer memory with kzalloc.\n");
		return NULL;
	}
	//pr_info("kzalloc worked for text buf\n");

	// on startup, power off AXI_HDMI
	write_ah(AH_PWR_REG_ADDR, 0);
	write_ah(AH_TEXT_PARAM_ADDR, gen_textparam_reg(GCON_TEXT_COLS, GCON_TEXT_ROWS));
	// cursor off by default
	write_ah(AH_CURSOR_PARAM_ADDR, gen_cursorparam_reg(0, 0, 0, 2 * GCON_FONTW - 1, fontfac_param, 0, GCON_BLINK_T));
	//set timings
	write_ah(AH_HVACT_REG_ADDR, (GCON_VIDEO_COLS << 16) + GCON_VIDEO_LINES);
	write_ah(AH_HVTOT_REG_ADDR, (GCON_HTOT << 16) + GCON_VTOT);
	write_ah(AH_HVFRONT_REG_ADDR, (GCON_HFRONT << 16) + GCON_VFRONT);

	//set vsync, hsync and polarity
	hvsync = (GCON_HSYNC << 16) + GCON_VSYNC;
	hvsync |= 0x80008000;
	write_ah(AH_HVSYNC_REG_ADDR, hvsync);

	font_factor = fontfac_param;
	gcon_init_done = 1;

	//pr_info("gcon startup done\n");
	return display_desc;
}

static void gcon_init(struct vc_data *vc, int init) {
	//pr_info("Entered gcon_init\n");
	vc->vc_can_do_color = 1;
	if (init) {
		vc->vc_cols = GCON_TEXT_COLS;
		vc->vc_rows = GCON_TEXT_ROWS;
	} else {
		vc_resize(vc, GCON_TEXT_COLS, GCON_TEXT_ROWS);
	}

	vc->vc_scan_lines = GCON_VIDEO_LINES;
	vc->vc_font.height = 2 * GCON_FONTW;
	vc->vc_complement_mask = 0x7700;
	vc->vc_hi_font_mask = 0;

	//pr_info("Finish gcon_init\n");
}

static int gcon_set_origin(struct vc_data *vc) {
	//pr_info("Entered gcon_set_origin\n");

	u64 curr_p;
	u32 pwr;
	unsigned long tp_phys_actual = 0;

	curr_p = read_current_p_ah();
	pwr = read_ah(AH_PWR_REG_ADDR);
	//pr_info("Current pointer: %llx\n", curr_p);

	if (text_buf) {
		//pr_info("Attempt setting pointer to origin\n");
		vc->vc_screenbuf = text_buf;
		vc->vc_screenbuf_size = GCON_TEXT_COLS * GCON_TEXT_ROWS * sizeof(u16);
		vc->vc_origin = (unsigned long)text_buf;
		tp_phys_actual = virt_to_phys(text_buf);
		//pr_info("physical address of text_buffer: %lx\n", tp_phys_actual);
	} else {
		pr_alert("No text buffer set");
		return -ENOMEM;
	}

	if (!tp_phys_actual) {
		pr_alert("No physical address for dma access set!");
		return -ENOMEM;
	}

	// give pointer for text_buffer to AH_BASE
	if (curr_p != tp_phys_actual) {
		write_text_p_ah((u64)tp_phys_actual);
	}
	if (!(pwr & 0x1)) {
		write_ah(AH_PWR_REG_ADDR, 1);
	}
	//printregs();
	return 1;
}

static u8 gcon_build_attr(struct vc_data *vc, u8 color, u8 intensity, u8 blink, u8 underline, u8 reverse, u8 italic) {
	//pr_info("Entered gcon_build_attr!\n");
	u8 attr;
	attr = color;
	if (italic) {
		attr = (attr & 0xF0) | vc->vc_itcolor;
	} else if (underline) {
		attr = (attr & 0xf0) | vc->vc_ulcolor;
	} else if (intensity == 0) {
		attr = (attr & 0xf0) | vc->vc_halfcolor;
	}

	if (reverse) {
		attr = ((attr)&0x88) | ((((attr) >> 4) | ((attr) << 4)) & 0x77);
	}
	if (blink) {
		attr ^= 0x80;
	}
	if (intensity == 2) {
		attr ^= 0x08;
	}
	return attr;
}

static void gcon_deinit(struct vc_data *vc) {
	pr_info("Enter gcon_deinit\n");
	write_ah(AH_PWR_REG_ADDR, 0);
	iounmap(mapped_base);
	release_mem_region(AH_BASE, 4096);
	mapped_base = NULL;
	if (blank_buf) {
		kfree((void *)blank_buf);
	}
	if (text_buf) {
		kfree((void *)text_buf);
	}
}
static void gcon_clear(struct vc_data *vc, int sy, int sx, int height, int width) {
}

static unsigned long gcon_getxy(struct vc_data *vc, unsigned long pos, int *px, int *py) {
	unsigned long ret;
	int x, y;

	if (pos >= vc->vc_origin && pos < vc->vc_scr_end) {
		unsigned long offset = (pos - vc->vc_origin) / 2;
		x = offset % vc->vc_cols;
		y = offset / vc->vc_cols;
		ret = pos;
	} else {
		x = 0;
		y = 0;
		ret = vc->vc_origin;
	}
	if (px) {
		*px = x;
	}
	if (py) {
		*py = y;
	}
	return ret;
}

static void gcon_cursor(struct vc_data *vc, int mode) {
	if (vc->vc_mode != KD_TEXT) {
		return;
	}

	u32 cur = read_ah(AH_CURSOR_PARAM_ADDR);

	switch (mode) {
	case CM_ERASE:
		cur &= 0xffffdfff; //disable cursor
		break;
	case CM_MOVE:
	case CM_DRAW:
		switch (vc->vc_cursor_type & 0x0f) {
			int x, y;
		case CUR_UNDERLINE:
			gcon_getxy(vc, vc->vc_pos, &x, &y);
			set_cursor_size(&cur, (u8)(vc->vc_font.height - (vc->vc_font.height < 10 ? 2 : 3)),
					(u8)(vc->vc_font.height - (vc->vc_font.height < 10 ? 1 : 2)));
			set_xy_cursor(&cur, x, y);
			cur |= 0x00002000;
			break;
		case CUR_TWO_THIRDS:
			gcon_getxy(vc, vc->vc_pos, &x, &y);
			set_cursor_size(&cur, (u8)(vc->vc_font.height / 3), (u8)(vc->vc_font.height - (vc->vc_font.height < 10 ? 1 : 2)));
			set_xy_cursor(&cur, x, y);
			cur |= 0x00002000;
			break;
		case CUR_LOWER_THIRD:
			gcon_getxy(vc, vc->vc_pos, &x, &y);
			set_cursor_size(&cur, (u8)((vc->vc_font.height * 2) / 3), (u8)(vc->vc_font.height - (vc->vc_font.height < 10 ? 1 : 2)));
			set_xy_cursor(&cur, x, y);
			cur |= 0x00002000;
			break;
		case CUR_LOWER_HALF:
			gcon_getxy(vc, vc->vc_pos, &x, &y);
			set_cursor_size(&cur, (u8)(vc->vc_font.height / 2), (u8)(vc->vc_font.height - (vc->vc_font.height < 10 ? 1 : 2)));
			set_xy_cursor(&cur, x, y);
			cur |= 0x00002000;
			break;
		case CUR_NONE:
			cur &= 0xffffdfff;
			break;
		default:
			gcon_getxy(vc, vc->vc_pos, &x, &y);
			set_cursor_size(&cur, 1, (u8)vc->vc_font.height);
			set_xy_cursor(&cur, x, y);
			cur |= 0x00002000;
			break;
		}
		break;
	}
	write_ah(AH_CURSOR_PARAM_ADDR, cur);
}

static bool gcon_scroll(struct vc_data *vc, unsigned int top, unsigned int bottom, enum con_scroll dir, unsigned int lines) {
	return false;
}

static int gcon_switch(struct vc_data *vc) {
	//pr_info("Entered gcon switch\n");
	return 1;
}

static int gcon_font_set(struct vc_data *vc, struct console_font *font, unsigned int flags) {
	return 0;
}

static int gcon_font_default(struct vc_data *vc, struct console_font *font, char *name) {
	return 0;
}

static int gcon_font_copy(struct vc_data *vc, int con) {
	return 0;
}

/* --------------------------------
   Internal Functions
   -------------------------------- */

static void write_ah(int offset, u32 data) {
	if (mapped_base) {
		writel(data, (volatile void *)mapped_base + offset);
	}
}

static void write_ah64(int offset, u64 data) {
	if (mapped_base) {
		writeq(data, (volatile void *)mapped_base + offset);
	}
}

static u32 read_ah(int offset) {
	if (mapped_base) {
		return readl((const volatile void *)mapped_base + offset);
	} else {
		return 0;
	}
}

static u64 read_ah64(int offset) {
	if (mapped_base) {
		return readq((const volatile void *)mapped_base + offset);
	} else {
		return 0;
	}
}

//keeps power status reg as-is
static void write_text_p_ah(u64 p) {
	u32 pwr = read_ah(AH_PWR_REG_ADDR);
	pwr |= (1 << 16);
	write_ah(AH_PWR_REG_ADDR, pwr);
	write_ah64(AH_PNTRQ_ADDR, p);
}

static u64 read_current_p_ah(void) {
	return read_ah64(AH_CURR_PNTR_ADDR);
}

static u32 gen_textparam_reg(u16 cols, u16 rows) {
	return ((cols << 16) + rows);
}

static u32 gen_cursorparam_reg(u16 col, u16 row, u8 start, u8 end, u8 font_fac, u8 enable, u8 blink_t) {
	u32 curs_reg = 0;
	curs_reg |= (col << 24);
	curs_reg |= (row << 16);
	curs_reg |= (font_fac << 14);
	curs_reg |= (enable << 13);
	curs_reg |= (start << 8);
	curs_reg |= (blink_t << 5);
	curs_reg |= (end << 0);
	return curs_reg;
}

static void set_cursor_size(u32 *cur, u8 start, u8 end) {
	//start and end - 1 to mimic behavior of vgacon function for cursor size
	*cur &= 0xffffe0e0;
	*cur |= ((start - 1) << 8);
	*cur |= ((end - 1) << 0);
}

static void set_xy_cursor(u32 *cur, int col, int row) {
	*cur &= 0x0000ffff;
	*cur |= col << 24;
	*cur |= row << 16;
}

/* --------------------------------
   Debug Functions
   -------------------------------- 

static void printregs(void) {
	// this prints every 64 bit register from PAPER
	pr_info("BASEPT:\t0x%llx\n", read_ah64(AH_PNTRQ_ADDR));
	pr_info("HVTOTL:\t0x%llx\n", read_ah64(AH_HVTOT_REG_ADDR));
	pr_info("HVACTI:\t0x%llx\n", read_ah64(AH_HVACT_REG_ADDR));
	pr_info("HVFRNT:\t0x%llx\n", read_ah64(AH_HVFRONT_REG_ADDR));
	pr_info("HVSYNC:\t0x%llx\n", read_ah64(AH_HVSYNC_REG_ADDR));
	pr_info("PWRREG:\t0x%llx\n", read_ah64(AH_PWR_REG_ADDR));
	pr_info("CURPTR:\t0x%llx\n", read_ah64(AH_CURR_PNTR_ADDR));
	pr_info("TXTPRM:\t0x%llx\n", read_ah64(AH_TEXT_PARAM_ADDR));
	pr_info("CURPRM:\t0x%llx\n", read_ah64(AH_CURSOR_PARAM_ADDR));
}
*/

const struct consw gcon = {
	.owner = THIS_MODULE,
	.con_startup = gcon_startup,
	.con_init = gcon_init,
	.con_deinit = gcon_deinit,
	.con_clear = gcon_clear,
	.con_putc = gcon_putc,
	.con_putcs = gcon_putcs,
	.con_cursor = gcon_cursor,
	.con_scroll = gcon_scroll,
	.con_switch = gcon_switch,
	.con_blank = gcon_blank,
	.con_font_set = gcon_font_set,
	.con_font_default = gcon_font_default,
	.con_font_copy = gcon_font_copy,
	.con_set_origin = gcon_set_origin,
	.con_build_attr = gcon_build_attr,
	.con_getxy = gcon_getxy,
};
EXPORT_SYMBOL_GPL(gcon);