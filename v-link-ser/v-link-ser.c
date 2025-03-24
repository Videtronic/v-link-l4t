// SPDX-License-Identifier: GPL-2.0+
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/i2c-mux.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/regmap.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-device.h>

/* PAD0 - image sink */
#define V_LINK_SER_SINK_PAD0 0
#define V_LINK_SER_N_SINK_PADS 1

/* PAD1 - image source */
#define V_LINK_SER_SRC_PAD0 1
#define V_LINK_SER_N_SRC_PADS 1

#define V_LINK_SER_N_PADS (V_LINK_SER_N_SRC_PADS + V_LINK_SER_N_SINK_PADS)

#define MAX96717_NUM_GPIO 1
#define MAX96717_GPIO_REG_A(gpio) (0x2be + (gpio)*3)
#define MAX96717_GPIO_OUT BIT(4)
#define MAX96717_GPIO_IN BIT(3)
#define MAX96717_GPIO_RX_EN BIT(2)
#define MAX96717_GPIO_TX_EN BIT(1)
#define MAX96717_GPIO_OUT_DIS BIT(0)
#define MAX96717_MIPI_RX1 (0x331)
#define MAX96717_MIPI_LANES_CNT GENMASK(5, 4)
#define MAX96717_MIPI_RX2 (0x332)
#define MAX96717_PHY2_LANES_MAP GENMASK(7, 4)
#define MAX96717_MIPI_RX3 (0x333)
#define MAX96717_PHY1_LANES_MAP GENMASK(3, 0)
#define MAX96717_MIPI_RX4 (0x334)
#define MAX96717_PHY1_LANES_POL GENMASK(6, 4)
#define MAX96717_MIPI_RX5 (0x335)
#define MAX96717_PHY2_LANES_POL GENMASK(2, 0)
#define MAX96717_FRONTOP0 (0x308)
#define MAX96717_START_PORT_B BIT(5)

#define MAX96717_CSI_NLANES 4

#define MHZ(v) ((u32)((v)*1000000U))

struct v_link_ser_priv {
  struct i2c_client *client;
  struct i2c_mux_core *mux;
  struct regmap *regmap;
  struct gpio_chip gpio_chip;
  struct media_pad pads[V_LINK_SER_N_PADS];
  struct v4l2_subdev sd;
  struct v4l2_ctrl_handler ctrls;
  u32 bus_width;
};

static inline struct v_link_ser_priv *sd_to_v_link(struct v4l2_subdev *sd) {
  return container_of(sd, struct v_link_ser_priv, sd);
}

static int v_link_ser_i2c_mux_select(struct i2c_mux_core *mux, u32 chan)
{
	return 0;
}

static int v_link_ser_i2c_mux_init(struct v_link_ser_priv *priv)
{
	priv->mux = i2c_mux_alloc(priv->client->adapter, &priv->client->dev, 1,
				  0, 0, 
				  v_link_ser_i2c_mux_select, NULL);
	if (!priv->mux)
		return -ENOMEM;

	return i2c_mux_add_adapter(priv->mux, 0, 0, 0);
}

static const struct regmap_config v_link_ser_i2c_regmap = {
    .reg_bits = 16,
    .val_bits = 8,
    .max_register = 0x1f00,
};

static int v_link_ser_write(struct v_link_ser_priv *priv, unsigned int reg,
                            unsigned int val) {
  int ret;

  ret = regmap_write(priv->regmap, reg, val);
  if (ret)
    dev_err(&priv->client->dev, "write 0x%04x failed\n", reg);

  return ret;
}

static int v_link_ser_read(struct v_link_ser_priv *priv, unsigned int reg,
                           unsigned int *val) {
  int ret;

  ret = regmap_read(priv->regmap, reg, val);
  if (ret)
    dev_err(&priv->client->dev, "write 0x%04x failed\n", reg);

  return ret;
}

static int v_link_ser_update_bits(struct v_link_ser_priv *priv,
                                  unsigned int reg, unsigned int mask,
                                  unsigned int val) {
  int ret;

  ret = regmap_update_bits(priv->regmap, reg, mask, val);
  if (ret)
    dev_err(&priv->client->dev, "update 0x%04x failed\n", reg);

  return ret;
}

