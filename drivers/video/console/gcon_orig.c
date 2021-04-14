#include <linux/module.h>
#include <linux/types.h>
#include <linux/vt_kern.h>
#include <linux/console.h>
#include <linux/console_struct.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/slab.h>
#include <asm/io.h>
#include <asm-generic/io.h>
#include <linux/dma-mapping.h>

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

/* ----------------------------------
   World Interface
   ---------------------------------- */

/*
  configure AXI_HDMI:
    -resolution/timings -> assume configuration already in place (...nasty business)
    -text cols/rows
    -font size -> based on cols/rows and resolution
    -cursor parameters
    -blanking

  pass the pointer to vc_screenbuf to AXI_HDMI pointer queue*/
static void gcon_init(struct vc_data *c, int init);

static const char * gcon_startup(void);
/*
  turn off AXI_HDMI
*/
static void gcon_deinit(struct vc_data *c);
static void gcon_cursor(struct vc_data *c, int mode);
/*
  enqueue the pointer for the switched-to text buffer
*/
static int gcon_switch(struct vc_data *c);
/*
  probably let vt handle this one
  --> not possible, so use blank_buf
*/
static int gcon_blank(struct vc_data *c, int blank, int mode_switch);

/*
  pass the pointer to origin to AXI_HDMI
*/
static int gcon_set_origin(struct vc_data *c);


/*
  this is a dummy which just inserts a memory barrier
*/
static void gcon_putc(struct vc_data *c, int a, int b, int k);
static void gcon_putcs(struct vc_data *c, const unsigned short *a, int b, int k, int d);
/*
   copied from fbcon.c
*/
static unsigned long gcon_getxy(struct vc_data *c, unsigned long pos, int *x, int *y);

/*
  unsure what this must do
*/
/*static void gcon_save_screen(struct vc_data *c);
static void gcon_invert_Region(struct vc_data *c, u16 * p, int count);
*/
static u8 gcon_build_attr(struct vc_data * c, u8 color, u8 intensity, u8 blink,
                          u8 underline, u8 reverse, u8 italic);
static int gcon_switch(struct vc_data *c);

static unsigned long gcon_getxy(struct vc_data *c, unsigned long pos, int *x, int *y);
/* --------------------------------
   Internal Functions
   -------------------------------- */

static void write_ah(int offset, u32 data);
static u32 read_ah(int offset);
static u32 read_current_p_ah(void);
static void write_text_p_ah(dma_addr_t);
static u32 gen_textparam_reg(u16 cols, u16 rows);
static u32 gen_cursorparam_reg(u16 col, u16 row, u8 start, u8 end, u8 font_fac,
                               u8 enable, u8 blink_t);

static bool gcon_init_done;
static u32 * mapped_base = NULL;
/* gets set in startup and on resize (lol there is no resize yet) */
static int font_factor;

static u16* blank_buf = NULL;
static u16* text_buf = NULL;

static dma_addr_t blank_buf_phys = 0, text_buf_phys = 0;

