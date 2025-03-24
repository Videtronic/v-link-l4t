// SPDX-License-Identifier: GPL-2.0+
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fwnode.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/i2c-mux.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_graph.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-device.h>

/* PAD0 - image sink */
#define V_LINK_DESER_SINK_PAD0 0
#define V_LINK_DESER_N_SINK_PADS 1

/* PAD1 - image source */
#define V_LINK_DESER_SRC_PAD0 1
#define V_LINK_DESER_N_SRC_PADS 1

#define V_LINK_DESER_N_PADS (V_LINK_DESER_N_SRC_PADS + V_LINK_DESER_N_SINK_PADS)

#define MAX96714_MIPI_STDBY_N (0x0332)
#define MAX96714_MIPI_STDBY_MASK GENMASK(5, 4)
#define MAX96714_BACKTOP25 (0x0320)
#define CSI_DPLL_FREQ_MASK GENMASK(4, 0)
#define MAX96714_MIPI_LANE_CNT (0x044a)
#define MAX96714_CSI2_LANE_CNT_MASK GENMASK(7, 6)
#define MAX96714_MIPI_POLARITY (0x0335)
#define MAX96714_MIPI_POLARITY_MASK GENMASK(5, 0)
#define MAX96714_MIPI_LANE_MAP (0x0333)

#define MAX96714_CSI_NLANES 4

#define MHZ(v) ((u32)((v)*1000000U))

struct v_link_deser_priv {
  struct i2c_client *client;
  struct regmap *regmap;
  struct i2c_mux_core *mux;
  struct media_pad pads[V_LINK_DESER_N_PADS];
  struct v4l2_subdev sd;
  struct v4l2_ctrl_handler ctrls;
  u32 bus_width;
  u64 link_freq;
};

static inline struct v_link_deser_priv *sd_to_v_link(struct v4l2_subdev *sd) {
  return container_of(sd, struct v_link_deser_priv, sd);
}

static int v_link_deser_i2c_mux_select(struct i2c_mux_core *mux, u32 chan)
{
	return 0;
}

static int v_link_deser_i2c_mux_init(struct v_link_deser_priv *priv)
{
	priv->mux = i2c_mux_alloc(priv->client->adapter, &priv->client->dev, 1,
				  0, 0,
				  v_link_deser_i2c_mux_select, NULL);
	if (!priv->mux)
		return -ENOMEM;

	return i2c_mux_add_adapter(priv->mux, 0, 0, 0);
}

static const struct regmap_config v_link_deser_i2c_regmap = {
    .reg_bits = 16,
    .val_bits = 8,
    .max_register = 0x1f00,
};

static int v_link_deser_write(struct v_link_deser_priv *priv, unsigned int reg,
                              unsigned int val) {
  int ret;

  ret = regmap_write(priv->regmap, reg, val);
  if (ret)
    dev_err(&priv->client->dev, "write 0x%04x failed\n", reg);

  return ret;
}

static int v_link_deser_read(struct v_link_deser_priv *priv, unsigned int reg,
                             unsigned int *val) {
  int ret;

  ret = regmap_read(priv->regmap, reg, val);
  if (ret)
    dev_err(&priv->client->dev, "write 0x%04x failed\n", reg);

  return ret;
}

static int v_link_deser_update_bits(struct v_link_deser_priv *priv,
                                    unsigned int reg, unsigned int mask,
                                    unsigned int val) {
  int ret;

  ret = regmap_update_bits(priv->regmap, reg, mask, val);
  if (ret)
    dev_err(&priv->client->dev, "update 0x%04x failed\n", reg);

  return ret;
}

