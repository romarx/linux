/*
	Framebuffer driver for PAPER AXI HDMI 
	heavily inspired by ocfb.c 
*/

#include <linux/delay.h>
//#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/slab.h>

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

#define PALETTE_SIZE 256

#define PAPERFB_NAME "PAPER AXI/RGB FB"

static char *mode_option;

static const struct fb_videomode default_mode = {
	/* 800x600 @ 60 Hz, 37.8 kHz hsync */
	/* 	
		NULL, <Freq>, <hact>, <vact>, <pxclk_T>, 
		<hback>, <hfront>, <vback>, <vfront>, 
		<hsync>, <vsync>, <hsyncpol | vsyncpol>, 
		<vmode>, <isvesa>	
	*/
	NULL,
	60,
	800,
	600,
	25000,
	88,
	40,
	23,
	1,
	128,
	4,
	FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	FB_VMODE_NONINTERLACED
};

struct paperfb_dev {
	struct fb_info info;
	void __iomem *regs;
	/* Physical and virtual addresses of framebuffer */
	dma_addr_t fb_phys;
	void __iomem *fb_virt;
	u32 pseudo_palette[PALETTE_SIZE];
};

#ifndef MODULE
static int __init paperfb_setup(char *options)
{
	char *curr_opt;

	if (!options || !*options)
		return 0;

	while ((curr_opt = strsep(&options, ",")) != NULL) {
		if (!*curr_opt)
			continue;
		mode_option = curr_opt;
	}

	return 0;
}
#endif

static inline u32 paperfb_readreg(struct paperfb_dev *fbdev, loff_t offset)
{
	return ioread32(fbdev->regs + offset);
}

static void paperfb_writereg(struct paperfb_dev *fbdev, loff_t offset, u32 data)
{
	iowrite32(data, fbdev->regs + offset);
}

static int paperfb_setupfb(struct paperfb_dev *fbdev)
{
	/*	TODO: set pixel clock with var->pixclock (this is the pixel 
		clock period in ps, e.g 25000 for 800x600). 
		This module will depend on the clock wizard driver, it needs to 
		call it to reconfigure the clock but this functionality is 
		not implemented yet.
		Add COMMON_CLK_XLNX_CLKWZRD to the dependencies of this module
		in Kconfig after implementation (and enable it).
		Maybe also copy paste the newest version of the clocking wizard
		driver from linus torvalds linux git repo for fractional
		multiplication.
		It's in drivers/staging/clocking-wizard
	*/
	struct fb_var_screeninfo *var = &fbdev->info.var;
	u32 hlen;
	u32 vlen;
	u32 hvsync;

	/* Disable display */
	paperfb_writereg(fbdev, AH_PWR_REG_ADDR, 0);

	/* length and width of frame*/
	paperfb_writereg(fbdev, AH_HVACT_REG_ADDR,
			 (var->xres << 16) + var->yres);

	/* Total length and width of frame */
	hlen = var->left_margin + var->right_margin + var->hsync_len +
	       var->xres;
	vlen = var->upper_margin + var->lower_margin + var->vsync_len +
	       var->yres;
	paperfb_writereg(fbdev, AH_HVTOT_REG_ADDR, (hlen << 16) + vlen);

	/* Horizontal and vertical front porch */
	paperfb_writereg(fbdev, AH_HVFRONT_REG_ADDR,
			 (var->right_margin << 16) + var->lower_margin);

	/* Horizontal and verical sync */
	hvsync = (var->hsync_len << 16) + var->vsync_len;
	if (0x00000001 & var->sync) {
		hvsync |= 0x80000000;
	}
	if (0x00000010 & var->sync) {
		hvsync |= 0x00008000;
	}
	paperfb_writereg(fbdev, AH_HVSYNC_REG_ADDR, hvsync);

	/* Put framebuffer address into queue */
	paperfb_writereg(fbdev, AH_PNTRQ_ADDR, fbdev->fb_phys);

	/*check addresses
	pr_info("BASEPT:\t0x%x\n", paperfb_readreg(fbdev, AH_PNTRQ_ADDR));
	pr_info("HVTOTL:\t0x%x\n", paperfb_readreg(fbdev, AH_HVTOT_REG_ADDR));
	pr_info("HVACTI:\t0x%x\n", paperfb_readreg(fbdev, AH_HVACT_REG_ADDR));
	pr_info("HVFRNT:\t0x%x\n", paperfb_readreg(fbdev, AH_HVFRONT_REG_ADDR));
	pr_info("HVSYNC:\t0x%x\n", paperfb_readreg(fbdev, AH_HVSYNC_REG_ADDR));
	pr_info("PWRREG:\t0x%x\n", paperfb_readreg(fbdev, AH_PWR_REG_ADDR));
	pr_info("CURPTR:\t0x%x\n", paperfb_readreg(fbdev, AH_CURR_PNTR_ADDR));
	pr_info("TXTPRM:\t0x%x\n", paperfb_readreg(fbdev, AH_TEXT_PARAM_ADDR));
	pr_info("CURPRM:\t0x%x\n",
		paperfb_readreg(fbdev, AH_CURSOR_PARAM_ADDR));
	*/

	/* Enable display */
	paperfb_writereg(fbdev, AH_PWR_REG_ADDR, 1);
	return 0;
}

static int paperfb_setcolreg(unsigned regno, unsigned red, unsigned green,
			     unsigned blue, unsigned transp,
			     struct fb_info *info)
{
	if (regno >= info->cmap.len) {
		dev_err(info->device, "regno >= cmap.len\n");
		return 1;
	}

	red >>= (16 - info->var.red.length);
	green >>= (16 - info->var.green.length);
	blue >>= (16 - info->var.blue.length);

