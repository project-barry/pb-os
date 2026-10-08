// SPDX-License-Identifier: GPL-2.0
/*
 * pcid3dbg - TEST ONLY diagnostic for the KONKR Pocket FIT deep-sleep work.
 *
 * Late in system suspend (syscore phase: after every device's noirq
 * callback, CPUs already offlined) log each PCI device's power state and
 * evaluate the same rule as pci-host-common's
 * __pci_host_common_d3cold_possible(): a host bridge only powers its link
 * down (dw_pcie_suspend_noirq -> pci->suspended) if every active device on it
 * is in D3hot and every wakeup-enabled one can signal PME from D3cold.
 * Read-only: it changes no device state. insmod/rmmod, never installed.
 */
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pci.h>
#include <linux/pm_wakeup.h>
#include <linux/suspend.h>
#include <linux/syscore_ops.h>

#define MAXDEV 32
static struct pci_dev *devs[MAXDEV];
static bool forced[MAXDEV];
static int ndev;

/*
 * TEST ONLY: the Qualcomm root ports (17cb:0113) advertise HotPlug+, so
 * pci_bridge_d3_possible() refuses D3 (pciehp is not built in) and their
 * links stay up through S2RAM. force_bridge_d3=1 allows D3 on them until
 * rmmod, to test whether that is what blocks CX collapse / DDR self-refresh.
 */
static bool force_bridge_d3;
module_param(force_bridge_d3, bool, 0444);
/* -1 = every QCOM root port, else only the one in this PCI domain (0 Wi-Fi, 1 Renesas on the KPF) */
static int only_domain = -1;
module_param(only_domain, int, 0444);

static const char *st(pci_power_t s)
{
	switch (s) {
	case PCI_D0: return "D0";
	case PCI_D1: return "D1";
	case PCI_D2: return "D2";
	case PCI_D3hot: return "D3hot";
	case PCI_D3cold: return "D3cold";
	default: return "unknown";
	}
}

static void dump(const char *when)
{
	int i;

	for (i = 0; i < ndev; i++) {
		struct pci_dev *p = devs[i];
		bool active = p->dev.driver || pci_is_enabled(p);
		bool wake = device_may_wakeup(&p->dev);
		bool pme3c = pci_pme_capable(p, PCI_D3cold);
		const char *why = "ok";

		if (!active)
			why = "ok (inactive: ignored)";
		else if (p->current_state != PCI_D3hot)
			why = "BLOCKS (not in D3hot)";
		else if (wake && !pme3c)
			why = "BLOCKS (wakeup but no PME from D3cold)";

		pr_info("pcid3dbg %s: %s [%04x:%04x] drv=%s enabled=%d state=%s wakeup=%d pme_d3cold=%d bridge_d3=%d no_d3cold=%d -> %s\n",
			when, pci_name(p), p->vendor, p->device,
			p->dev.driver ? p->dev.driver->name : "-",
			atomic_read(&p->enable_cnt), st(p->current_state),
			wake, pme3c, p->bridge_d3, p->no_d3cold, why);
	}
}

/*
 * TEST ONLY: msm_dsi_phy manages its "iface" clock (disp_cc_mdss_ahb_clk)
 * with pm_clk, which prepares it once and only disables it on runtime
 * suspend. A prepared clock under bi_tcxo keeps the RPMh XO vote, so the
 * SoC can never reach XO shutdown. fix_ahb=1 unprepares it in suspend_noirq
 * (display already off, clock disabled) and prepares it back in
 * resume_noirq. This module's device is registered last, so its noirq
 * suspend runs first and its noirq resume runs last.
 */
static bool fix_ahb;
module_param(fix_ahb, bool, 0444);
static struct clk *ahb;
static int ahb_unprepared;
static struct platform_device *pdev;

static int dbg_suspend_noirq(struct device *dev)
{
	struct clk_hw *hw;

	if (!ahb)
		return 0;
	hw = __clk_get_hw(ahb);
	while (ahb_unprepared < 4 && clk_hw_is_prepared(hw) &&
	       !clk_hw_is_enabled(hw)) {
		clk_unprepare(ahb);
		ahb_unprepared++;
	}
	pr_info("pcid3dbg: %s unprepared x%d (prepared=%d enabled=%d)\n",
		__clk_get_name(ahb), ahb_unprepared, clk_hw_is_prepared(hw),
		clk_hw_is_enabled(hw));
	return 0;
}

