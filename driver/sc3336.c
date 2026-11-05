// SPDX-License-Identifier: GPL-2.0
/*
 * SmartSens SC3336 Camera Sensor Driver
 * Fully Featured: Multi-resolution, HDR 2-Frame, HFLIP/VFLIP,
 * Test Pattern, High-Precision Gain, Non-Continuous Clock.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/rk-camera-module.h>
#include <linux/rk-preisp.h>          /* PREISP_CMD_SET_HDRAE_EXP 等命令 */
#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>

#define SC3336_NAME                     "sc3336"

/*
 * pwdn 引脚"工作电平"。
 *
 * 设备树写的是 pwdn-gpios = <&gpio3 RK_PC5 GPIO_ACTIVE_HIGH>，
 * 字面理解"power down 有效=高" → 工作就该写 0。但【这块板实测相反】：
 *
 *     写 0 → 读 Chip ID 得到 0x0000，i2c 返回 -EIO(-5)，probe 直接失败
 *     写 1 → 读到 0xcc41，一切正常
 *
 * 原因大概是模组丝印 "SC3336 & OV2685" 是通用板（兼容 OV2685 接法），
 * 这脚实际是 STANDBY 而不是 PWRDN，或者板上串了个反相器。
 *
 * 所以做成参数，默认用实测能工作的 1：
 *     insmod sc3336.ko pwdn_work_level=1     # 默认，能工作
 *     insmod sc3336.ko pwdn_work_level=0     # 想验证极性时翻过来
 */
static int pwdn_work_level = 1;
module_param(pwdn_work_level, int, 0644);
MODULE_PARM_DESC(pwdn_work_level, "pwdn 引脚工作电平（本板实测需为 1）");
#define SC3336_LANES                    2
#define SC3336_BITS_PER_SAMPLE          10
#define SC3336_LINK_FREQ_255            255000000ULL
#define PIXEL_RATE_WITH_255M_10BIT      (SC3336_LINK_FREQ_255 * 2 * \
                                         SC3336_LANES / SC3336_BITS_PER_SAMPLE)
#define SC3336_XVCLK_FREQ               24000000

/* 寄存器定义 */
#define SC3336_REG_CHIP_ID              0x3107
#define SC3336_REG_CTRL_MODE            0x0100
#define SC3336_MODE_SW_STANDBY          0x00
#define SC3336_MODE_STREAMING           BIT(0)

/* 曝光相关 */
#define SC3336_REG_EXPOSURE_H           0x3e00
#define SC3336_REG_EXPOSURE_M           0x3e01
#define SC3336_REG_EXPOSURE_L           0x3e02
#define SC3336_REG_SHORT_EXP_H          0x3e04
#define SC3336_REG_SHORT_EXP_M          0x3e05
#define SC3336_REG_SHORT_EXP_L          0x3e06

#define SC3336_EXPOSURE_MIN             1
#define SC3336_EXPOSURE_STEP            1

/* 增益控制 */
#define SC3336_REG_DIG_GAIN             0x3e06
#define SC3336_REG_DIG_FINE_GAIN        0x3e07
#define SC3336_REG_ANA_GAIN             0x3e09
#define SC3336_REG_SHORT_DIG_GAIN       0x3e10
#define SC3336_REG_SHORT_DIG_FINE_GAIN  0x3e11
#define SC3336_REG_SHORT_ANA_GAIN       0x3e12

#define SC3336_GAIN_MIN                 0x0080
#define SC3336_GAIN_MAX                 (99614)
#define SC3336_GAIN_STEP                1
#define SC3336_GAIN_DEFAULT             0x80

/* 测试图与翻转 */
#define SC3336_REG_TEST_PATTERN         0x4501
#define SC3336_TEST_PATTERN_MASK        BIT(3)
#define SC3336_REG_VTS_H                0x320e
#define SC3336_REG_VTS_L                0x320f
#define SC3336_REG_FLIP_MIRROR          0x3221

#define SC3336_FETCH_EXP_H(VAL)         (((VAL) >> 12) & 0xF)
#define SC3336_FETCH_EXP_M(VAL)         (((VAL) >> 4) & 0xFF)
#define SC3336_FETCH_EXP_L(VAL)         (((VAL) & 0xF) << 4)

#define REG_NULL                        0xFFFF

static const char * const sc3336_supply_names[] = {
    "avdd", "dovdd", "dvdd",
};
#define SC3336_NUM_SUPPLIES ARRAY_SIZE(sc3336_supply_names)

struct regval {
    u16 addr;
    u8 val;
};

/* 模式配置结构体 */
struct sc3336_mode {
    u32 width;
    u32 height;
    struct v4l2_fract max_fps;
    u32 hts_def;
    u32 vts_def;
    u32 exp_def;
    u32 hdr_mode;
    const struct regval *reg_list;
};