static int v_link_ser_gpiochip_get(struct gpio_chip *gpiochip,
                                   unsigned int offset) {
  struct v_link_ser_priv *priv = gpiochip_get_data(gpiochip);
  unsigned int val;
  int ret;

  ret = v_link_ser_read(priv, MAX96717_GPIO_REG_A(offset), &val);
  if (ret)
    return ret;

  if (val & MAX96717_GPIO_OUT_DIS)
    return !!(val & MAX96717_GPIO_IN);
  else
    return !!(val & MAX96717_GPIO_OUT);
}

static void v_link_ser_gpiochip_set(struct gpio_chip *gpiochip,
                                    unsigned int offset, int value) {
  struct v_link_ser_priv *priv = gpiochip_get_data(gpiochip);

  v_link_ser_update_bits(priv, MAX96717_GPIO_REG_A(offset),
                                MAX96717_GPIO_OUT_DIS | MAX96717_GPIO_OUT,
                                value ? MAX96717_GPIO_OUT : 0);                
}

static int v_link_ser_gpio_get_direction(struct gpio_chip *gpiochip,
                                         unsigned int offset) {
  struct v_link_ser_priv *priv = gpiochip_get_data(gpiochip);
  unsigned int val;
  int ret;

  ret = v_link_ser_read(priv, MAX96717_GPIO_REG_A(offset), &val);
  if (ret < 0)
    return ret;

  return !!(val & MAX96717_GPIO_OUT_DIS);
}

static int v_link_ser_gpio_direction_out(struct gpio_chip *gpiochip,
                                         unsigned int offset, int value) {
  struct v_link_ser_priv *priv = gpiochip_get_data(gpiochip);

  return v_link_ser_update_bits(priv, MAX96717_GPIO_REG_A(offset),
                                MAX96717_GPIO_OUT_DIS | MAX96717_GPIO_OUT,
                                value ? MAX96717_GPIO_OUT : 0);
}

static int v_link_ser_gpio_direction_in(struct gpio_chip *gpiochip,
                                        unsigned int offset) {
  struct v_link_ser_priv *priv = gpiochip_get_data(gpiochip);

  return v_link_ser_update_bits(priv, MAX96717_GPIO_REG_A(offset),
                                MAX96717_GPIO_OUT_DIS, MAX96717_GPIO_OUT_DIS);
}

static int v_link_ser_gpiochip_probe(struct v_link_ser_priv *priv) {
  struct device *dev = &priv->client->dev;
  struct gpio_chip *gc = &priv->gpio_chip;
  int i, ret = 0;

  gc->label = dev_name(dev);
  gc->parent = dev;
  gc->owner = THIS_MODULE;
  gc->ngpio = MAX96717_NUM_GPIO; /* We only use camera reset so 1 is enough */
  gc->base = -1;
  gc->can_sleep = true;
  gc->get_direction = v_link_ser_gpio_get_direction;
  gc->direction_input = v_link_ser_gpio_direction_in;
  gc->direction_output = v_link_ser_gpio_direction_out;
  gc->set = v_link_ser_gpiochip_set;
  gc->get = v_link_ser_gpiochip_get;
  gc->of_gpio_n_cells = 2;

  /* Disable GPIO forwarding */
  for (i = 0; i < gc->ngpio; i++)
    ret = v_link_ser_update_bits(priv, MAX96717_GPIO_REG_A(i),
                                 MAX96717_GPIO_RX_EN | MAX96717_GPIO_TX_EN, 0);

  if (ret)
    return ret;

  ret = devm_gpiochip_add_data(dev, gc, priv);
  if (ret) {
    dev_err(dev, "Unable to create gpio_chip");
    return ret;
  }

  dev_info(dev, "GPIOCHIP probed");
  return 0;
}