	((u32 *)(info->pseudo_palette))[regno] =
		(red << info->var.red.offset) |
		(green << info->var.green.offset) |
		(blue << info->var.blue.offset);
	return 0;
}

static int paperfb_init_fix(struct paperfb_dev *fbdev)
{
	struct fb_var_screeninfo *var = &fbdev->info.var;
	struct fb_fix_screeninfo *fix = &fbdev->info.fix;

	strcpy(fix->id, PAPERFB_NAME);

	fix->line_length = var->xres * var->bits_per_pixel / 8;
	fix->smem_len = fix->line_length * var->yres;
	fix->type = FB_TYPE_PACKED_PIXELS;

	fix->visual = FB_VISUAL_TRUECOLOR;

	return 0;
}

static int paperfb_init_var(struct paperfb_dev *fbdev)
{
	struct fb_var_screeninfo *var = &fbdev->info.var;
	struct device *dev = fbdev->info.device;

	var->accel_flags = FB_ACCEL_NONE;
	var->activate = FB_ACTIVATE_NOW;
	var->xres_virtual = var->xres;
	var->yres_virtual = var->yres;

	if (var->bits_per_pixel != 24) {
		dev_err(dev, "Colour depth not supported!\n");
		return -EINVAL;
	} else {
		var->transp.offset = 0;
		var->transp.length = 0;
		var->red.offset = 16;
		var->red.length = 8;
		var->green.offset = 8;
		var->green.length = 8;
		var->blue.offset = 0;
		var->blue.length = 8;
	}
	return 0;
}

static struct fb_ops paperfb_ops = {
	.owner = THIS_MODULE,
	.fb_setcolreg = paperfb_setcolreg,
	.fb_fillrect = cfb_fillrect,
	.fb_copyarea = cfb_copyarea,
	.fb_imageblit = cfb_imageblit,
};

static int paperfb_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct paperfb_dev *fbdev;
	struct resource *res;
	int fbsize;

	fbdev = devm_kzalloc(&pdev->dev, sizeof(*fbdev), GFP_KERNEL);
	if (!fbdev) {
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, fbdev);

	fbdev->info.fbops = &paperfb_ops;
	fbdev->info.device = &pdev->dev;
	fbdev->info.par = fbdev;

	/* Video mode setup */
	if (!fb_find_mode(&fbdev->info.var, &fbdev->info, mode_option, NULL, 0,
			  &default_mode, 24)) {
		dev_err(&pdev->dev, "No valid video modes found\n");
		return -EINVAL;
	}

	paperfb_init_var(fbdev);
	paperfb_init_fix(fbdev);

	/* Request I/O resource */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "I/O resource request failed\n");
		return -ENXIO;
	}

	fbdev->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(fbdev->regs)) {
		return PTR_ERR(fbdev->regs);
	}

	/* Allocate framebuffer memory */
	/* maybe try this with the dma-remapping include, this works but it isn't as nice*/
	fbsize = fbdev->info.fix.smem_len;
	fbdev->fb_virt = kzalloc(fbsize, GFP_KERNEL);

	if (!fbdev->fb_virt) {
		dev_err(&pdev->dev, "Frame buffer memory allocation failed\n");
		return -ENOMEM;
	}

	fbdev->fb_phys = virt_to_phys(fbdev->fb_virt);
	fbdev->info.fix.smem_start = fbdev->fb_phys;
	fbdev->info.screen_base = fbdev->fb_virt;
	fbdev->info.pseudo_palette = fbdev->pseudo_palette;

	/* Setup and enable the framebuffer */
	paperfb_setupfb(fbdev);

	/* Allocate color map */
	ret = fb_alloc_cmap(&fbdev->info.cmap, PALETTE_SIZE, 0);
	if (ret) {
		dev_err(&pdev->dev, "Color map allocation failed\n");
		kfree(fbdev->fb_virt);
	}

	/* Register framebuffer */
	ret = register_framebuffer(&fbdev->info);
	if (ret) {
		dev_err(&pdev->dev, "Framebuffer registration failed\n");
		fb_dealloc_cmap(&fbdev->info.cmap);
	}

	return ret;
}

static int paperfb_remove(struct platform_device *pdev)
{
	struct paperfb_dev *fbdev = platform_get_drvdata(pdev);

	unregister_framebuffer(&fbdev->info);
	fb_dealloc_cmap(&fbdev->info.cmap);
	kfree(fbdev->fb_virt);

	/* Disable display */
	paperfb_writereg(fbdev, AH_PWR_REG_ADDR, 0);

	platform_set_drvdata(pdev, NULL);

	return 0;
}

static struct of_device_id paperfb_match[] = {
	{
		.compatible = "paper,paperfb",
	},
	{},
};
MODULE_DEVICE_TABLE(of, paperfb_match);

static struct platform_driver paperfb_driver = { .probe = paperfb_probe,
						 .remove = paperfb_remove,
						 .driver = {
							 .name = "paper_fb",
							 .of_match_table =
								 paperfb_match,
						 } };

/*
 * Init and exit routines
 */
static int __init paperfb_init(void)
{
#ifndef MODULE
	char *option = NULL;

	if (fb_get_options("paperfb", &option))
		return -ENODEV;
	paperfb_setup(option);
#endif
	return platform_driver_register(&paperfb_driver);
}

static void __exit paperfb_exit(void)
{
	platform_driver_unregister(&paperfb_driver);
}

module_init(paperfb_init);
module_exit(paperfb_exit);

MODULE_DESCRIPTION("Framebuffer driver for PAPER on ariane");
module_param(mode_option, charp, 0);
MODULE_PARM_DESC(mode_option, "Video mode ('<xres>x<yres>[-<bpp>][@refresh]')");