/* 基础通用初始化配置表 */
static const struct regval sc3336_common_regs[] = {
    {0x0103, 0x01}, {0x36e9, 0x80}, {0x37f9, 0x80}, {0x301f, 0x02},
    {0x30b8, 0x33}, {0x3253, 0x10}, {0x325f, 0x20}, {0x3301, 0x04},
    {0x3306, 0x50}, {0x330a, 0x00}, {0x330b, 0xd8}, {0x3314, 0x13},
    {0x3333, 0x10}, {0x3334, 0x40}, {0x335e, 0x06}, {0x335f, 0x0a},
    {0x3364, 0x5e}, {0x337c, 0x02}, {0x337d, 0x0e}, {0x3390, 0x01},
    {0x3391, 0x03}, {0x3392, 0x07}, {0x3393, 0x04}, {0x3394, 0x04},
    {0x3395, 0x04}, {0x3396, 0x08}, {0x3397, 0x0b}, {0x3398, 0x1f},
    {0x3399, 0x04}, {0x339a, 0x0a}, {0x339b, 0x3a}, {0x339c, 0xc4},
    {0x33a2, 0x04}, {0x33ac, 0x08}, {0x33ad, 0x1c}, {0x33ae, 0x10},
    {0x33af, 0x30}, {0x33b1, 0x80}, {0x33b3, 0x48}, {0x33f9, 0x60},
    {0x33fb, 0x74}, {0x33fc, 0x4b}, {0x33fd, 0x5f}, {0x349f, 0x03},
    {0x34a6, 0x4b}, {0x34a7, 0x5f}, {0x34a8, 0x20}, {0x34a9, 0x18},
    {0x34ab, 0xe8}, {0x34ac, 0x01}, {0x34ad, 0x00}, {0x34f8, 0x5f},
    {0x34f9, 0x18}, {0x3630, 0xc0}, {0x3631, 0x84}, {0x3632, 0x64},
    {0x3633, 0x32}, {0x363b, 0x03}, {0x363c, 0x08}, {0x3641, 0x38},
    {0x3670, 0x4e}, {0x3674, 0xc0}, {0x3675, 0xc0}, {0x3676, 0xc0},
    {0x3677, 0x84}, {0x3678, 0x8a}, {0x3679, 0x8c}, {0x367c, 0x48},
    {0x367d, 0x49}, {0x367e, 0x4b}, {0x367f, 0x5f}, {0x3690, 0x33},
    {0x3691, 0x33}, {0x3692, 0x44}, {0x369c, 0x4b}, {0x369d, 0x5f},
    {0x36b0, 0x87}, {0x36b1, 0x90}, {0x36b2, 0xa1}, {0x36b3, 0xd8},
    {0x36b4, 0x49}, {0x36b5, 0x4b}, {0x36b6, 0x4f}, {0x36ea, 0x11},
    {0x36eb, 0x0d}, {0x36ec, 0x1c}, {0x36ed, 0x26}, {0x370f, 0x01},
    {0x3722, 0x09}, {0x3724, 0x41}, {0x3725, 0xc1}, {0x3771, 0x09},
    {0x3772, 0x09}, {0x3773, 0x05}, {0x377a, 0x48}, {0x377b, 0x5f},
    {0x37fa, 0x11}, {0x37fb, 0x33}, {0x37fc, 0x11}, {0x37fd, 0x08},
    {0x3904, 0x04}, {0x3905, 0x8c}, {0x391d, 0x04}, {0x3921, 0x20},
    {0x3926, 0x21}, {0x3933, 0x80}, {0x3934, 0x0a}, {0x3935, 0x00},
    {0x3936, 0x2a}, {0x3937, 0x6a}, {0x3938, 0x6a}, {0x39dc, 0x02},
    {0x440e, 0x02}, {0x4509, 0x20}, {0x5ae0, 0xfe}, {0x5ae1, 0x40},
    {0x5ae2, 0x38}, {0x5ae3, 0x30}, {0x5ae4, 0x28}, {0x5ae5, 0x38},
    {0x5ae6, 0x30}, {0x5ae7, 0x28}, {0x5ae8, 0x3f}, {0x5ae9, 0x34},
    {0x5aea, 0x2c}, {0x5aeb, 0x3f}, {0x5aec, 0x34}, {0x5aed, 0x2c},
    {0x36e9, 0x54}, {0x37f9, 0x47}, {REG_NULL, 0x00},
};

/* 2304x1296 线性模式特定参数 */
static const struct regval sc3336_linear_2304x1296_regs[] = {
    {0x320e, 0x05}, {0x320f, 0x50}, /* VTS = 1360 */
    {REG_NULL, 0x00},
};

/* 1920x1080 裁剪模式特定参数 */
static const struct regval sc3336_linear_1920x1080_regs[] = {
    {0x320e, 0x04}, {0x320f, 0x65}, /* VTS = 1125 */
    {0x3211, 0x60}, {0x3213, 0x6c}, /* 硬件窗口开窗偏移 */
    {REG_NULL, 0x00},
};

/* 双曝光 HDR 模式特定参数 */
static const struct regval sc3336_hdr_x2_regs[] = {
    {0x320e, 0x0a}, {0x320f, 0xa0}, /* 双帧曝光需要双倍行周期 */
    {0x3220, 0x55},                 /* 开启行交织 HDR 模式使能 */
    {REG_NULL, 0x00},
};

static const struct sc3336_mode supported_modes[] = {
    {
        .width = 2304,
        .height = 1296,
        .max_fps = { .numerator = 10000, .denominator = 300000 },
        .hts_def = 2800,
        .vts_def = 1360,
        .exp_def = 0x0080,
        .hdr_mode = NO_HDR,
        .reg_list = sc3336_linear_2304x1296_regs,
    },
    {
        .width = 1920,
        .height = 1080,
        .max_fps = { .numerator = 10000, .denominator = 300000 },
        .hts_def = 2800,
        .vts_def = 1125,
        .exp_def = 0x0080,
        .hdr_mode = NO_HDR,
        .reg_list = sc3336_linear_1920x1080_regs,
    },
    {
        .width = 2304,
        .height = 1296,
        .max_fps = { .numerator = 10000, .denominator = 300000 },
        .hts_def = 2800,
        .vts_def = 2720,
        .exp_def = 0x0080,
        .hdr_mode = HDR_X2,
        .reg_list = sc3336_hdr_x2_regs,
    },
};

static const s64 link_freq_menu_items[] = {
    SC3336_LINK_FREQ_255,
};

