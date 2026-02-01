// SPDX-License-Identifier: GPL-2.0-only

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>

#define RTC_CTRL0_UNLOCKKEY		0x4
#define RTC_CTRL0				0x8
#define RTC_CTRL0_STATUS0		0xC
#define RTC_EN_PWR_WAKEUP		0xBC
#define RTC_EN_SHDN_REQ			0xC0
#define RTC_EN_PWR_CYC_REQ		0xC8
#define RTC_EN_WARM_RST_REQ		0xCC
#define RTC_EN_WDT_RST_REQ		0xE0
#define RTC_EN_SUSPEND_REQ		0xE4
#define RSM_STATE				0xD4
#define ST_ON					0x3

static void __iomem *base;

static int cvi_restart_handler(struct notifier_block *this,
				unsigned long mode, void *cmd)
{
	void __iomem *REG_RTC_CTRL_BASE = base;
	void __iomem *REG_RTC_BASE = base + 0x1000;

	/* Enable power suspend wakeup source mask */
	writel(0x1, REG_RTC_BASE + 0x3C); // 1 = select prdata from 32K domain

	writel(0xAB18, REG_RTC_CTRL_BASE + RTC_CTRL0_UNLOCKKEY);

	writel(0x1, REG_RTC_BASE + RTC_EN_WARM_RST_REQ);

	while (readl(REG_RTC_BASE + RTC_EN_WARM_RST_REQ) != 0x01)
		;

	while (readl(REG_RTC_BASE + RSM_STATE) != ST_ON)
		;

	writel(0xFFFF0800 | (0x1 << 4), REG_RTC_CTRL_BASE + RTC_CTRL0);

	return NOTIFY_DONE;
}

#define RTC_EN_PWR_VBAT_DET		0xD0

static void cvi_do_pwroff(void)
{
	void __iomem *REG_RTC_CTRL_BASE = base;
	void __iomem *REG_RTC_BASE = base + 0x1000;
	u32 wakeup_mask, val;

	pr_info("cvi_do_pwroff: Starting power off sequence\n");

	/* Enable power suspend wakeup source mask */
	writel(0x1, REG_RTC_BASE + 0x3C); // 1 = select prdata from 32K domain

	writel(0xAB18, REG_RTC_CTRL_BASE + RTC_CTRL0_UNLOCKKEY);

	/*
	 * CRITICAL: Disable RTC_EN_AUTO_POWER_UP (bit 2 of RTC_EN_PWR_VBAT_DET)
	 * Per SG2002 TRM section 6.4.3 point 7:
	 * "After the RTC is powered on for the first time, the register
	 * RTC_EN_AUTO_POWER_UP must be configured from the default value 1 to 0.
	 * If the default value is maintained, when the chip enters the power-down
	 * state, the RTC will automatically enter the power-on state when it
	 * detects that PWR_VBAT_DET is high level."
	 *
	 * This is the ROOT CAUSE of immediate reboot after poweroff!
	 */
	val = readl(REG_RTC_BASE + RTC_EN_PWR_VBAT_DET);
	pr_info("cvi_do_pwroff: RTC_EN_PWR_VBAT_DET before: 0x%08x\n", val);
	writel(0x0, REG_RTC_BASE + RTC_EN_PWR_VBAT_DET);
	val = readl(REG_RTC_BASE + RTC_EN_PWR_VBAT_DET);
	pr_info("cvi_do_pwroff: RTC_EN_PWR_VBAT_DET after:  0x%08x\n", val);
	if (val != 0x0)
		pr_warn("cvi_do_pwroff: WARNING - RTC_EN_PWR_VBAT_DET not cleared!\n");

	/*
	 * Disable power cycle request.
	 * The FSBL enables RTC_EN_PWR_CYC_REQ, which causes power cycle behavior.
	 */
	pr_info("cvi_do_pwroff: Disabling power cycle request\n");
	writel(0x0, REG_RTC_BASE + RTC_EN_PWR_CYC_REQ);
	while (readl(REG_RTC_BASE + RTC_EN_PWR_CYC_REQ) != 0x00)
		;

	/*
	 * Only keep RTC alarm as wake source.
	 * Clear all other wake bits, keep only bit 5 (alarm from suspend)
	 * and bit 13 (alarm power-on from off).
	 */
	wakeup_mask = readl(REG_RTC_BASE + RTC_EN_PWR_WAKEUP);
	pr_info("cvi_do_pwroff: Current wake mask: 0x%08x\n", wakeup_mask);
	/* Keep only bits 4, 5, 13 for RTC alarm wake */
	wakeup_mask &= (0x30 | (1 << 13));
	writel(wakeup_mask, REG_RTC_BASE + RTC_EN_PWR_WAKEUP);
	pr_info("cvi_do_pwroff: New wake mask: 0x%08x\n", wakeup_mask);

	writel(0x1, REG_RTC_BASE + RTC_EN_SHDN_REQ);

	while (readl(REG_RTC_BASE + RTC_EN_SHDN_REQ) != 0x01)
		;

	/* Log final register states for debugging */
	pr_info("cvi_do_pwroff: Final RTC_EN_PWR_WAKEUP: 0x%08x\n",
		readl(REG_RTC_BASE + RTC_EN_PWR_WAKEUP));
	pr_info("cvi_do_pwroff: Final RTC_EN_PWR_VBAT_DET: 0x%08x\n",
		readl(REG_RTC_BASE + RTC_EN_PWR_VBAT_DET));
	pr_info("cvi_do_pwroff: Final RTC_EN_PWR_CYC_REQ: 0x%08x\n",
		readl(REG_RTC_BASE + RTC_EN_PWR_CYC_REQ));

	pr_info("cvi_do_pwroff: Triggering shutdown via RTC_CTRL0\n");

	writel(0xFFFF0800 | (0x1 << 0), REG_RTC_CTRL_BASE + RTC_CTRL0);

	/* Wait some time until system down, otherwise, notice with a warn */
	mdelay(1000);

	WARN_ONCE(1, "Unable to power off system\n");
}

static struct notifier_block cvi_restart_nb = {
	.notifier_call = cvi_restart_handler,
	.priority = 128,
};

static int cvi_reboot_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	int err;

	base = of_iomap(np, 0);
	if (!base) {
		WARN(1, "failed to map base address");
		return -ENODEV;
	}

	err = register_restart_handler(&cvi_restart_nb);
	if (err) {
		dev_err(&pdev->dev, "cannot register restart handler (err=%d)\n",
			err);
		iounmap(base);
	}

	pm_power_off = &cvi_do_pwroff;

	return err;
}

static const struct of_device_id cvi_reboot_of_match[] = {
	{ .compatible = "cvitek,restart" },
	{}
};

static struct platform_driver cvi_reboot_driver = {
	.probe = cvi_reboot_probe,
	.driver = {
		.name = "cvi-reboot",
		.of_match_table = cvi_reboot_of_match,
	},
};
module_platform_driver(cvi_reboot_driver);
