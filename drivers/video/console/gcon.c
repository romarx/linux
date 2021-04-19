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
//#include <linux/dma-mapping.h>

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

//would like to use this but doesn't work atm.
//static dma_addr_t blank_buf_phys = 0, text_buf_phys = 0;

/* --------------------------------
   Internal Functions
   -------------------------------- */

static void write_ah(int offset, u32 data);
static u64 read_ah(int offset);
static u64 read_current_p_ah(void);
static void write_text_p_ah(unsigned long p);
static u32 gen_textparam_reg(u16 cols, u16 rows);
static u32 gen_cursorparam_reg(u16 col, u16 row, u8 start, u8 end, u8 font_fac,
			       u8 enable, u8 blink_t);

static void dummycon_putc(struct vc_data *vc, int c, int ypos, int xpos)
{
}
static void dummycon_putcs(struct vc_data *vc, const unsigned short *s,
			   int count, int ypos, int xpos)
{
}
static int dummycon_blank(struct vc_data *vc, int blank, int mode_switch)
{
	return 0;
}
//#endif

static const char *gcon_startup(void)
{
	pr_info("Entered gcon_startup\n");
	u8 max_fontfac_w, max_fontfac_h, max_fontfac;
	u8 fontfac_param;
	if (gcon_init_done) {
		return "AXI_HDMI Text Mode Console";
	}
	if (!request_mem_region(AH_BASE, 4096, "G Console AXI2HDMI driver")) {
		printk(KERN_ALERT "Failed to reserve A2H IO Address Space!\n");
		return "AXI_HDMI Text Mode Console no memory reserved";
	}
	pr_info("requesting memory region succeeded!\n");

	if (!(mapped_base = ioremap((resource_size_t)AH_BASE, 4096))) {
		printk(KERN_ALERT "IOremap failed\n");
		mapped_base = NULL;
		return "AXI_HDMI Text Mode Console no ioremap";
	}
	pr_info("ioremap worked, mapped base: %pK\n", mapped_base);

	max_fontfac_w = GCON_VIDEO_COLS / (GCON_TEXT_COLS * GCON_FONTW);
	max_fontfac_h = GCON_VIDEO_LINES / (GCON_TEXT_ROWS * 2 * GCON_FONTW);
	max_fontfac =
		(max_fontfac_w < max_fontfac_h) ? max_fontfac_w : max_fontfac_h;
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

	pr_info("fontfac param calculated\n");
	/* this breaks everything

	if (!(blank_buf = (u16 *)dma_alloc_coherent(NULL, GCON_TEXT_COLS*GCON_TEXT_ROWS*sizeof(u16), &blank_buf_phys, GFP_KERNEL))) {
    	pr_info("Failed to allocate buffer of no color!\n");
    	blank_buf_phys = 0;
		return "AXI_HDMI Text Mode Console no blank buf";

	}
	pr_info("dma_alloc_coherent worked for blank buf\n");

	
	if (!(text_buf = (u16 *)dma_alloc_coherent(NULL, GCON_TEXT_COLS*GCON_TEXT_ROWS*sizeof(u16), &text_buf_phys, GFP_KERNEL))) {
    	pr_info("Failed to allocate buffer of text!\n");
		text_buf_phys = 0;
		return "AXI_HDMI Text Mode Console no text buf";
  	}
	pr_info("dma_alloc_coherent worked for text buf\n");
	*/

	//Using kmalloc (I don't know how safe this is yet)
	if (!(blank_buf = kzalloc(GCON_TEXT_COLS * GCON_TEXT_ROWS * sizeof(u16),
				  GFP_KERNEL))) {
		pr_info("Failed to allocate blank buffer memory with kzalloc.\n");
		return "AXI_HDMI Text Mode Console no blank buf";
	}
	pr_info("kzalloc worked for blank buf\n");

	if (!(text_buf = kzalloc(GCON_TEXT_COLS * GCON_TEXT_ROWS * sizeof(u16),
				 GFP_KERNEL))) {
		pr_info("Failed to allocate text buffer memory with kzalloc.\n");
		return "AXI_HDMI Text Mode Console no text buf";
	}
	pr_info("kzalloc worked for text buf\n");

	// on startup, power off AXI_HDMI
	write_ah(AH_PWR_REG_ADDR, 0);
	write_ah(AH_TEXT_PARAM_ADDR,
		 gen_textparam_reg(GCON_TEXT_COLS, GCON_TEXT_ROWS));
	// cursor off by default
	write_ah(AH_CURSOR_PARAM_ADDR,
		 gen_cursorparam_reg(0, 0, 0, 2 * GCON_FONTW - 1, fontfac_param,
				     0, GCON_BLINK_T));
	//set timings
	write_ah(AH_HVACT_REG_ADDR, (GCON_VIDEO_COLS << 16) + GCON_VIDEO_LINES);
	write_ah(AH_HVTOT_REG_ADDR, (GCON_HTOT << 16) + GCON_VTOT);
	write_ah(AH_HVFRONT_REG_ADDR, (GCON_HFRONT << 16) + GCON_VFRONT);
	write_ah(AH_HVSYNC_REG_ADDR, (GCON_HSYNC << 16) + GCON_VSYNC);

	font_factor = fontfac_param;
	gcon_init_done = 1;

	pr_info("gcon startup done\n");
	return "AXI_HDMI Text Mode Console";
}