static const char * const sc3336_test_pattern_menu[] = {
    "Disabled",
    "Vertical Color Bars",
};

struct sc3336 {
    struct i2c_client       *client;
    struct clk              *xvclk;
    struct gpio_desc        *reset_gpio;
    struct gpio_desc        *pwdn_gpio;
    struct regulator_bulk_data supplies[SC3336_NUM_SUPPLIES];
    struct v4l2_subdev      subdev;
    struct media_pad        pad;
    struct v4l2_ctrl_handler ctrl_handler;
    struct v4l2_ctrl        *exposure;
    struct v4l2_ctrl        *anal_gain;
    struct v4l2_ctrl        *hblank;
    struct v4l2_ctrl        *vblank;
    struct v4l2_ctrl        *pixel_rate;
    struct v4l2_ctrl        *link_freq;
    struct v4l2_ctrl        *hflip;
    struct v4l2_ctrl        *vflip;
    struct v4l2_ctrl        *test_pattern;
    struct mutex            mutex;
    bool                    streaming;
    bool                    power_on;      /* 电源逻辑状态（s_power 幂等判断用） */
    u32                     module_index;  /* 设备树 camera-module-index，参与拼实体名 */
    const struct sc3336_mode *cur_mode;
    u32                     cur_vts;
    u32                     cur_hdr_mode;
    const char              *module_facing;
    const char              *module_name;
    const char              *len_name;
};

#define to_sc3336(sd) container_of(sd, struct sc3336, subdev)

static int sc3336_write_reg(struct i2c_client *client, u16 reg, u32 val)
{
    u8 buf[3] = { reg >> 8, reg & 0xff, val & 0xff };
    return (i2c_master_send(client, buf, 3) == 3) ? 0 : -EIO;
}

static int sc3336_write_array(struct i2c_client *client, const struct regval *regs)
{
    u32 i;
    int ret = 0;
    for (i = 0; ret == 0 && regs[i].addr != REG_NULL; i++)
        ret = sc3336_write_reg(client, regs[i].addr, regs[i].val);
    return ret;
}

static int sc3336_read_reg(struct i2c_client *client, u16 reg, u32 *val)
{
    struct i2c_msg msgs[2];
    u8 addr_buf[2] = { reg >> 8, reg & 0xff };
    u8 data_buf[2] = { 0 };
    int ret;

    msgs[0].addr = client->addr;
    msgs[0].flags = 0;
    msgs[0].len = 2;
    msgs[0].buf = addr_buf;

    msgs[1].addr = client->addr;
    msgs[1].flags = I2C_M_RD;
    msgs[1].len = 2;
    msgs[1].buf = data_buf;

    ret = i2c_transfer(client->adapter, msgs, 2);
    if (ret != 2)
        return -EIO;

    *val = (data_buf[0] << 8) | data_buf[1];
    return 0;
}

/* ========================== Q10 (1024定点) 高精度增益映射 ========================== */
static int sc3336_set_gain_reg(struct sc3336 *sc3336, u32 gain)
{
    u32 coarse_again = 0, coarse_dgain = 0, fine_dgain = 0;
    int ret = 0;
    /* Q10 换算：gain_factor = (gain * 1024) / 128 = gain * 8 */
    u32 factor_q10;

    gain = clamp_val(gain, SC3336_GAIN_MIN, SC3336_GAIN_MAX);
    factor_q10 = gain << 3;

    if (factor_q10 < 1556) {            /* < 1.52x */
        coarse_again = 0x00; coarse_dgain = 0x00;
        fine_dgain = (factor_q10 * 128) >> 10;
    } else if (factor_q10 < 3113) {     /* < 3.04x */
        coarse_again = 0x40; coarse_dgain = 0x00;
        fine_dgain = (factor_q10 * 128) / 1556;
    } else if (factor_q10 < 6226) {     /* < 6.08x */
        coarse_again = 0x48; coarse_dgain = 0x00;
        fine_dgain = (factor_q10 * 128) / 3113;
    } else if (factor_q10 < 12452) {    /* < 12.16x */
        coarse_again = 0x49; coarse_dgain = 0x00;
        fine_dgain = (factor_q10 * 128) / 6226;
    } else if (factor_q10 < 24904) {    /* < 24.32x */
        coarse_again = 0x4b; coarse_dgain = 0x00;
        fine_dgain = (factor_q10 * 128) / 12452;
    } else if (factor_q10 < 49807) {    /* < 48.64x */
        coarse_again = 0x4f; coarse_dgain = 0x00;
        fine_dgain = (factor_q10 * 128) / 24904;
    } else if (factor_q10 < (49807 << 1)) {
        coarse_again = 0x5f; coarse_dgain = 0x00;
        fine_dgain = (factor_q10 * 128) / 49807;
    } else {
        coarse_again = 0x5f; coarse_dgain = 0x01;
        fine_dgain = ((factor_q10 * 128) / 49807) >> 1;
    }

    ret = sc3336_write_reg(sc3336->client, SC3336_REG_DIG_GAIN, coarse_dgain);
    ret |= sc3336_write_reg(sc3336->client, SC3336_REG_DIG_FINE_GAIN, fine_dgain);
    ret |= sc3336_write_reg(sc3336->client, SC3336_REG_ANA_GAIN, coarse_again);

    if (sc3336->cur_hdr_mode == HDR_X2) {
        ret |= sc3336_write_reg(sc3336->client, SC3336_REG_SHORT_DIG_GAIN, coarse_dgain);
        ret |= sc3336_write_reg(sc3336->client, SC3336_REG_SHORT_DIG_FINE_GAIN, fine_dgain);
        ret |= sc3336_write_reg(sc3336->client, SC3336_REG_SHORT_ANA_GAIN, coarse_again);
    }

    return ret;
}

