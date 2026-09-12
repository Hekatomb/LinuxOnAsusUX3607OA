// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm Glymur PMGK RTC driver (out-of-tree prototype)
 *
 * Author: Troels Vognbjerg
 *
 * Read-only prototype for the ASUS Zenbook A16 UX3607OA and UX3407NA
 *
 * PMGK protocol used here:
 *   owner  = 0x8011
 *   type   = PMIC_GLINK_REQ_RESP
 *   opcode = 0x90 (read)
 *
 * Read request:
 *   struct pmic_glink_hdr
 *   u32 reserved
 *   u32 offset
 *   u32 length
 *
 * Real-time data lives at PMGK offset 0x30200 and is the 16-byte ACPI
 * Time and Alarm Device real-time structure returned by _GRT.
 */

#include <linux/completion.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/rtc.h>
#include <linux/slab.h>
#include <linux/soc/qcom/pmic_glink.h>
#include <linux/soc/qcom/pdr.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#define GLYMUR_PMGK_OWNER		0x8011
#define GLYMUR_PMGK_READ_OPCODE		0x90
#define GLYMUR_PMGK_XRTD		0x00030200
#define GLYMUR_PMGK_XRTD_LEN		0x10
#define GLYMUR_PMGK_TIMEOUT		HZ

struct glymur_pmgk_read_req {
	struct pmic_glink_hdr hdr;
	__le32 reserved;
	__le32 offset;
	__le32 length;
} __packed;

/* ACPI Time and Alarm Device real-time buffer returned by _GRT. */
struct glymur_acpi_real_time {
	__le16 year;
	u8 month;
	u8 day;
	u8 hour;
	u8 minute;
	u8 second;
	u8 valid;
	__le16 milliseconds;
	__le16 timezone;
	u8 daylight;
	u8 reserved[3];
} __packed;

struct glymur_pmgk_rtc {
	struct device *dev;
	struct pmic_glink_client *client;
	struct rtc_device *rtc;

	struct completion done;
	struct mutex request_lock;
	spinlock_t state_lock;

	bool request_pending;
	bool service_up;
	bool rtc_registered;
	int request_result;
	struct glymur_acpi_real_time response;
	struct delayed_work register_work;
};

static struct platform_device *glymur_rtc_pdev;

static void glymur_pmgk_rx(const void *data, size_t len, void *priv)
{
	struct glymur_pmgk_rtc *grtc = priv;
	const struct pmic_glink_hdr *hdr = data;
	unsigned long flags;

	if (len < sizeof(*hdr))
		return;

	if (le32_to_cpu(hdr->owner) != GLYMUR_PMGK_OWNER ||
	    le32_to_cpu(hdr->type) != PMIC_GLINK_REQ_RESP ||
	    le32_to_cpu(hdr->opcode) != GLYMUR_PMGK_READ_OPCODE)
		return;

	spin_lock_irqsave(&grtc->state_lock, flags);
	if (!grtc->request_pending) {
		spin_unlock_irqrestore(&grtc->state_lock, flags);
		return;
	}

	if (len < sizeof(*hdr) + sizeof(grtc->response)) {
		grtc->request_result = -EMSGSIZE;
	} else {
		/*
		 * The Windows-style PMGK read returns a 1040-byte response.
		 * The requested XRTD bytes begin immediately after the
		 * 12-byte pmic_glink_hdr, i.e. at offset 0x0c.
		 */
		memcpy(&grtc->response, (const u8 *)data + sizeof(*hdr),
		       sizeof(grtc->response));
		grtc->request_result = 0;
	}

	grtc->request_pending = false;
	complete(&grtc->done);
	spin_unlock_irqrestore(&grtc->state_lock, flags);
}

static void glymur_pmgk_pdr_notify(void *priv, int state)
{
	struct glymur_pmgk_rtc *grtc = priv;
	unsigned long flags;
	bool up = state == SERVREG_SERVICE_STATE_UP;

	spin_lock_irqsave(&grtc->state_lock, flags);
	grtc->service_up = up;
	if (!up && grtc->request_pending) {
		grtc->request_result = -ECONNRESET;
		grtc->request_pending = false;
		complete(&grtc->done);
	}
	spin_unlock_irqrestore(&grtc->state_lock, flags);

	dev_dbg(grtc->dev, "PMIC GLink state=%#x\n", state);

	if (up)
		schedule_delayed_work(&grtc->register_work, 0);
}