static const char *  gcon_startup(void) {

  const char *display_desc = "AXI_HDMI Text Mode Console";
  u8 max_fontfac_w, max_fontfac_h, max_fontfac;
  u8 fontfac_param;
  printk(KERN_INFO "Entered gcon_startup\n");
  if (gcon_init_done)
    return display_desc;
  if (!request_mem_region(AH_BASE, 4096, "G Console AXI2HDMI driver")){
    printk(KERN_ALERT "Failed to reserve A2H IO Address Space!\n");
    return NULL;
  }
  if (!(mapped_base = (u32 *) ioremap((resource_size_t)AH_BASE, 4096))) {
    mapped_base = NULL;
    return NULL;
  }
  max_fontfac_w = GCON_VIDEO_COLS/(GCON_TEXT_COLS*GCON_FONTW);
  max_fontfac_h = GCON_VIDEO_LINES/(GCON_TEXT_ROWS*2*GCON_FONTW);
  max_fontfac = (max_fontfac_w < max_fontfac_h) ? max_fontfac_w : max_fontfac_h;
  fontfac_param = 0;
  switch(max_fontfac) {
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
  if (!(blank_buf = (u16 *)dma_alloc_coherent(NULL, GCON_TEXT_COLS*GCON_TEXT_ROWS*sizeof(u16), &blank_buf_phys, GFP_KERNEL))) {
    printk("Failed to allocate buffer of no color!\nAllocating instead with kmalloc as a cop-out");
    blank_buf_phys = 0;
    if (!(blank_buf = kmalloc(GCON_TEXT_COLS*GCON_TEXT_ROWS*sizeof(u16), GFP_KERNEL)))
        printk("Failed to allocate buffer of no color with kmalloc!\n All is lost.\n");
  }
    if (!(text_buf = (u16 *)dma_alloc_coherent(NULL, GCON_TEXT_COLS*GCON_TEXT_ROWS*sizeof(u16), &text_buf_phys, GFP_KERNEL))) {
    printk("Failed to allocate buffer of text!\n");
  }


  // on startup, power off AXI_HDMI
  write_ah(AH_PWR_REG_ADDR, 0);
  write_ah(AH_TEXT_PARAM_ADDR, gen_textparam_reg(GCON_TEXT_COLS, GCON_TEXT_ROWS));
  // cursor off by default
  write_ah(AH_CURSOR_PARAM_ADDR, gen_cursorparam_reg(0,0,0,2*GCON_FONTW-1,
                                                     fontfac_param, 0, GCON_BLINK_T));
  write_ah(AH_HVACT_REG_ADDR, (GCON_VIDEO_COLS<<16) + GCON_VIDEO_LINES);
  write_ah(AH_HVTOT_REG_ADDR, (GCON_HTOT<<16) + GCON_VTOT);
  write_ah(AH_HVFRONT_REG_ADDR, (GCON_HFRONT<<16) + GCON_VFRONT);
  write_ah(AH_HVSYNC_REG_ADDR, (GCON_HSYNC<<16) + GCON_VSYNC);
  font_factor = fontfac_param;
  gcon_init_done = 1;
  return display_desc;
}

static void gcon_init(struct vc_data *c, int init) {
  if (!c) {
    printk(KERN_ERR "Got null vc_data struct in gcon_init!\n");
    return;
  }
  printk(KERN_INFO "Entered gcon_init\n");
  printk(KERN_INFO "Initializing gcon for console #%d\n", c->vc_num);
  c->vc_can_do_color = 1;
  /* unsure what the init is about */
  if (init) {
		c->vc_cols = GCON_TEXT_COLS;
		c->vc_rows = GCON_TEXT_ROWS;
	} else
		vc_resize(c, GCON_TEXT_COLS, GCON_TEXT_ROWS);
  /* let's hope this works - in some configurations of
     screen resolution and #cols/rows, we have to blank */
  c->vc_scan_lines = GCON_VIDEO_LINES;
  c->vc_font.height = 2*GCON_FONTW;
  c->vc_complement_mask = 0x7700;
  c->vc_hi_font_mask = 0;
}


static int gcon_set_origin(struct vc_data *c) {
  u32 curr_p;
  u32 pwr;
  dma_addr_t tp_phys_actual;

  curr_p = read_current_p_ah();
  pwr = read_ah(AH_PWR_REG_ADDR);
  if (text_buf_phys) {
    c->vc_origin = (unsigned long) text_buf;
    tp_phys_actual = text_buf_phys;
  } else {
    c->vc_origin = (unsigned long) c->vc_screenbuf;
    tp_phys_actual = (dma_addr_t) virt_to_phys((volatile void *)c->vc_origin);
    printk(KERN_ALERT "Sheeit text_buf_phys didn't get filled I'm glad I have this graceful error handling\n");
  }

  if (curr_p != tp_phys_actual)
    write_text_p_ah(tp_phys_actual);
  if (!(pwr & 0x1))
    write_ah(AH_PWR_REG_ADDR, 1);
  return 1;
}

static bool gcon_scroll(struct vc_data *c,unsigned int top,
                        unsigned int bottom, enum con_scroll dir,
                        unsigned int lines) {
  // is this right?????
  return false;
}


static int gcon_switch(struct vc_data *c) {
  /* maybe update pointer here?? */
  return 1; /* just redraw to be sure... */
}

static u8 gcon_build_attr(struct vc_data * c, u8 color, u8 intensity, u8 blink,
                          u8 underline, u8 reverse, u8 italic) {
  u8 attr;
  attr = color;
  if (italic)
    attr = (attr & 0xF0) | c->vc_itcolor;
  else if (underline)
    attr = (attr & 0xf0) | c->vc_ulcolor;
  else if (intensity == 0)
    attr = (attr & 0xf0) | c->vc_halfcolor;

	if (reverse)
		attr =
      ((attr) & 0x88) | ((((attr) >> 4) | ((attr) << 4)) &
                         0x77);
	if (blink)
		attr ^= 0x80;
	if (intensity == 2)
		attr ^= 0x08;
	return attr;
}

static unsigned long gcon_getxy(struct vc_data *c, unsigned long pos,
                                int *px, int *py) {
  unsigned long ret;
	int x, y;

	if (pos >= c->vc_origin && pos < c->vc_scr_end) {
		unsigned long offset = (pos - c->vc_origin) / 2;

		x = offset % c->vc_cols;
		y = offset / c->vc_cols;
    ret = pos;
  } else {
    x = 0;
    y = 0;
    ret = c->vc_origin;
  }
	if (px)
		*px = x;
	if (py)
		*py = y;
  return ret;
}

static int gcon_blank(struct vc_data *c, int blank, int mode_switch) {


  switch (blank) {
  case 0: /*unblank*/
    if (text_buf_phys)
      write_text_p_ah(text_buf_phys);
    else
      write_text_p_ah((dma_addr_t) virt_to_phys((volatile void *)c->vc_origin));
    return 1; /* whatever this means... */
  case 1:
  default: /*blank*/
    if (blank_buf_phys)
      write_text_p_ah(blank_buf_phys);
    else if (blank_buf)
      write_text_p_ah((dma_addr_t) virt_to_phys((volatile void *)blank_buf));
    else {
      printk(KERN_ALERT "Blanking attempted with NULL blank_buf; Skipping\n");
      return 0;
    }
    return 1;
  }
  return 0;
}

static void gcon_cursor(struct vc_data *c, int mode) {
  u32 cur = read_ah(AH_CURSOR_PARAM_ADDR);
  switch (mode) {
  case CM_ERASE:
    cur &= 0xffffdfff;
    break;
  case CM_MOVE:
  case CM_DRAW:
    /* for now only check for CUR_NONE */
    if ((c->vc_cursor_type & 0x0f) == CUR_NONE)
      cur &= 0xffffdfff;
    else {
      int x, y;
      gcon_getxy(c, c->vc_pos, &x, &y);
      cur |= 0x00002000;
      cur &= 0x0000ffff;
      cur |= x<<24;
      cur |= y<<16;
    }
  }
  write_ah(AH_CURSOR_PARAM_ADDR, cur);
}



static void gcon_deinit(struct vc_data * c) {
  write_ah(AH_PWR_REG_ADDR, 0);
  iounmap(mapped_base);
  release_mem_region(AH_BASE, 4096);
  mapped_base = NULL;
  if (text_buf_phys)
    dma_free_coherent(NULL, c->vc_screenbuf_size, (void *)text_buf, text_buf_phys);
  else
    kfree((void *)text_buf);
  blank_buf = NULL;
}




static void write_ah(int offset, u32 data) {
  if (mapped_base)
    writel(data, (volatile void *) mapped_base+offset);
  mb();
}

static u32 read_ah(int offset) {
  if (mapped_base)
    return readl((const volatile void *) mapped_base+offset);
  else return 0;
}

/*
   keeps power status reg as-is
*/
static void write_text_p_ah(dma_addr_t p) {
  u32 pwr = read_ah(AH_PWR_REG_ADDR);
  pwr |= (1<<16);
  write_ah(AH_PWR_REG_ADDR, pwr);
  write_ah(AH_PNTRQ_ADDR, (u32) p);
}

 static u32 read_current_p_ah(void) {
   return read_ah(AH_CURR_PNTR_ADDR);
 }

static u32 gen_textparam_reg(u16 cols, u16 rows) {
  return ((cols<<16)+rows);
}

static u32 gen_cursorparam_reg(u16 col, u16 row, u8 start, u8 end, u8 font_fac,
                               u8 enable, u8 blink_t) {
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
static void gcon_putcs(struct vc_data *c, const unsigned short *a, int b, int k, int d) {
    mb();
}

static void gcon_putc(struct vc_data *c, int a, int b, int k) {
    mb();
}

static void gcon_set_palette(struct vc_data *vc, const unsigned char *table) {
    mb();
}

static int gcon_dummy(struct vc_data *c)
{
	return 0;
}

#define DUMMY (void *) gcon_dummy

/* this driver is still in pre-alpha phase so
   lots of NULL's */
const struct consw gcon = {
  .owner = THIS_MODULE,
  .con_startup = gcon_startup,
  .con_init = gcon_init,
  .con_deinit = gcon_deinit ,
  .con_clear = DUMMY,
  .con_putc = gcon_putc,
  .con_putcs = gcon_putcs,
  .con_cursor = gcon_cursor,
  .con_scroll = gcon_scroll,
  .con_switch = gcon_switch,
  .con_blank = gcon_blank,
  .con_font_set = NULL, /* not supported (yet) */
  .con_font_get = NULL,
  .con_font_copy = NULL,
  .con_resize = NULL, /* not supported yet - this should
                         be easy to implement */
  .con_set_palette = gcon_set_palette,
  .con_scrolldelta = NULL,
  .con_set_origin = gcon_set_origin,
  .con_save_screen = NULL,
  .con_build_attr = gcon_build_attr,
  .con_invert_region = NULL, /* let vt.c handle this crap */
  .con_screen_pos = NULL,
  .con_getxy = gcon_getxy
};
EXPORT_SYMBOL_GPL(gcon);