/* 翻转动态映射 Bayer 格式 */
static u32 sc3336_get_format_code(struct sc3336 *sc3336)
{
    bool hflip = sc3336->hflip ? sc3336->hflip->val : false;
    bool vflip = sc3336->vflip ? sc3336->vflip->val : false;

    if (!hflip && !vflip)
        return MEDIA_BUS_FMT_SBGGR10_1X10;
    else if (hflip && !vflip)
        return MEDIA_BUS_FMT_SGBRG10_1X10;
    else if (!hflip && vflip)
        return MEDIA_BUS_FMT_SGRBG10_1X10;
    else
        return MEDIA_BUS_FMT_SRGGB10_1X10;
}

/* ========================== 格式匹配与协商 ========================== */
static const struct sc3336_mode *
sc3336_find_best_fit(struct sc3336 *sc3336, struct v4l2_subdev_format *fmt)
{
    int i;
    u32 dist, best_dist = ~0;
    const struct sc3336_mode *best_mode = &supported_modes[0];

    for (i = 0; i < ARRAY_SIZE(supported_modes); i++) {
        if (supported_modes[i].hdr_mode != sc3336->cur_hdr_mode)
            continue;

        dist = abs(supported_modes[i].width - fmt->format.width) +
               abs(supported_modes[i].height - fmt->format.height);
        if (dist < best_dist) {
            best_dist = dist;
            best_mode = &supported_modes[i];
        }
    }
    return best_mode;
}

static int sc3336_set_fmt(struct v4l2_subdev *sd,
                          struct v4l2_subdev_pad_config *cfg,
                          struct v4l2_subdev_format *fmt)
{
    struct sc3336 *sc3336 = to_sc3336(sd);
    const struct sc3336_mode *mode;

    mutex_lock(&sc3336->mutex);
    mode = sc3336_find_best_fit(sc3336, fmt);
    fmt->format.code = sc3336_get_format_code(sc3336);
    fmt->format.width = mode->width;
    fmt->format.height = mode->height;
    fmt->format.field = V4L2_FIELD_NONE;

    if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
        sc3336->cur_mode = mode;
        sc3336->cur_vts = mode->vts_def;
        if (sc3336->vblank)
            __v4l2_ctrl_modify_range(sc3336->vblank,
                                     mode->vts_def - mode->height,
                                     0x7fff - mode->height, 1,
                                     mode->vts_def - mode->height);
    }
    mutex_unlock(&sc3336->mutex);
    return 0;
}

static int sc3336_get_fmt(struct v4l2_subdev *sd,
                          struct v4l2_subdev_pad_config *cfg,
                          struct v4l2_subdev_format *fmt)
{
    struct sc3336 *sc3336 = to_sc3336(sd);

    mutex_lock(&sc3336->mutex);
    fmt->format.code = sc3336_get_format_code(sc3336);
    fmt->format.width = sc3336->cur_mode->width;
    fmt->format.height = sc3336->cur_mode->height;
    fmt->format.field = V4L2_FIELD_NONE;
    mutex_unlock(&sc3336->mutex);
    return 0;
}

static int sc3336_enum_mbus_code(struct v4l2_subdev *sd,
                                 struct v4l2_subdev_pad_config *cfg,
                                 struct v4l2_subdev_mbus_code_enum *code)
{
    struct sc3336 *sc3336 = to_sc3336(sd);

    if (code->index != 0)
        return -EINVAL;
    code->code = sc3336_get_format_code(sc3336);
    return 0;
}

static int sc3336_enum_frame_sizes(struct v4l2_subdev *sd,
                                   struct v4l2_subdev_pad_config *cfg,
                                   struct v4l2_subdev_frame_size_enum *fse)
{
    struct sc3336 *sc3336 = to_sc3336(sd);

    if (fse->index >= ARRAY_SIZE(supported_modes))
        return -EINVAL;

    if (fse->code != sc3336_get_format_code(sc3336))
        return -EINVAL;

    fse->min_width = supported_modes[fse->index].width;
    fse->max_width = supported_modes[fse->index].width;
    fse->min_height = supported_modes[fse->index].height;
    fse->max_height = supported_modes[fse->index].height;
    return 0;
}

static int sc3336_g_frame_interval(struct v4l2_subdev *sd,
                                   struct v4l2_subdev_frame_interval *fi)
{
    struct sc3336 *sc3336 = to_sc3336(sd);
    fi->interval = sc3336->cur_mode->max_fps;
    return 0;
}

static int sc3336_g_mbus_config(struct v4l2_subdev *sd,
                                unsigned int pad_id,
                                struct v4l2_mbus_config *config)
{
    config->type = V4L2_MBUS_CSI2_DPHY;
    /* 移除强制 CONTINUOUS_CLOCK，支持动态低功耗时钟待机 */
    config->flags = (1 << (SC3336_LANES - 1)) |
                    V4L2_MBUS_CSI2_CHANNEL_0;
    return 0;
}

/* ========================== RK IOCTL 与 HDR 动态协商 ========================== */
static void sc3336_get_module_inf(struct sc3336 *sc3336, struct rkmodule_inf *inf)
{
    memset(inf, 0, sizeof(*inf));
    strscpy(inf->base.sensor, SC3336_NAME, sizeof(inf->base.sensor));
    strscpy(inf->base.module, sc3336->module_name ? sc3336->module_name : "default", sizeof(inf->base.module));
    strscpy(inf->base.lens, sc3336->len_name ? sc3336->len_name : "default", sizeof(inf->base.lens));
}