static int v_link_ser_setup(struct v_link_ser_priv *priv) {
  struct device *dev = &priv->client->dev;
  unsigned long lanes_used = 0;
  unsigned int nlanes, lane, val = 0;
  int ret = 0;

  nlanes = priv->bus_width;

  ret = v_link_ser_update_bits(priv, MAX96717_MIPI_RX1, MAX96717_MIPI_LANES_CNT,
                               FIELD_PREP(MAX96717_MIPI_LANES_CNT, nlanes - 1));
  if (ret) {
    dev_err(dev, "Unable to set MIPI_RX1 reg");
    return ret;
  }

  /* Fixed values for this register based on HW design */
  val = 0x07;
  ret = v_link_ser_update_bits(priv, MAX96717_MIPI_RX5, MAX96717_PHY2_LANES_POL,
                               FIELD_PREP(MAX96717_PHY2_LANES_POL, val));
  if (ret) {
    dev_err(dev, "Unable to set MIPI_RX5 reg");
    return ret;
  }

  /* Fixed values for this register based on HW design */
  val = 0x03;
  ret = v_link_ser_update_bits(priv, MAX96717_MIPI_RX4, MAX96717_PHY1_LANES_POL,
                               FIELD_PREP(MAX96717_PHY1_LANES_POL, val >> 3));
  if (ret) {
    dev_err(dev, "Unable to set MIPI_RX4 reg");
    return ret;
  }

  val = 0;
  /* Map used lanes */
  for (lane = 0, val = 0; lane < nlanes; lane++) {
    val |= lane << (lane * 2);
    lanes_used |= BIT(lane);
  }

  /*
   * Map unused lanes
   */
  for (; lane < MAX96717_CSI_NLANES; lane++) {
    unsigned int idx = find_first_zero_bit(&lanes_used, MAX96717_CSI_NLANES);

    val |= idx << (lane * 2);
    lanes_used |= BIT(idx);
  }

  ret = v_link_ser_update_bits(priv, MAX96717_MIPI_RX3, MAX96717_PHY1_LANES_MAP,
                               FIELD_PREP(MAX96717_PHY1_LANES_MAP, val));
  if (ret) {
    dev_err(dev, "Unable to set MIPI_RX3 reg");
    return ret;
  }

  ret = v_link_ser_update_bits(priv, MAX96717_MIPI_RX2, MAX96717_PHY2_LANES_MAP,
                               FIELD_PREP(MAX96717_PHY2_LANES_MAP, val >> 4));
  if (ret) {
    dev_err(dev, "Unable to set MIPI_RX2 reg");
    return ret;
  }

  return v_link_ser_gpiochip_probe(priv);
}

static int v_link_ser_enable_sources(struct v_link_ser_priv *priv) {
  struct device *dev = &priv->client->dev;
  int ret;

  ret = v_link_ser_update_bits(priv, MAX96717_FRONTOP0, MAX96717_START_PORT_B,
                               MAX96717_START_PORT_B);
  if (ret) {
    dev_err(dev, "Unable to start serializer CSI port.");
    return ret;
  }

  return ret;
}

static int v_link_ser_disable_sources(struct v_link_ser_priv *priv) {
  struct device *dev = &priv->client->dev;
  int ret;

  ret =
      v_link_ser_update_bits(priv, MAX96717_FRONTOP0, MAX96717_START_PORT_B, 0);
  if (ret) {
    dev_err(dev, "Unable to stop serializer CSI port.");
    return ret;
  }

  return ret;
}

static int v_link_ser_parse_dt(struct v_link_ser_priv *priv) {
  struct device *dev = &priv->client->dev;
  u32 bus_width;

  if (of_property_read_u32(dev->of_node, "bus-width", &bus_width)) {
    dev_err(dev, "Missing bus-width property");
    return -EINVAL;
  }

  if (bus_width < 1 || bus_width > MAX96717_CSI_NLANES) {
    dev_err(dev, "Invalid number of data lanes must be 1 to 4");
    return -EINVAL;
  }

  priv->bus_width = bus_width;
  dev_dbg(dev, "Got bus-width: %u", bus_width);

  return 0;
}

static int v_link_ser_s_stream(struct v4l2_subdev *sd, int enable) {
  struct v_link_ser_priv *priv = sd_to_v_link(sd);
  struct device *dev = &priv->client->dev;

  dev_dbg(dev, "s_stream");
  
  if(enable) {
    return v_link_ser_enable_sources(priv);
  } else {
    return v_link_ser_disable_sources(priv);
  };
  return 0;
}

static const struct v4l2_subdev_video_ops v_link_ser_video_ops = {
    .s_stream = v_link_ser_s_stream,
};

static const struct v4l2_subdev_ops v_link_ser_subdev_ops = {
    .video = &v_link_ser_video_ops,
};

static const struct media_entity_operations vlink_media_ops = {
    .link_validate = v4l2_subdev_link_validate,
};