static int glymur_pmgk_read_real_time(struct glymur_pmgk_rtc *grtc,
				     struct glymur_acpi_real_time *time)
{
	struct glymur_pmgk_read_req req = {
		.hdr = {
			.owner = cpu_to_le32(GLYMUR_PMGK_OWNER),
			.type = cpu_to_le32(PMIC_GLINK_REQ_RESP),
			.opcode = cpu_to_le32(GLYMUR_PMGK_READ_OPCODE),
		},
		.reserved = cpu_to_le32(0),
		.offset = cpu_to_le32(GLYMUR_PMGK_XRTD),
		.length = cpu_to_le32(GLYMUR_PMGK_XRTD_LEN),
	};
	unsigned long flags;
	unsigned long left;
	int ret;

	mutex_lock(&grtc->request_lock);

	reinit_completion(&grtc->done);

	spin_lock_irqsave(&grtc->state_lock, flags);
	grtc->request_pending = true;
	grtc->request_result = -EINPROGRESS;
	spin_unlock_irqrestore(&grtc->state_lock, flags);

	ret = pmic_glink_send(grtc->client, &req, sizeof(req));
	if (ret) {
		spin_lock_irqsave(&grtc->state_lock, flags);
		grtc->request_pending = false;
		spin_unlock_irqrestore(&grtc->state_lock, flags);
		goto out_unlock;
	}

	left = wait_for_completion_timeout(&grtc->done, GLYMUR_PMGK_TIMEOUT);
	if (!left) {
		spin_lock_irqsave(&grtc->state_lock, flags);
		grtc->request_pending = false;
		spin_unlock_irqrestore(&grtc->state_lock, flags);
		ret = -ETIMEDOUT;
		goto out_unlock;
	}

	spin_lock_irqsave(&grtc->state_lock, flags);
	ret = grtc->request_result;
	if (!ret)
		*time = grtc->response;
	spin_unlock_irqrestore(&grtc->state_lock, flags);

out_unlock:
	mutex_unlock(&grtc->request_lock);
	return ret;
}

static int glymur_pmgk_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct glymur_pmgk_rtc *grtc = dev_get_drvdata(dev);
	struct glymur_acpi_real_time raw;
	struct rtc_time local_tm;
	time64_t time;
	s16 timezone;
	int ret;

	ret = glymur_pmgk_read_real_time(grtc, &raw);
	if (ret)
		return ret;

	/*
	 * Glymur's raw XRTD record has been observed with byte[7] == 0
	 * while the date/time fields are valid.  Do not apply the ACPI _GRT
	 * Valid semantics to the raw PMGK record.
	 */

	local_tm.tm_year = le16_to_cpu(raw.year) - 1900;
	local_tm.tm_mon = raw.month - 1;
	local_tm.tm_mday = raw.day;
	local_tm.tm_hour = raw.hour;
	local_tm.tm_min = raw.minute;
	local_tm.tm_sec = raw.second;

	ret = rtc_valid_tm(&local_tm);
	if (ret)
		return ret;

	timezone = (s16)le16_to_cpu(raw.timezone);
	if (timezone == 0x07ff)
		return -ENODATA;

	/*
	 * XRTD stores local time.  Its timezone field is the offset,
	 * in minutes, which must be added to local time to obtain UTC.
	 * The RTC class expects read_time() to return UTC.
	 */
	time = rtc_tm_to_time64(&local_tm);
	time += (time64_t)timezone * 60;
	rtc_time64_to_tm(time, tm);

	dev_dbg(grtc->dev,
		"RTC local %04d-%02d-%02d %02d:%02d:%02d.%03u timezone=%d -> UTC %04d-%02d-%02d %02d:%02d:%02d raw_valid=%u daylight=%#x\n",
		 local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday,
		 local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec,
		 le16_to_cpu(raw.milliseconds), timezone,
		 tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
		 tm->tm_hour, tm->tm_min, tm->tm_sec, raw.valid, raw.daylight);

	return 0;
}

static const struct rtc_class_ops glymur_pmgk_rtc_ops = {
	.read_time = glymur_pmgk_rtc_read_time,
};