static long sc3336_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
    struct sc3336 *sc3336 = to_sc3336(sd);
    struct rkmodule_hdr_cfg *hdr;
    u32 i, stream;

    switch (cmd) {
    case RKMODULE_GET_MODULE_INFO:
        sc3336_get_module_inf(sc3336, (struct rkmodule_inf *)arg);
        return 0;
    case RKMODULE_GET_HDR_CFG:
        hdr = (struct rkmodule_hdr_cfg *)arg;
        hdr->esp.mode = HDR_NORMAL_VC;
        hdr->hdr_mode = sc3336->cur_hdr_mode;
        return 0;
    case RKMODULE_SET_HDR_CFG:
        hdr = (struct rkmodule_hdr_cfg *)arg;
        if (hdr->hdr_mode != NO_HDR && hdr->hdr_mode != HDR_X2)
            return -EINVAL;

        mutex_lock(&sc3336->mutex);
        sc3336->cur_hdr_mode = hdr->hdr_mode;
        /* 切换对应 HDR 模式 */
        for (i = 0; i < ARRAY_SIZE(supported_modes); i++) {
            if (supported_modes[i].hdr_mode == sc3336->cur_hdr_mode) {
                sc3336->cur_mode = &supported_modes[i];
                sc3336->cur_vts = sc3336->cur_mode->vts_def;
                break;
            }
        }
        mutex_unlock(&sc3336->mutex);
        return 0;
    case RKMODULE_SET_QUICK_STREAM:
        stream = *((u32 *)arg);
        return sc3336_write_reg(sc3336->client, SC3336_REG_CTRL_MODE,
                                stream ? SC3336_MODE_STREAMING : SC3336_MODE_SW_STANDBY);
    /*
     * RKMODULE_GET_CHANNEL_INFO —— 告诉 rkaiq "这路 MIPI 上传的是什么"。
     *
     * ★ 少了它 rkaiq 的 init 直接失败，日志里【只有一句】
     *   "MessageParser process loop exit!"，不给任何原因。
     *   它要四样东西：宽高、Bayer 排列、虚拟通道、CSI-2 data type。
     *   data_type=0x2a 是 RAW10 的固定编码。
     */
    case RKMODULE_GET_CHANNEL_INFO: {
        struct rkmodule_channel_info *ch = arg;

        if (!ch)
            return -EINVAL;
        ch->width = sc3336->cur_mode->width;
        ch->height = sc3336->cur_mode->height;
        ch->bus_fmt = MEDIA_BUS_FMT_SBGGR10_1X10;
        ch->vc = V4L2_MBUS_CSI2_CHANNEL_0;
        ch->data_type = 0x2a;   /* CSI-2 RAW10 */
        return 0;
    }
    /*
     * PREISP_CMD_SET_HDRAE_EXP —— 只有 HDR 才用（设长短曝）。
     * 线性模式永远用不到，但【不能返回 -ENOIOCTLCMD】：
     * 返回"不支持"会让调用方以为出错走异常分支，空操作才对。
     */
    case PREISP_CMD_SET_HDRAE_EXP:
        return 0;

    default:
        return -ENOIOCTLCMD;
    }
}

/* ========================== 上电/断电回调 (s_power) ========================== */
/*
 * rkaiq 在 prepare 阶段会调 s_power(on) 先上电灌表，
 * 这样它去读 statistics 才有数据，之后才走正式 s_stream。
 *
 * ★ 必须幂等：rkaiq 和 rkcif 可能各调一次，不判状态会重复上电，
 *   导致 pm_runtime 引用计数失衡、断电函数提前执行。
 */
static int sc3336_s_power(struct v4l2_subdev *sd, int on)
{
    struct sc3336 *sc3336 = to_sc3336(sd);
    struct i2c_client *client = sc3336->client;
    int ret = 0;

    mutex_lock(&sc3336->mutex);

    if (sc3336->power_on == !!on)
        goto unlock;

    if (on) {
        ret = pm_runtime_get_sync(&client->dev);
        if (ret < 0) {
            pm_runtime_put_noidle(&client->dev);
            goto unlock;
        }
        sc3336->power_on = true;
    } else {
        pm_runtime_put(&client->dev);
        sc3336->power_on = false;
    }

unlock:
    mutex_unlock(&sc3336->mutex);
    return ret;
}

/* ========================== 视频流开关 ========================== */
static int sc3336_s_stream(struct v4l2_subdev *sd, int on)
{
    struct sc3336 *sc3336 = to_sc3336(sd);
    struct i2c_client *client = sc3336->client;
    int ret = 0;

    mutex_lock(&sc3336->mutex);
    on = !!on;
    if (on == sc3336->streaming)
        goto unlock;

    if (on) {
        ret = pm_runtime_get_sync(&client->dev);
        if (ret < 0) {
            pm_runtime_put_noidle(&client->dev);
            goto unlock;
        }

        ret = sc3336_write_array(client, sc3336_common_regs);
        if (ret)
            goto err_stream;

        ret = sc3336_write_array(client, sc3336->cur_mode->reg_list);
        if (ret)
            goto err_stream;

        ret = __v4l2_ctrl_handler_setup(&sc3336->ctrl_handler);
        if (ret)
            goto err_stream;

        ret = sc3336_write_reg(client, SC3336_REG_CTRL_MODE, SC3336_MODE_STREAMING);
        if (ret)
            goto err_stream;
    } else {
        sc3336_write_reg(client, SC3336_REG_CTRL_MODE, SC3336_MODE_SW_STANDBY);
        pm_runtime_put(&client->dev);
    }

    sc3336->streaming = on;
unlock:
    mutex_unlock(&sc3336->mutex);
    return ret;

err_stream:
    v4l2_err(sd, "s_stream failed\n");
    pm_runtime_put(&client->dev);
    mutex_unlock(&sc3336->mutex);
    return ret;
}

