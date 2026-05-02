// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hasivo HS104 PoE PSE controller driver
 *
 * The HS104PTI/HS104PBI are single-chip PoE PSE controllers managing 4
 * delivery channels, allowing them to supply 4 ports of 802.3af/at/bt power.
 * The HS104PTI can have 1x 802.3bt port and 3x 802.3at ports.
 * The HS104PBI can have 4x 802.3bt ports.
 *
 * Copyright (c) 2025 Bevan Weiss <bevan.weiss@gmail.com>
 * Copyright (c) 2026 Carlo Szelinsky <github@szelinsky.de>
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pse-pd/pse.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define HS104_MAX_PORTS		4

/* Register map */
#define HS104_REG_PW_STATUS	0x01	/* Power delivery status */
#define HS104_REG_INPUT_V	0x02	/* Input voltage, 16-bit BE, 10mV */
#define HS104_REG_PORT0_I	0x04	/* Port current, 16-bit BE, 1mA (not yet implemented) */
#define HS104_REG_DEVID		0x0C	/* Device ID */
#define HS104_REG_PORT0_CLASS	0x0D	/* Port power class */
#define HS104_REG_PW_EN		0x14	/* Port enable control */
#define HS104_REG_PROTOCOL	0x19	/* Protocol per port (2 bits each) */
#define HS104_REG_TOTAL_POWER	0x1D	/* Total power, 16-bit BE, 10mW (not yet implemented) */
#define HS104_REG_PORT0_POWER	0x21	/* Port power, 16-bit BE, 10mW */

#define HS104_DEVICE_ID		0x91
#define HS104_EXECUTE		0x40	/* Execute bit for writes */

/*
 * BIT-field registers (PW_EN, PW_STATUS) use reversed bit ordering
 * relative to sequential PORT registers (PORT0_POWER, PORT0_CLASS).
 * Map PSE PI index to the hardware bit position.
 */
#define HS104_PORT_BIT(id)	BIT(HS104_MAX_PORTS - 1 - (id))

/* Protocol encoding (2 bits per port in PROTOCOL register) */
#define HS104_PROTO_MASK	0x3
#define HS104_PROTO_BT		0	/* 802.3bt - 60W */
#define HS104_PROTO_HIPO	1	/* Hi-PoE - 90W */
#define HS104_PROTO_AT		2	/* 802.3at - 30W */
#define HS104_PROTO_AF		3	/* 802.3af - 15.4W */

/* Power limits in mW */
#define HS104_PW_AF		15400
#define HS104_PW_AT		30000
#define HS104_PW_BT		60000
#define HS104_PW_HIPO		90000

/* Unit conversion steps */
#define HS104_UV_STEP		10000	/* 10mV -> uV */
#define HS104_UA_STEP		1000	/* 1mA -> uA */
#define HS104_MW_STEP		10	/* 10mW -> mW */

struct hs104_priv {
	struct regmap		*regmap;
	struct regmap		*led_regmap;
	struct i2c_client	*led_client;
	struct pse_controller_dev pcdev;
	struct delayed_work	led_work;
	unsigned long		led_poll_interval;
	u8			led_reg;
	u8			led_exec_bit;
	u8			led_masks[HS104_MAX_PORTS];
};

static inline struct hs104_priv *to_hs104(struct pse_controller_dev *pcdev)
{
	return container_of(pcdev, struct hs104_priv, pcdev);
}

static bool hs104_has_poe_leds(struct hs104_priv *priv)
{
	return !!priv->led_regmap;
}

static int hs104_write_poe_leds(struct hs104_priv *priv, unsigned int status)
{
	unsigned int val, led_mask_all = 0, led_val = 0;
	int id, ret;

	if (!hs104_has_poe_leds(priv))
		return 0;

	ret = regmap_read(priv->led_regmap, priv->led_reg, &val);
	if (ret)
		return ret;

	for (id = 0; id < HS104_MAX_PORTS; id++) {
		led_mask_all |= priv->led_masks[id];
		if (status & HS104_PORT_BIT(id))
			led_val |= priv->led_masks[id];
	}

	/*
	 * Stock F1100WP userspace forces the high nibble to the execute bit
	 * (0x40) and then updates the low nibble with the per-port LED bits.
	 */
	val = (val & ~0xf0) | priv->led_exec_bit;
	val = (val & ~led_mask_all) | led_val;

	return regmap_write(priv->led_regmap, priv->led_reg, val);
}