static int v_link_ser_open(struct v4l2_subdev *sd,
                             struct v4l2_subdev_fh *fh) {
  struct i2c_client *client = v4l2_get_subdevdata(sd);

  dev_dbg(&client->dev, "%s:\n", __func__);
  return 0;
}

static const struct v4l2_subdev_internal_ops vlink_subdev_internal_ops = {
    .open = v_link_ser_open,
};

static int v_link_ser_v4l2_register(struct v_link_ser_priv *priv) {
  struct device *dev = &priv->client->dev;
  int ret;

  dev_info(dev, "vlink_v4l2_register");

  v4l2_i2c_subdev_init(&priv->sd, priv->client, &v_link_ser_subdev_ops);

  v4l2_ctrl_handler_init(&priv->ctrls, 1);
  priv->sd.ctrl_handler = &priv->ctrls;

  if (priv->ctrls.error) {
    ret = priv->ctrls.error;
    goto err_free_ctrl;
  }

  priv->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_HAS_EVENTS;
  priv->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
  priv->pads[1].flags = MEDIA_PAD_FL_SOURCE;
  priv->pads[0].flags = MEDIA_PAD_FL_SINK;
  priv->sd.entity.ops = &vlink_media_ops;

  ret =
      media_entity_pads_init(&priv->sd.entity, V_LINK_SER_N_PADS, priv->pads);

  if (ret)
    goto err_free_ctrl;

  priv->sd.dev = &priv->client->dev;
  priv->sd.internal_ops = &vlink_subdev_internal_ops;

  ret = v4l2_async_register_subdev(&priv->sd);
  if (ret < 0) {
    dev_err(dev, "Unable to register subdevice\n");
    goto err_subdev_cleanup;
  }

  return 0;

err_subdev_cleanup:
  v4l2_async_unregister_subdev(&priv->sd);
err_free_ctrl:
  v4l2_ctrl_handler_free(&priv->ctrls);

  return ret;
}

static int v_link_ser_probe(struct i2c_client *client) {
  struct device *dev = &client->dev;
  struct v_link_ser_priv *priv;
  int ret;

  priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
  if (!priv)
    return -ENOMEM;

  priv->client = client;

  priv->regmap = devm_regmap_init_i2c(client, &v_link_ser_i2c_regmap);
  if (IS_ERR(priv->regmap))
    return PTR_ERR(priv->regmap);

  ret = v_link_ser_parse_dt(priv);
  if (ret) {
    dev_err_probe(dev, ret, "Failed to parse dt");
    return ret;
  }

	ret = v_link_ser_i2c_mux_init(priv);
	if (ret) {
		dev_err_probe(dev, ret, "Unable to initialize I2C multiplexer");
		return ret;
	}

  ret = v_link_ser_setup(priv);
  if (ret) {
    dev_err_probe(dev, ret, "Unable to setup serializer");
    return ret;
  }

  ret = v_link_ser_v4l2_register(priv);
  if (ret) {
    dev_err_probe(dev, ret, "Unable to register V4L2 subdev");
    return ret;
  }

  dev_info(dev, "Probed succesfully");

  return ret;
}

static int v_link_ser_remove(struct i2c_client *client) {
  struct v_link_ser_priv *priv =
		sd_to_v_link(i2c_get_clientdata(client));
  
  v4l2_async_unregister_subdev(&priv->sd);
	media_entity_cleanup(&priv->sd.entity);
	v4l2_ctrl_handler_free(&priv->ctrls);
  i2c_mux_del_adapters(priv->mux);
  
  return 0;
}

static const struct of_device_id v_link_ser_dt_ids[] = {
    {.compatible = "videtronic,v-link-ser"},
    {},
};
MODULE_DEVICE_TABLE(of, v_link_ser_dt_ids);

static struct i2c_driver v_link_ser_i2c_driver = {
    .driver =
        {
            .name = "v-link-ser",
            .of_match_table = of_match_ptr(v_link_ser_dt_ids),
        },
    .probe_new = v_link_ser_probe,
    .remove = v_link_ser_remove,
};

module_i2c_driver(v_link_ser_i2c_driver);

MODULE_DESCRIPTION("Videtronic v-link serializer driver");
MODULE_AUTHOR("Jakub Kostiw");
MODULE_LICENSE("GPL");