/* ========================== Controls 控制回调 ========================== */
static int sc3336_set_ctrl(struct v4l2_ctrl *ctrl)
{
    struct sc3336 *sc3336 = container_of(ctrl->handler, struct sc3336, ctrl_handler);
    struct i2c_client *client = sc3336->client;
    u8 flip_reg = 0;
    int ret = 0;

    if (!pm_runtime_get_if_in_use(&client->dev))
        return 0;

    switch (ctrl->id) {
    case V4L2_CID_EXPOSURE:
        ret = sc3336_write_reg(client, SC3336_REG_EXPOSURE_H, SC3336_FETCH_EXP_H(ctrl->val));
        ret |= sc3336_write_reg(client, SC3336_REG_EXPOSURE_M, SC3336_FETCH_EXP_M(ctrl->val));
        ret |= sc3336_write_reg(client, SC3336_REG_EXPOSURE_L, SC3336_FETCH_EXP_L(ctrl->val));
        if (sc3336->cur_hdr_mode == HDR_X2) {
            u32 short_exp = ctrl->val >> 2; /* 短曝光默认按 1:4 比例下发 */
            ret |= sc3336_write_reg(client, SC3336_REG_SHORT_EXP_H, SC3336_FETCH_EXP_H(short_exp));
            ret |= sc3336_write_reg(client, SC3336_REG_SHORT_EXP_M, SC3336_FETCH_EXP_M(short_exp));
            ret |= sc3336_write_reg(client, SC3336_REG_SHORT_EXP_L, SC3336_FETCH_EXP_L(short_exp));
        }
        break;
    case V4L2_CID_ANALOGUE_GAIN:
        ret = sc3336_set_gain_reg(sc3336, ctrl->val);
        break;
    case V4L2_CID_VBLANK:
        sc3336->cur_vts = ctrl->val + sc3336->cur_mode->height;
        ret = sc3336_write_reg(client, SC3336_REG_VTS_H, sc3336->cur_vts >> 8);
        ret |= sc3336_write_reg(client, SC3336_REG_VTS_L, sc3336->cur_vts & 0xff);
        if (sc3336->exposure)
            __v4l2_ctrl_modify_range(sc3336->exposure,
                                     SC3336_EXPOSURE_MIN,
                                     sc3336->cur_vts - 8,
                                     SC3336_EXPOSURE_STEP,
                                     sc3336->cur_vts - 8);
        break;
    case V4L2_CID_HFLIP:
    case V4L2_CID_VFLIP:
        if (sc3336->hflip->val)
            flip_reg |= 0x06;
        if (sc3336->vflip->val)
            flip_reg |= 0x60;
        ret = sc3336_write_reg(client, SC3336_REG_FLIP_MIRROR, flip_reg);
        break;
    case V4L2_CID_TEST_PATTERN:
        ret = sc3336_write_reg(client, SC3336_REG_TEST_PATTERN,
                               ctrl->val ? SC3336_TEST_PATTERN_MASK : 0x00);
        break;
    default:
        break;
    }

    pm_runtime_put(&client->dev);
    return ret;
}

static const struct v4l2_ctrl_ops sc3336_ctrl_ops = {
    .s_ctrl = sc3336_set_ctrl,
};

static int sc3336_init_controls(struct sc3336 *sc3336)
{
    struct v4l2_ctrl_handler *hdl = &sc3336->ctrl_handler;
    int ret;

    ret = v4l2_ctrl_handler_init(hdl, 9);
    if (ret)
        return ret;
    hdl->lock = &sc3336->mutex;

    sc3336->link_freq = v4l2_ctrl_new_int_menu(hdl, NULL, V4L2_CID_LINK_FREQ, 0, 0, link_freq_menu_items);
    if (sc3336->link_freq)
        sc3336->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

    sc3336->pixel_rate = v4l2_ctrl_new_std(hdl, NULL, V4L2_CID_PIXEL_RATE, 0,
                                           PIXEL_RATE_WITH_255M_10BIT, 1, PIXEL_RATE_WITH_255M_10BIT);

    sc3336->hblank = v4l2_ctrl_new_std(hdl, NULL, V4L2_CID_HBLANK,
                                       sc3336->cur_mode->hts_def - sc3336->cur_mode->width,
                                       sc3336->cur_mode->hts_def - sc3336->cur_mode->width, 1,
                                       sc3336->cur_mode->hts_def - sc3336->cur_mode->width);
    if (sc3336->hblank)
        sc3336->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

    sc3336->vblank = v4l2_ctrl_new_std(hdl, &sc3336_ctrl_ops, V4L2_CID_VBLANK,
                                       sc3336->cur_mode->vts_def - sc3336->cur_mode->height,
                                       0x7fff - sc3336->cur_mode->height, 1,
                                       sc3336->cur_mode->vts_def - sc3336->cur_mode->height);

    sc3336->exposure = v4l2_ctrl_new_std(hdl, &sc3336_ctrl_ops, V4L2_CID_EXPOSURE,
                                         SC3336_EXPOSURE_MIN,
                                         sc3336->cur_mode->vts_def - 8,
                                         SC3336_EXPOSURE_STEP,
                                         sc3336->cur_mode->exp_def);

    sc3336->anal_gain = v4l2_ctrl_new_std(hdl, &sc3336_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
                                          SC3336_GAIN_MIN, SC3336_GAIN_MAX,
                                          SC3336_GAIN_STEP, SC3336_GAIN_DEFAULT);

    sc3336->hflip = v4l2_ctrl_new_std(hdl, &sc3336_ctrl_ops, V4L2_CID_HFLIP, 0, 1, 1, 0);
    sc3336->vflip = v4l2_ctrl_new_std(hdl, &sc3336_ctrl_ops, V4L2_CID_VFLIP, 0, 1, 1, 0);

    sc3336->test_pattern = v4l2_ctrl_new_std_menu_items(hdl, &sc3336_ctrl_ops,
                                                        V4L2_CID_TEST_PATTERN,
                                                        ARRAY_SIZE(sc3336_test_pattern_menu) - 1,
                                                        0, 0, sc3336_test_pattern_menu);

    if (hdl->error) {
        ret = hdl->error;
        v4l2_ctrl_handler_free(hdl);
        return ret;
    }

    sc3336->subdev.ctrl_handler = hdl;
    return 0;
}