static void hs104_sync_poe_leds(struct hs104_priv *priv)
{
	unsigned int val, status;
	int ret;

	if (!hs104_has_poe_leds(priv))
		return;

	ret = regmap_read(priv->regmap, HS104_REG_PW_STATUS, &val);
	if (ret)
		return;

	status = val & GENMASK(HS104_MAX_PORTS - 1, 0);
	hs104_write_poe_leds(priv, status);
}

static void hs104_queue_led_sync(struct hs104_priv *priv, unsigned long delay)
{
	if (!hs104_has_poe_leds(priv))
		return;

	mod_delayed_work(system_wq, &priv->led_work, delay);
}

static void hs104_led_work_fn(struct work_struct *work)
{
	struct hs104_priv *priv = container_of(to_delayed_work(work),
					       struct hs104_priv, led_work);

	hs104_sync_poe_leds(priv);
	if (priv->led_poll_interval)
		hs104_queue_led_sync(priv, priv->led_poll_interval);
}

/* Read 16-bit big-endian register and apply unit conversion */
static int hs104_read_be16(struct hs104_priv *priv, unsigned int reg,
			   unsigned int step)
{
	__be16 val;
	int ret;

	ret = regmap_bulk_read(priv->regmap, reg, &val, sizeof(val));
	if (ret)
		return ret;

	/* Hardware uses 14-bit data field, upper 2 bits are reserved */
	return (be16_to_cpu(val) & 0x3fff) * step;
}

/* Convert protocol code to power limit in mW */
static int hs104_proto_to_mw(unsigned int proto)
{
	switch (proto) {
	case HS104_PROTO_AF:	return HS104_PW_AF;
	case HS104_PROTO_AT:	return HS104_PW_AT;
	case HS104_PROTO_BT:	return HS104_PW_BT;
	case HS104_PROTO_HIPO:	return HS104_PW_HIPO;
	default:		return 0;
	}
}

/* PSE controller operations */

static int hs104_pi_enable(struct pse_controller_dev *pcdev, int id)
{
	struct hs104_priv *priv = to_hs104(pcdev);
	unsigned int val;
	int ret;

	ret = regmap_read(priv->regmap, HS104_REG_PW_EN, &val);
	if (ret)
		return ret;

	val = (val | HS104_PORT_BIT(id)) | HS104_EXECUTE;

	dev_dbg(pcdev->dev, "PI %d: enable (0x%02x)\n", id, val);
	ret = regmap_write(priv->regmap, HS104_REG_PW_EN, val);
	if (!ret)
		hs104_queue_led_sync(priv, 0);

	return ret;
}

static int hs104_pi_disable(struct pse_controller_dev *pcdev, int id)
{
	struct hs104_priv *priv = to_hs104(pcdev);
	unsigned int val;
	int ret;

	ret = regmap_read(priv->regmap, HS104_REG_PW_EN, &val);
	if (ret)
		return ret;

	val = (val & ~HS104_PORT_BIT(id)) | HS104_EXECUTE;

	dev_dbg(pcdev->dev, "PI %d: disable (0x%02x)\n", id, val);
	ret = regmap_write(priv->regmap, HS104_REG_PW_EN, val);
	if (!ret)
		hs104_queue_led_sync(priv, 0);

	return ret;
}

static int hs104_pi_get_voltage(struct pse_controller_dev *pcdev, int id)
{
	struct hs104_priv *priv = to_hs104(pcdev);

	/* Input voltage is shared across all PIs */
	return hs104_read_be16(priv, HS104_REG_INPUT_V, HS104_UV_STEP);
}

static int hs104_pi_get_pw_limit(struct pse_controller_dev *pcdev, int id)
{
	struct hs104_priv *priv = to_hs104(pcdev);
	unsigned int val, proto;
	int ret;

	ret = regmap_read(priv->regmap, HS104_REG_PROTOCOL, &val);
	if (ret)
		return ret;

	proto = (val >> (id * 2)) & HS104_PROTO_MASK;
	return hs104_proto_to_mw(proto) ?: -ENODATA;
}