static int dbg_resume_noirq(struct device *dev)
{
	while (ahb && ahb_unprepared) {
		if (clk_prepare(ahb))
			pr_err("pcid3dbg: re-prepare failed\n");
		ahb_unprepared--;
	}
	return 0;
}

static const struct dev_pm_ops dbg_pm = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(dbg_suspend_noirq, dbg_resume_noirq)
};
static struct platform_driver dbg_drv = {
	.driver = { .name = "pcid3dbg", .pm = &dbg_pm },
};

/*
 * TEST ONLY: log the GIC SPIs pending at syscore suspend and resume. In deep
 * (PSCI system suspend) any interrupt reaching the CPUs resumes the whole
 * system without a Linux wakeup record (IRQF_NO_SUSPEND ones like
 * glink-smem/ipcc); whatever is pending at syscore resume is what woke it.
 */
#define GICD_BASE	0x17100000
#define GICD_TYPER	0x0004
#define GICD_ISPENDR	0x0200
static void __iomem *gicd;

static void gic_pending(const char *when)
{
	u32 lines, i;
	int n = 0;

	if (!gicd)
		return;
	lines = ((readl_relaxed(gicd + GICD_TYPER) & 0x1f) + 1) * 32;
	for (i = 32; i < lines && i < 1020; i += 32) {
		u32 v = readl_relaxed(gicd + GICD_ISPENDR + i / 8);

		while (v) {
			int b = __ffs(v);

			pr_info("pcid3dbg %s: GIC SPI pending hwirq %u (GICv3 %u in /proc/interrupts)\n",
				when, i + b, i + b);
			v &= v - 1;
			n++;
		}
	}
	pr_info("pcid3dbg %s: %d GIC SPI(s) pending\n", when, n);
}

/*
 * TEST ONLY: mask one Linux IRQ (the IPCC mailbox, IRQF_NO_SUSPEND) for the
 * deep-suspend window only. The ADSP's pmic_glink battery notifications
 * otherwise resume the whole system from PSCI system suspend; while masked
 * they stay pending and are handled right after a real wake.
 */
static int mask_irq = -1;
module_param(mask_irq, int, 0444);
static bool masked;

/*
 * TEST ONLY: list the TLMM GPIOs driven high (output enabled, OUT=1) at
 * syscore suspend, read straight from the TLMM registers (no pinctrl calls,
 * nothing runtime-resumed). Board rails and enables left on show up here.
 */
#define TLMM_BASE	0x0f100000
#define TLMM_NGPIO	210
static void __iomem *tlmm;

static void gpio_scan(const char *when)
{
	char buf[512] = "";
	int i, n = 0, len = 0;

	if (!tlmm)
		return;
	for (i = 0; i < TLMM_NGPIO; i++) {
		/* gpio-reserved-ranges (secure): reading them faults */
		if ((i >= 32 && i <= 36) || (i >= 38 && i <= 43) || i == 74)
			continue;
		{
		u32 cfg = readl_relaxed(tlmm + i * 0x1000);
		u32 io = readl_relaxed(tlmm + i * 0x1000 + 4);

		if ((cfg & BIT(9)) && (io & BIT(1))) {
			len += scnprintf(buf + len, sizeof(buf) - len, " %d(f%u)", i,
					 (cfg >> 2) & 0xf);
			n++;
		}
		}
	}
	pr_info("pcid3dbg %s: %d GPIOs driven high:%s\n", when, n, buf);
}

/*
 * TEST ONLY: drive these TLMM GPIOs low for the deep-sleep window (syscore
 * suspend .. syscore resume, i.e. after every device suspended and before
 * any resumes), then restore them. For board rails left on in sleep.
 */
static int off_gpios[16];
static int n_off_gpios;
module_param_array(off_gpios, int, &n_off_gpios, 0444);
static u32 off_saved[16];
static int off_delay_ms = 20;
module_param(off_delay_ms, int, 0444);

static void gpios_off(void)
{
	int i;

	for (i = 0; tlmm && i < n_off_gpios; i++) {
		void __iomem *io = tlmm + off_gpios[i] * 0x1000 + 4;

		off_saved[i] = readl_relaxed(io);
		writel_relaxed(off_saved[i] & ~BIT(1), io);
	}
}