/* ========================== 电源管理 ========================== */
static int sc3336_power_on(struct sc3336 *sc3336)
{
    struct device *dev = &sc3336->client->dev;
    int ret;

    ret = clk_set_rate(sc3336->xvclk, SC3336_XVCLK_FREQ);
    ret |= clk_prepare_enable(sc3336->xvclk);
    if (ret) {
        dev_err(dev, "Failed to enable xvclk\n");
        return ret;
    }

    ret = regulator_bulk_enable(SC3336_NUM_SUPPLIES, sc3336->supplies);
    if (ret)
        dev_warn(dev, "Failed to enable regulators, assuming always on\n");

    /* pwdn 工作电平：本板实测要 1（不是字面上的 0），见 module_param 说明 */
    if (!IS_ERR_OR_NULL(sc3336->pwdn_gpio))
        gpiod_set_value_cansleep(sc3336->pwdn_gpio, pwdn_work_level);

    if (!IS_ERR_OR_NULL(sc3336->reset_gpio))
        gpiod_set_value_cansleep(sc3336->reset_gpio, 0);

    usleep_range(10000, 15000);
    return 0;
}

static void sc3336_power_off(struct sc3336 *sc3336)
{
    /* 拉回"非工作"电平（与 power_on 相反） */
    if (!IS_ERR_OR_NULL(sc3336->pwdn_gpio))
        gpiod_set_value_cansleep(sc3336->pwdn_gpio, !pwdn_work_level);

    if (!IS_ERR_OR_NULL(sc3336->reset_gpio))
        gpiod_set_value_cansleep(sc3336->reset_gpio, 1);

    clk_disable_unprepare(sc3336->xvclk);
    regulator_bulk_disable(SC3336_NUM_SUPPLIES, sc3336->supplies);
}

static int sc3336_runtime_resume(struct device *dev)
{
    struct i2c_client *client = to_i2c_client(dev);
    struct v4l2_subdev *sd = i2c_get_clientdata(client);
    return sc3336_power_on(to_sc3336(sd));
}

static int sc3336_runtime_suspend(struct device *dev)
{
    struct i2c_client *client = to_i2c_client(dev);
    struct v4l2_subdev *sd = i2c_get_clientdata(client);
    sc3336_power_off(to_sc3336(sd));
    return 0;
}

static const struct dev_pm_ops sc3336_pm_ops = {
    SET_RUNTIME_PM_OPS(sc3336_runtime_suspend, sc3336_runtime_resume, NULL)
};

/* ========================== 操作集注册 ========================== */
static const struct v4l2_subdev_core_ops sc3336_core_ops = {
    .ioctl = sc3336_ioctl,
    .s_power = sc3336_s_power,   /* rkaiq prepare 阶段会调 */
};

static const struct v4l2_subdev_video_ops sc3336_video_ops = {
    .s_stream = sc3336_s_stream,
    .g_frame_interval = sc3336_g_frame_interval,
};

static const struct v4l2_subdev_pad_ops sc3336_pad_ops = {
    .enum_mbus_code = sc3336_enum_mbus_code,
    .enum_frame_size = sc3336_enum_frame_sizes,
    .get_fmt = sc3336_get_fmt,
    .set_fmt = sc3336_set_fmt,
    .get_mbus_config = sc3336_g_mbus_config,
};

static const struct v4l2_subdev_ops sc3336_subdev_ops = {
    .core = &sc3336_core_ops,
    .video = &sc3336_video_ops,
    .pad = &sc3336_pad_ops,
};