static void gcon_init(struct vc_data *vc, int init)
{
	pr_info("Entered gcon_init\n");
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
	pr_info("Finish gcon_init\n");
}


static int gcon_set_origin(struct vc_data *vc) {
  pr_info("Entered gcon_set_origin");
  u64 curr_p,curr_p_after;
  u32 pwr;
  unsigned long tp_phys_actual;

  curr_p = read_current_p_ah();
  pwr = read_ah(AH_PWR_REG_ADDR);
  pr_info("Current pointer: %llx", curr_p);
  if(text_buf){
	pr_info("Attempt setting pointer to origin");
    vc->vc_origin = (unsigned long) text_buf;
    tp_phys_actual = virt_to_phys(text_buf);
	pr_info("Physical address: %lx", tp_phys_actual);
  } 

  if (curr_p != tp_phys_actual)
    write_text_p_ah(tp_phys_actual);
  if (!(pwr & 0x1))
    write_ah(AH_PWR_REG_ADDR, 1);
 
  curr_p_after = read_current_p_ah();
  pr_info("Current pointer after setting: %llx", curr_p_after);
  
  return 1;
}



static void gcon_deinit(struct vc_data *vc)
{
	pr_info("Enter gcon_deinit");
	write_ah(AH_PWR_REG_ADDR, 0);
	pr_info("Entered gcon_deinit");
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
static void dummycon_clear(struct vc_data *vc, int sy, int sx, int height,
			   int width)
{
}
/*
static unsigned long gcon_getxy(struct vc_data *vc, unsigned long pos, int *px,
				int *py)
{
	pr_info("Entered gcon_getxy!");
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
	if (px)
		*px = x;
	if (py)
		*py = y;
	return ret;
}
*/


static void gcon_cursor(struct vc_data *vc, int mode)
{
	/*
	pr_info("Entered gcon_cursor");
	u32 cur = read_ah(AH_CURSOR_PARAM_ADDR);
	switch (mode) {
	case CM_ERASE:
		cur &= 0xffffdfff;
		break;
	case CM_MOVE:
	case CM_DRAW:
		// for now only check for CUR_NONE 
		if ((vc->vc_cursor_type & 0x0f) == CUR_NONE)
			cur &= 0xffffdfff;
		else {
			int x, y;
			gcon_getxy(vc, vc->vc_pos, &x, &y);
			cur |= 0x00002000;
			cur &= 0x0000ffff;
			cur |= x << 24;
			cur |= y << 16;
		}
	}
	write_ah(AH_CURSOR_PARAM_ADDR, cur);
	*/
}


static bool dummycon_scroll(struct vc_data *vc, unsigned int top,
			    unsigned int bottom, enum con_scroll dir,
			    unsigned int lines)
{
	return false;
}

static int gcon_switch(struct vc_data *vc)
{
	pr_info("Entered gcon switch");
	return 1;
}

static int dummycon_font_set(struct vc_data *vc, struct console_font *font,
			     unsigned int flags)
{
	return 0;
}

static int dummycon_font_default(struct vc_data *vc, struct console_font *font,
				 char *name)
{
	return 0;
}

static int dummycon_font_copy(struct vc_data *vc, int con)
{
	return 0;
}

/* --------------------------------
   Internal Functions
   -------------------------------- */

static void write_ah(int offset, u32 data)
{
	if (mapped_base)
		writel(data, (volatile void *)mapped_base + offset);
}

static u64 read_ah(int offset)
{
	if (mapped_base)
		return readq((const volatile void *)mapped_base + offset);
	else
		return 0;
}

/* 
keeps power status reg as-is 

original has a dma_addr_t instead of unsigned long
*/
static void write_text_p_ah(unsigned long p)
{
	u32 pwr = read_ah(AH_PWR_REG_ADDR);
	pwr |= (1 << 16);
	write_ah(AH_PWR_REG_ADDR, pwr);
	write_ah(AH_PNTRQ_ADDR, p);
}

static u64 read_current_p_ah(void)
{
	return read_ah(AH_CURR_PNTR_ADDR);
}

static u32 gen_textparam_reg(u16 cols, u16 rows)
{
	return ((cols << 16) + rows);
}

static u32 gen_cursorparam_reg(u16 col, u16 row, u8 start, u8 end, u8 font_fac,
			       u8 enable, u8 blink_t)
{
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

const struct consw gcon = {
	.owner = THIS_MODULE,
	.con_startup = gcon_startup,
	.con_init = gcon_init,
	.con_deinit = gcon_deinit,
	.con_clear = dummycon_clear,
	.con_putc = dummycon_putc,
	.con_putcs = dummycon_putcs,
	.con_cursor = gcon_cursor,
	.con_scroll = dummycon_scroll,
	.con_switch = gcon_switch,
	.con_blank = dummycon_blank,
	.con_font_set = dummycon_font_set,
	.con_font_default = dummycon_font_default,
	.con_font_copy = dummycon_font_copy,
	
	.con_set_origin = gcon_set_origin,
	//.con_getxy = gcon_getxy,
};
EXPORT_SYMBOL_GPL(gcon);