static int hs104_mw_to_proto(int max_mw)
{
	if (max_mw <= HS104_PW_AF)
		return HS104_PROTO_AF;
	if (max_mw <= HS104_PW_AT)
		return HS104_PROTO_AT;
	if (max_mw <= HS104_PW_BT)
		return HS104_PROTO_BT;
	if (max_mw <= HS104_PW_HIPO)
		return HS104_PROTO_HIPO;
	return -EINVAL;
}

static int hs104_pi_set_pw_limit(struct pse_controller_dev *pcdev,
				 int id, int max_mw)
{
	struct hs104_priv *priv = to_hs104(pcdev);
	unsigned int val, proto, mask;
	int ret, pcode;

	pcode = hs104_mw_to_proto(max_mw);
	if (pcode < 0)
		return pcode;

	mask = HS104_PROTO_MASK << (id * 2);
	proto = (unsigned int)pcode << (id * 2);

	ret = regmap_read(priv->regmap, HS104_REG_PROTOCOL, &val);
	if (ret)
		return ret;

	val = (val & ~mask) | proto;

	dev_dbg(pcdev->dev, "PI %d: set pw_limit %d mW (proto=%u, reg=0x%02x)\n",
		id, max_mw, pcode, val);
	return regmap_write(priv->regmap, HS104_REG_PROTOCOL, val);
}

static int hs104_pi_get_admin_state(struct pse_controller_dev *pcdev, int id,
				    struct pse_admin_state *admin_state)
{
	struct hs104_priv *priv = to_hs104(pcdev);
	unsigned int val;
	int ret;

	ret = regmap_read(priv->regmap, HS104_REG_PW_EN, &val);
	if (ret) {
		admin_state->c33_admin_state =
			ETHTOOL_C33_PSE_ADMIN_STATE_UNKNOWN;
		return ret;
	}

	if (val & HS104_PORT_BIT(id))
		admin_state->c33_admin_state = ETHTOOL_C33_PSE_ADMIN_STATE_ENABLED;
	else
		admin_state->c33_admin_state = ETHTOOL_C33_PSE_ADMIN_STATE_DISABLED;

	return 0;
}

static int hs104_pi_get_pw_status(struct pse_controller_dev *pcdev, int id,
				  struct pse_pw_status *pw_status)
{
	struct hs104_priv *priv = to_hs104(pcdev);
	unsigned int val;
	int ret;

	ret = regmap_read(priv->regmap, HS104_REG_PW_STATUS, &val);
	if (ret) {
		pw_status->c33_pw_status = ETHTOOL_C33_PSE_PW_D_STATUS_UNKNOWN;
		return ret;
	}

	if (val & HS104_PORT_BIT(id))
		pw_status->c33_pw_status = ETHTOOL_C33_PSE_PW_D_STATUS_DELIVERING;
	else
		pw_status->c33_pw_status = ETHTOOL_C33_PSE_PW_D_STATUS_DISABLED;

	return 0;
}

static int hs104_pi_get_actual_pw(struct pse_controller_dev *pcdev, int id)
{
	struct hs104_priv *priv = to_hs104(pcdev);

	return hs104_read_be16(priv, HS104_REG_PORT0_POWER + id * 2,
			       HS104_MW_STEP);
}

static const struct ethtool_c33_pse_pw_limit_range hs104_pw_ranges[] = {
	{ .min = HS104_PW_AF,   .max = HS104_PW_AF },
	{ .min = HS104_PW_AT,   .max = HS104_PW_AT },
	{ .min = HS104_PW_BT,   .max = HS104_PW_BT },
	{ .min = HS104_PW_HIPO, .max = HS104_PW_HIPO },
};

static int hs104_pi_get_pw_limit_ranges(struct pse_controller_dev *pcdev,
					int id,
					struct pse_pw_limit_ranges *pw_limit_ranges)
{
	struct ethtool_c33_pse_pw_limit_range *c33_pw_limit_ranges;

	c33_pw_limit_ranges = kmemdup(hs104_pw_ranges, sizeof(hs104_pw_ranges),
				      GFP_KERNEL);
	if (!c33_pw_limit_ranges)
		return -ENOMEM;

	pw_limit_ranges->c33_pw_limit_ranges = c33_pw_limit_ranges;

	/* Return number of ranges */
	return ARRAY_SIZE(hs104_pw_ranges);
}