static int v_link_deser_setup(struct v_link_deser_priv *priv) {
  struct device *dev = &priv->client->dev;
  unsigned long lanes_used = 0;
  unsigned int nlanes, lane, val = 0;
  int ret = 0;

  val = 0xc4;
  ret = v_link_deser_write(priv, 0x0332, val);
  if (ret)
    return ret;

  val = div_u64(priv->link_freq * 2, MHZ(100));
  ret = v_link_deser_update_bits(priv, MAX96714_BACKTOP25, CSI_DPLL_FREQ_MASK,
                                 val);
  if (ret) {
    dev_err(dev, "Unable to set MAX96714_BACKTOP25 reg");
    return ret;
  }

  val = FIELD_PREP(MAX96714_CSI2_LANE_CNT_MASK, priv->bus_width - 1);
  ret = v_link_deser_update_bits(priv, MAX96714_MIPI_LANE_CNT,
                                 MAX96714_CSI2_LANE_CNT_MASK, val);
  if (ret) {
    dev_err(dev, "Unable to set MAX96714_MIPI_LANE_CNT reg");
    return ret;
  }

  /* Fixed values for this register based on HW design */
  val = 0x3b;
  ret = v_link_deser_write(priv, MAX96714_MIPI_POLARITY, val);
  if (ret) {
    dev_err(dev, "Unable to set MAX96714_MIPI_POLARITY reg");
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
  for (; lane < MAX96714_CSI_NLANES; lane++) {
    unsigned int idx = find_first_zero_bit(&lanes_used, MAX96714_CSI_NLANES);

    val |= idx << (lane * 2);
    lanes_used |= BIT(idx);
  }

  return v_link_deser_write(priv, MAX96714_MIPI_LANE_MAP, val);
}

static int v_link_deser_parse_dt(struct v_link_deser_priv *priv) {
  struct device *dev = &priv->client->dev;
  u32 bus_width;
  int ret;
  const char *link_freq;

  if (of_property_read_u32(dev->of_node, "bus-width", &bus_width)) {
    dev_err(dev, "Missing bus-width property");
    return -EINVAL;
  }

  if (of_property_read_string(dev->of_node, "link-freq", &link_freq)) {
    dev_err(dev, "Missing pix_clk_hz property");
    return -EINVAL;
  }

  priv->bus_width = bus_width;
  dev_dbg(dev, "Got bus-width: %u", bus_width);

  if (bus_width < 1 || bus_width > MAX96714_CSI_NLANES) {
    dev_err(dev, "Invalid number of data lanes must be 1 to 4");
    return -EINVAL;
  }

  ret = kstrtoull(link_freq, 10, &priv->link_freq);
  if (ret) {
    dev_err(dev, "Failed to convert link_freq");
    return -EFAULT;
  }

  dev_dbg(dev, "Got link_freq: %llu", priv->link_freq);

  /* Min 50MHz, Max 1250MHz, 50MHz step */
  if (priv->link_freq < MHZ(50) || priv->link_freq > MHZ(1250) ||
      priv->link_freq % MHZ(50)) {
    dev_err(dev, "Invalid link_freq frequency");
    return -EINVAL;
  }

  return ret;
}

static int v_link_deser_enable_sources(struct v_link_deser_priv *priv) {
  struct device *dev = &priv->client->dev;
  int ret;

  ret = v_link_deser_update_bits(priv, MAX96714_MIPI_STDBY_N,
                                 MAX96714_MIPI_STDBY_MASK,
                                 MAX96714_MIPI_STDBY_MASK);
  if (ret) {
    dev_err(dev, "Unable to start deserializer CSI port.");
    return ret;
  }

  return ret;
}

static int v_link_deser_disable_sources(struct v_link_deser_priv *priv) {
  struct device *dev = &priv->client->dev;
  int ret;

  ret = v_link_deser_update_bits(priv, MAX96714_MIPI_STDBY_N,
                                 MAX96714_MIPI_STDBY_MASK, 0);
  if (ret) {
    dev_err(dev, "Unable to stop deserializer CSI port.");
    return ret;
  }

  return ret;
}

static int v_link_deser_s_stream(struct v4l2_subdev *sd, int enable) {
  struct v_link_deser_priv *priv = sd_to_v_link(sd);
  struct device *dev = &priv->client->dev;

  dev_dbg(dev, "s_stream");
  
  if(enable) {
    return v_link_deser_enable_sources(priv);
  } else {
    return v_link_deser_disable_sources(priv);  
  };

  return 0;
}

static const struct v4l2_subdev_video_ops v_link_deser_video_ops = {
    .s_stream = v_link_deser_s_stream,
};

static const struct v4l2_subdev_ops v_link_deser_subdev_ops = {
    .video = &v_link_deser_video_ops,
};

static const struct media_entity_operations vlink_media_ops = {
    .link_validate = v4l2_subdev_link_validate,
};

static int v_link_deser_v4l2_register(struct v_link_deser_priv *priv) {
  struct device *dev = &priv->client->dev;
  int ret;

  dev_info(dev, "vlink_v4l2_register");

  v4l2_i2c_subdev_init(&priv->sd, priv->client, &v_link_deser_subdev_ops);

  v4l2_ctrl_handler_init(&priv->ctrls, 1);
  priv->sd.ctrl_handler = &priv->ctrls;

  v4l2_ctrl_new_int_menu(&priv->ctrls, NULL, V4L2_CID_LINK_FREQ, 0, 0,
                         &priv->link_freq);

  if (priv->ctrls.error) {
    ret = priv->ctrls.error;
    goto err_free_ctrl;
  }

  priv->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_HAS_EVENTS;
  priv->sd.entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
  priv->pads[0].flags = MEDIA_PAD_FL_SINK;
  priv->pads[1].flags = MEDIA_PAD_FL_SOURCE;
  priv->sd.entity.ops = &vlink_media_ops;

  ret =
      media_entity_pads_init(&priv->sd.entity, V_LINK_DESER_N_PADS, priv->pads);

  if (ret)
    goto err_free_ctrl;

  priv->sd.dev = &priv->client->dev;

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

static int v_link_deser_probe(struct i2c_client *client) {
  struct device *dev = &client->dev;
  struct v_link_deser_priv *priv;
  int ret;

  priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
  if (!priv)
    return -ENOMEM;

  priv->client = client;

  priv->regmap = devm_regmap_init_i2c(client, &v_link_deser_i2c_regmap);
  if (IS_ERR(priv->regmap))
    return PTR_ERR(priv->regmap);

  ret = v_link_deser_parse_dt(priv);
  if (ret) {
    dev_err_probe(dev, ret, "Failed to parse dt\n");
    return ret;
  }

  ret = v_link_deser_i2c_mux_init(priv);
	if (ret) {
		dev_err_probe(dev, ret, "Unable to initialize I2C multiplexer\n");
		return ret;
	}

  ret = v_link_deser_setup(priv);
  if (ret) {
    dev_err_probe(dev, ret, "Unable to setup serializer");
    return ret;
  }

  ret = v_link_deser_v4l2_register(priv);
  if (ret) {
    dev_err_probe(dev, ret, "Unable to register V4L2 subdev");
    return ret;
  }

  dev_info(dev, "Probed succesfully");

  return ret;
}

static int v_link_deser_remove(struct i2c_client *client) {
   struct v_link_deser_priv *priv =
		sd_to_v_link(i2c_get_clientdata(client));
  
  v4l2_async_unregister_subdev(&priv->sd);
	media_entity_cleanup(&priv->sd.entity);
	v4l2_ctrl_handler_free(&priv->ctrls);
  i2c_mux_del_adapters(priv->mux);

  return 0;
}

static const struct of_device_id v_link_deser_dt_ids[] = {
    {.compatible = "videtronic,v-link-deser"},
    {},
};
MODULE_DEVICE_TABLE(of, v_link_deser_dt_ids);

static struct i2c_driver v_link_deser_i2c_driver = {
    .driver =
        {
            .name = "v-link-deser",
            .of_match_table = of_match_ptr(v_link_deser_dt_ids),
        },
    .probe_new = v_link_deser_probe,
    .remove = v_link_deser_remove,
};

module_i2c_driver(v_link_deser_i2c_driver);

MODULE_DESCRIPTION("Videtronic v-link deserializer driver");
MODULE_AUTHOR("Jakub Kostiw");
MODULE_LICENSE("GPL");