static void gpios_restore(void)
{
	int i, n = 0;

	for (i = 0; tlmm && i < n_off_gpios; i++) {
		void __iomem *io = tlmm + off_gpios[i] * 0x1000 + 4;

		if (off_saved[i] & BIT(1)) {
			writel_relaxed(readl_relaxed(io) | BIT(1), io);
			n++;
		}
	}
	if (n)
		mdelay(off_delay_ms);
}

static int pcid3dbg_suspend(void *data)
{
	gpio_scan("syscore-suspend");
	if (pm_suspend_target_state == PM_SUSPEND_MEM)
		gpios_off();
	dump("syscore-suspend");
	if (mask_irq > 0 && pm_suspend_target_state == PM_SUSPEND_MEM) {
		disable_irq_nosync(mask_irq);
		masked = true;
	}
	gic_pending("syscore-suspend");
	return 0;
}

static void pcid3dbg_resume(void *data)
{
	gpios_restore();
	gic_pending("syscore-resume");
	if (masked) {
		masked = false;
		enable_irq(mask_irq);
		pr_info("pcid3dbg: irq %d unmasked after resume\n", mask_irq);
	}
}

static const struct syscore_ops ops = {
	.suspend = pcid3dbg_suspend,
	.resume = pcid3dbg_resume,
};
static struct syscore sc = { .ops = &ops };

static int pm_cb(struct notifier_block *nb, unsigned long ev, void *u)
{
	if (ev == PM_SUSPEND_PREPARE)
		dump("prepare");
	return NOTIFY_DONE;
}
static struct notifier_block nb = { .notifier_call = pm_cb };

static int __init pcid3dbg_init(void)
{
	struct pci_dev *p = NULL;
	int i;

	while ((p = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, p)) && ndev < MAXDEV)
		devs[ndev++] = pci_dev_get(p);
	pci_dev_put(p);
	for (i = 0; i < ndev; i++) {
		p = devs[i];
		if (force_bridge_d3 && p->vendor == PCI_VENDOR_ID_QCOM &&
		    pci_pcie_type(p) == PCI_EXP_TYPE_ROOT_PORT && !p->bridge_d3 &&
		    (only_domain < 0 || pci_domain_nr(p->bus) == only_domain)) {
			p->bridge_d3 = 1;
			forced[i] = true;
			pr_info("pcid3dbg: %s bridge_d3 forced on\n", pci_name(p));
		}
	}
	if (fix_ahb) {
		struct device *phy = bus_find_device_by_name(&platform_bus_type,
							     NULL, "ae95000.phy");
		if (phy) {
			ahb = clk_get(phy, "iface");
			put_device(phy);
			if (IS_ERR(ahb))
				ahb = NULL;
		}
		pr_info("pcid3dbg: fix_ahb clock %s\n",
			ahb ? __clk_get_name(ahb) : "NOT FOUND");
		if (!platform_driver_register(&dbg_drv))
			pdev = platform_device_register_simple("pcid3dbg", -1,
							       NULL, 0);
	}
	gicd = ioremap(GICD_BASE, 0x10000);
	tlmm = ioremap(TLMM_BASE, TLMM_NGPIO * 0x1000);
	gpio_scan("load");
	register_syscore(&sc);
	register_pm_notifier(&nb);
	pr_info("pcid3dbg: watching %d PCI devices\n", ndev);
	dump("load");
	return 0;
}

static void __exit pcid3dbg_exit(void)
{
	unregister_pm_notifier(&nb);
	unregister_syscore(&sc);
	if (gicd)
		iounmap(gicd);
	if (tlmm)
		iounmap(tlmm);
	if (pdev) {
		platform_device_unregister(pdev);
		platform_driver_unregister(&dbg_drv);
	}
	if (ahb)
		clk_put(ahb);
	while (ndev) {
		ndev--;
		if (forced[ndev])
			devs[ndev]->bridge_d3 = 0;
		pci_dev_put(devs[ndev]);
	}
}

module_init(pcid3dbg_init);
module_exit(pcid3dbg_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TEST ONLY: log PCI D3 state late in suspend");