static const struct pse_controller_ops hs104_ops = {
	.pi_enable		= hs104_pi_enable,
	.pi_disable		= hs104_pi_disable,
	.pi_get_admin_state	= hs104_pi_get_admin_state,
	.pi_get_pw_status	= hs104_pi_get_pw_status,
	/*
	 * pi_get_pw_class disabled: the HS104 class register location is
	 * not yet known. The previous PORT0_CLASS+id mapping (0x0D+id)
	 * hit unrelated bytes for ports 1-3 and returned garbage
	 * (register 0x0D reads 0x00 even with a classified PD on port 0).
	 * Re-enable once the real layout is confirmed via datasheet or
	 * OEM bus trace.
	 */
	.pi_get_actual_pw	= hs104_pi_get_actual_pw,
	.pi_get_voltage		= hs104_pi_get_voltage,
	.pi_get_pw_limit	= hs104_pi_get_pw_limit,
	.pi_set_pw_limit	= hs104_pi_set_pw_limit,
	.pi_get_pw_limit_ranges	= hs104_pi_get_pw_limit_ranges,
};

/* Driver initialization */

static const struct regmap_config hs104_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static void hs104_init_poe_leds(struct i2c_client *client, struct hs104_priv *priv)
{
	struct device *dev = &client->dev;
	u32 led_addr, led_reg, led_exec = HS104_EXECUTE;
	u32 led_masks[HS104_MAX_PORTS];
	int ret, id;

	if (of_property_read_u32(dev->of_node, "hasivo,poe-led-i2c-addr",
				 &led_addr))
		return;

	ret = of_property_read_u32(dev->of_node, "hasivo,poe-led-reg", &led_reg);
	if (ret) {
		dev_warn(dev, "PoE LED controller configured without hasivo,poe-led-reg\n");
		return;
	}

	ret = of_property_read_u32_array(dev->of_node, "hasivo,poe-led-masks",
					 led_masks, HS104_MAX_PORTS);
	if (ret) {
		dev_warn(dev, "PoE LED controller configured without 4 hasivo,poe-led-masks values\n");
		return;
	}

	of_property_read_u32(dev->of_node, "hasivo,poe-led-execute-bit",
			     &led_exec);

	priv->led_client = devm_i2c_new_dummy_device(dev, client->adapter,
						      led_addr);
	if (IS_ERR(priv->led_client)) {
		dev_warn(dev, "Failed to create PoE LED I2C client at 0x%02x: %ld\n",
			 led_addr, PTR_ERR(priv->led_client));
		priv->led_client = NULL;
		return;
	}

	priv->led_regmap = devm_regmap_init_i2c(priv->led_client,
						&hs104_regmap_config);
	if (IS_ERR(priv->led_regmap)) {
		dev_warn(dev, "Failed to create PoE LED regmap: %ld\n",
			 PTR_ERR(priv->led_regmap));
		priv->led_regmap = NULL;
		return;
	}

	priv->led_reg = led_reg;
	priv->led_exec_bit = led_exec;
	for (id = 0; id < HS104_MAX_PORTS; id++)
		priv->led_masks[id] = led_masks[id];

	dev_info(dev, "PoE LED controller at 0x%02x reg 0x%02x initialized\n",
		 led_addr, priv->led_reg);
}