static void glymur_pmgk_register_work(struct work_struct *work)
{
	struct glymur_pmgk_rtc *grtc =
		container_of(to_delayed_work(work), struct glymur_pmgk_rtc,
			     register_work);
	struct glymur_acpi_real_time raw;
	unsigned long flags;
	bool service_up;
	int ret;

	spin_lock_irqsave(&grtc->state_lock, flags);
	service_up = grtc->service_up;
	if (grtc->rtc_registered) {
		spin_unlock_irqrestore(&grtc->state_lock, flags);
		return;
	}
	spin_unlock_irqrestore(&grtc->state_lock, flags);

	if (!service_up)
		return;

	/*
	 * Do not expose rtc0 until the firmware has answered at least one
	 * XRTD request.  Registering the RTC triggers CONFIG_RTC_HCTOSYS
	 * immediately. 
	 */
	ret = glymur_pmgk_read_real_time(grtc, &raw);
	if (ret) {
		spin_lock_irqsave(&grtc->state_lock, flags);
		service_up = grtc->service_up;
		spin_unlock_irqrestore(&grtc->state_lock, flags);

		dev_dbg(grtc->dev, "RTC preflight read failed: %d\n", ret);
		if (service_up)
			schedule_delayed_work(&grtc->register_work,
					      msecs_to_jiffies(250));
		return;
	}

	ret = devm_rtc_register_device(grtc->rtc);
	if (ret) {
		dev_err(grtc->dev, "failed to register RTC: %d\n", ret);
		return;
	}

	spin_lock_irqsave(&grtc->state_lock, flags);
	grtc->rtc_registered = true;
	spin_unlock_irqrestore(&grtc->state_lock, flags);
}

static int glymur_pmgk_create_rtc(struct device *dev)
{
	struct glymur_pmgk_rtc *grtc;

	grtc = devm_kzalloc(dev, sizeof(*grtc), GFP_KERNEL);
	if (!grtc)
		return -ENOMEM;

	grtc->dev = dev;
	init_completion(&grtc->done);
	mutex_init(&grtc->request_lock);
	spin_lock_init(&grtc->state_lock);
	INIT_DELAYED_WORK(&grtc->register_work, glymur_pmgk_register_work);
	dev_set_drvdata(dev, grtc);

	/*
	 * Initialise the RTC before registering the GLink client. 
	 */
	grtc->rtc = devm_rtc_allocate_device(dev);
	if (IS_ERR(grtc->rtc))
		return PTR_ERR(grtc->rtc);

	grtc->rtc->ops = &glymur_pmgk_rtc_ops;
	grtc->rtc->range_min = RTC_TIMESTAMP_BEGIN_1900;
	grtc->rtc->range_max = RTC_TIMESTAMP_END_9999;

	grtc->client = devm_pmic_glink_client_alloc(dev,
						    GLYMUR_PMGK_OWNER,
						    glymur_pmgk_rx,
						    glymur_pmgk_pdr_notify,
						    grtc);
	if (IS_ERR(grtc->client))
		return dev_err_probe(dev, PTR_ERR(grtc->client),
				     "failed to allocate PMGK GLink client\n");

	pmic_glink_client_register(grtc->client);
	return 0;
}

static int __init glymur_pmgk_init(void)
{
	struct platform_device *pmic_pdev;
	struct device_node *np;
	int ret;

	np = of_find_compatible_node(NULL, NULL, "qcom,pmic-glink");
	if (!np) {
		pr_err("rtc_glymur_pmgk: qcom,pmic-glink DT node not found\n");
		return -ENODEV;
	}

	pmic_pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pmic_pdev) {
		pr_err("rtc_glymur_pmgk: PMIC GLink platform device not found\n");
		return -ENODEV;
	}

	glymur_rtc_pdev = platform_device_alloc("rtc-glymur-pmgk",
						PLATFORM_DEVID_NONE);
	if (!glymur_rtc_pdev) {
		put_device(&pmic_pdev->dev);
		return -ENOMEM;
	}

	glymur_rtc_pdev->dev.parent = &pmic_pdev->dev;

	ret = platform_device_add(glymur_rtc_pdev);
	put_device(&pmic_pdev->dev);
	if (ret) {
		platform_device_put(glymur_rtc_pdev);
		glymur_rtc_pdev = NULL;
		return ret;
	}

	ret = glymur_pmgk_create_rtc(&glymur_rtc_pdev->dev);
	if (ret) {
		platform_device_unregister(glymur_rtc_pdev);
		glymur_rtc_pdev = NULL;
		return ret;
	}

	return 0;
}

static void __exit glymur_pmgk_exit(void)
{
	struct glymur_pmgk_rtc *grtc;

	if (!glymur_rtc_pdev)
		return;

	grtc = dev_get_drvdata(&glymur_rtc_pdev->dev);
	if (grtc)
		cancel_delayed_work_sync(&grtc->register_work);

	platform_device_unregister(glymur_rtc_pdev);
}

module_init(glymur_pmgk_init);
module_exit(glymur_pmgk_exit);

MODULE_DESCRIPTION("Qualcomm Glymur PMGK RTC out-of-tree prototype");
MODULE_AUTHOR("Troels Vognbjerg");
MODULE_LICENSE("GPL");