/* ========================== Probe & Remove ========================== */
static int sc3336_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    struct device *dev = &client->dev;
    struct device_node *node = dev->of_node;
    struct sc3336 *sc3336;
    struct v4l2_subdev *sd;
    u32 chip_id = 0;
    int ret, i;

    sc3336 = devm_kzalloc(dev, sizeof(*sc3336), GFP_KERNEL);
    if (!sc3336)
        return -ENOMEM;

    sc3336->client = client;
    sc3336->cur_mode = &supported_modes[0];
    sc3336->cur_vts = sc3336->cur_mode->vts_def;
    sc3336->cur_hdr_mode = NO_HDR;

    of_property_read_u32(node, "rockchip,camera-module-index", &sc3336->module_index);
    of_property_read_string(node, RKMODULE_CAMERA_MODULE_FACING, &sc3336->module_facing);
    of_property_read_string(node, RKMODULE_CAMERA_MODULE_NAME, &sc3336->module_name);
    of_property_read_string(node, RKMODULE_CAMERA_LENS_NAME, &sc3336->len_name);

    sc3336->xvclk = devm_clk_get(dev, "xvclk");
    if (IS_ERR(sc3336->xvclk))
        return dev_err_probe(dev, PTR_ERR(sc3336->xvclk), "Failed to get xvclk\n");

    sc3336->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
    sc3336->pwdn_gpio = devm_gpiod_get_optional(dev, "pwdn", GPIOD_OUT_HIGH);

    for (i = 0; i < SC3336_NUM_SUPPLIES; i++)
        sc3336->supplies[i].supply = sc3336_supply_names[i];
    ret = devm_regulator_bulk_get(dev, SC3336_NUM_SUPPLIES, sc3336->supplies);
    if (ret && ret != -ENODEV)
        return dev_err_probe(dev, ret, "Failed to get supplies\n");

    mutex_init(&sc3336->mutex);
    sd = &sc3336->subdev;
    v4l2_i2c_subdev_init(sd, client, &sc3336_subdev_ops);

    /*
     * ★★★ 实体名必须改成 Rockchip 固定格式，否则 rkaiq 起不来 ★★★
     *
     *   v4l2_i2c_subdev_init() 确实会自动命名，但它给的是
     *       "sc3336 4-0030"        （驱动名 + i2c 地址）
     *   而 rkaiq 内部是按【固定位置】解析的：
     *       sscanf(&sns_entity_name[6], "%s", ctx->sns_name);
     *   它假设前 6 个字符是 m00_b_ 这种前缀，从第 7 个字符才是 model 名。
     *   用自动名 → sns_name 变成 "4-0030" → sysctl_init 返回 NULL，
     *   日志只吐一句 "MessageParser process loop exit!"，不说原因。
     *
     *   所以必须覆盖成：  m%02d_%s_%s %s
     *                     m00 _ b _ sc3336   4-0030
     *   （我上次就是漏了这步，官方 rkisp_demo 也崩，白查了很久）
     */
    {
        char facing[2] = { 'b', 0 };

        if (sc3336->module_facing && sc3336->module_facing[0] == 'f')
            facing[0] = 'f';

        snprintf(sd->name, sizeof(sd->name), "m%02d_%s_%s %s",
                 sc3336->module_index, facing, SC3336_NAME, dev_name(sd->dev));
        dev_info(dev, "subdev entity name = \"%s\"\n", sd->name);
    }

    /*
     * ★★★ 打开 /dev/v4l-subdevN 节点 ★★★
     *
     *   光有实体名还不够：rkaiq 拿到名字后还要 open() 对应的
     *   /dev/v4l-subdevN，再往这个 fd 下发 RKMODULE_* ioctl。
     *
     *   不设 HAS_DEVNODE 就【不会创建这个节点】：
     *       media-ctl 能看到 entity，但 /dev/v4l-subdev* 里没有它
     *   rkaiq open() 返回 NULL 后没判空，直接解引用 → SIGSEGV。
     *
     *   症状极具误导：stdout 是块缓冲，段错误来不及 flush，
     *   一行业务日志都看不到，只有 "Segmentation fault"。
     */
    sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_HAS_EVENTS;
    dev_info(dev, "subdev flags = 0x%x\n", sd->flags);

    ret = sc3336_init_controls(sc3336);
    if (ret)
        goto err_destroy_mutex;

    ret = sc3336_power_on(sc3336);
    if (ret)
        goto err_free_handler;

    ret = sc3336_read_reg(client, SC3336_REG_CHIP_ID, &chip_id);
    if (ret || (chip_id != 0xcc41 && chip_id != 0x3335 && chip_id != 0x3336)) {
        dev_err(dev, "Unexpected sensor id: 0x%04x (ret=%d)\n", chip_id, ret);
        goto err_power_off;
    }
    dev_info(dev, "Detected SC3336 sensor, ID: 0x%04x\n", chip_id);

    sc3336->pad.flags = MEDIA_PAD_FL_SOURCE;
    sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
    ret = media_entity_pads_init(&sd->entity, 1, &sc3336->pad);
    if (ret < 0)
        goto err_power_off;

    ret = v4l2_async_register_subdev_sensor_common(sd);
    if (ret) {
        dev_err(dev, "v4l2 async register subdev failed\n");
        goto err_clean_entity;
    }

    pm_runtime_set_active(dev);
    pm_runtime_enable(dev);
    pm_runtime_idle(dev);
    return 0;

err_clean_entity:
    media_entity_cleanup(&sd->entity);
err_power_off:
    sc3336_power_off(sc3336);
err_free_handler:
    v4l2_ctrl_handler_free(&sc3336->ctrl_handler);
err_destroy_mutex:
    mutex_destroy(&sc3336->mutex);
    return ret;
}

static int sc3336_remove(struct i2c_client *client)
{
    struct v4l2_subdev *sd = i2c_get_clientdata(client);
    struct sc3336 *sc3336 = to_sc3336(sd);

    v4l2_async_unregister_subdev(sd);
    media_entity_cleanup(&sd->entity);
    v4l2_ctrl_handler_free(&sc3336->ctrl_handler);
    mutex_destroy(&sc3336->mutex);
    pm_runtime_disable(&client->dev);
    if (!pm_runtime_status_suspended(&client->dev))
        sc3336_power_off(sc3336);
    pm_runtime_set_suspended(&client->dev);
    return 0;
}

static const struct of_device_id sc3336_of_match[] = {
    { .compatible = "smartsens,sc3336" },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sc3336_of_match);

static struct i2c_driver sc3336_i2c_driver = {
    .driver = {
        .name = SC3336_NAME,
        .pm = &sc3336_pm_ops,
        .of_match_table = sc3336_of_match,
    },
    .probe = sc3336_probe,
    .remove = sc3336_remove,
};

module_i2c_driver(sc3336_i2c_driver);

MODULE_DESCRIPTION("SmartSens SC3336 Sensor Driver (Full Features)");
MODULE_LICENSE("GPL v2");