static int hs104_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct hs104_priv *priv;
	unsigned int devid;
	u32 led_poll_ms = 0;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -ENXIO;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->regmap = devm_regmap_init_i2c(client, &hs104_regmap_config);
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);

	/* Verify device ID */
	ret = regmap_read(priv->regmap, HS104_REG_DEVID, &devid);
	if (ret)
		return ret;

	if (devid != HS104_DEVICE_ID) {
		dev_err(dev, "Unknown device ID: 0x%02x\n", devid);
		return -ENODEV;
	}

	INIT_DELAYED_WORK(&priv->led_work, hs104_led_work_fn);
	i2c_set_clientdata(client, priv);
	hs104_init_poe_leds(client, priv);

	/*
	 * Enable all ports before registering the PSE controller, so that
	 * pse_pi_is_hw_enabled() sees the correct state when consumers
	 * request PSE controls. HS104 only delivers power to valid PDs.
	 */
	ret = regmap_write(priv->regmap, HS104_REG_PW_EN,
			   HS104_EXECUTE | GENMASK(HS104_MAX_PORTS - 1, 0));
	if (ret)
		dev_warn(dev, "Failed to enable ports: %d\n", ret);

	{
		u32 pw[HS104_MAX_PORTS];
		unsigned int before = 0, after = 0, val = 0;
		int i, n, pcode;

		regmap_read(priv->regmap, HS104_REG_PROTOCOL, &before);

		n = of_property_read_variable_u32_array(dev->of_node,
					"hasivo,port-max-power-mw",
					pw, 1, HS104_MAX_PORTS);
		if (n > 0) {
			for (i = 0; i < n; i++) {
				pcode = hs104_mw_to_proto(pw[i]);
				if (pcode < 0) {
					dev_warn(dev, "PI %d: invalid hasivo,port-max-power-mw=%u, skipping\n",
						 i, pw[i]);
					continue;
				}
				val |= ((unsigned int)pcode & HS104_PROTO_MASK)
					<< (i * 2);
			}
			/* Ports not covered by DT stay at their reset default. */
			if (n < HS104_MAX_PORTS)
				val |= before & ~GENMASK(n * 2 - 1, 0);

			ret = regmap_write(priv->regmap,
					   HS104_REG_PROTOCOL, val);
			if (ret)
				dev_warn(dev, "Failed to program PROTOCOL: %d\n",
					 ret);
		}

		regmap_read(priv->regmap, HS104_REG_PROTOCOL, &after);
		dev_info(dev, "PROTOCOL reg: before=0x%02x wrote=0x%02x after=0x%02x\n",
			 before, val, after);
	}

	/* One-shot register map dump for layout discovery. */
	{
		unsigned int r, v;
		char line[3 * 16 + 1];
		int col;

		for (r = 0x00; r < 0x30; r += 16) {
			line[0] = '\0';
			for (col = 0; col < 16; col++) {
				v = 0xffu;
				regmap_read(priv->regmap, r + col, &v);
				scnprintf(line + col * 3, 4, "%02x ", v & 0xff);
			}
			dev_info(dev, "regs 0x%02x-0x%02x: %s\n",
				 r, r + 15, line);
		}
	}

	/* Register PSE controller */
	priv->pcdev.ops = &hs104_ops;
	priv->pcdev.dev = dev;
	priv->pcdev.owner = THIS_MODULE;
	priv->pcdev.nr_lines = HS104_MAX_PORTS;
	priv->pcdev.of_pse_n_cells = 1;
	priv->pcdev.types = ETHTOOL_PSE_C33;

	ret = devm_pse_controller_register(dev, &priv->pcdev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to register PSE controller\n");

	of_property_read_u32(dev->of_node, "led-poll-interval-ms", &led_poll_ms);
	if (hs104_has_poe_leds(priv))
		hs104_sync_poe_leds(priv);
	if (hs104_has_poe_leds(priv) && led_poll_ms) {
		priv->led_poll_interval = msecs_to_jiffies(led_poll_ms);
		hs104_queue_led_sync(priv, priv->led_poll_interval);
	}

	dev_info(dev, "HS104 PSE controller initialized\n");
	return 0;
}

static void hs104_remove(struct i2c_client *client)
{
	struct hs104_priv *priv = i2c_get_clientdata(client);

	if (!priv)
		return;

	cancel_delayed_work_sync(&priv->led_work);
	hs104_write_poe_leds(priv, 0);
}

static const struct of_device_id hs104_of_match[] = {
	{ .compatible = "hasivo,hs104" },
	{ .compatible = "hasivo,hs104pti" },
	{ .compatible = "hasivo,hs104pbi" },
	{ }
};
MODULE_DEVICE_TABLE(of, hs104_of_match);

static struct i2c_driver hs104_driver = {
	.driver = {
		.name		= "hasivo-hs104",
		.of_match_table	= hs104_of_match,
	},
	.probe	= hs104_probe,
	.remove	= hs104_remove,
};
module_i2c_driver(hs104_driver);

MODULE_AUTHOR("Bevan Weiss <bevan.weiss@gmail.com>");
MODULE_AUTHOR("Carlo Szelinsky <github@szelinsky.de>");
MODULE_DESCRIPTION("Hasivo HS104 PoE PSE Controller");
MODULE_LICENSE("GPL");
