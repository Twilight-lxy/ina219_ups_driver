#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/reboot.h>
#include <linux/power_supply.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/device.h>

/*UPS 基本配置*/
#define INA219 (0x41 << 1) // INA219 I2C 地址
#define DRIVER_NAME "UPS-Module-3S"
#define DRIVER_MANUFACTURER "WaveShare"
#define DESIGN_MAX_VOLTAGE_mV (4200)/* 单节锂电池满电电压 4.2V */
#define DESIGN_MIN_VOLTAGE_mV (3000)/* 单节锂电池低电压 3.0V */

#define DATA_TIMEOUT_MS 5000            /* 5 seconds timeout */

/*寄存器地址*/
#define	INA219_REG_CONFIG 0x00 // INA219 配置寄存器
#define INA219_REG_SHUNTVOLTAGE 0x01 // INA219 分流电压寄存器
#define INA219_REG_BUSVOLTAGE 0x02 // INA219 总线电压寄存器
#define INA219_REG_POWER 0x03 // INA219 功率寄存器
#define INA219_REG_CURRENT 0x04 // INA219 电流寄存器
#define INA219_REG_CALIBRATION 0x05 // INA219 校准寄存器

/*INA219配置*/
/*
| Bit | 字段  | 类型  | 默认值(二进制) | 描述  |
| --- | --- | --- | --- | --- |
| 15  | RST | R/W | 0   | 设置成1 复位 |
| 14  | NC  | R/W | 0   | 无功能 保留 |
| 13  | BRNG | R/W | 1   | 总线电压量程范围：0 = 16V FSR；1 = 32V FSR |
| 11 12 | PG  | R/W | 11  | 分流电阻最大电压，可设置最大量程电流，I=V/R：00 ±40 mV；01 ±80 mV；10 ±160 mV；11 ±320 mV |
| 7:10 | BADC | R/W | 0011 | 总线ADC分辨率及采样次数  |
| 3:6 | SADC | R/W | 0011 | 分流ADC分辨率及采样次数  |
| 0:2 | MODE | R/W | 111 | 操作模式：000 关闭；001 分流电压 触发；010 总线电压 触发；011 分流及总线电压 触发；100 关闭ADC；101 分流电压 连续测量；110 总线电压 连续测量；111 分流及总线电压 连续测量 |
*/
#define RST 0 // 默认 （Bit15）
#define NC 0 // 保留位 （Bit14）
#define BRNG 0x01 // 总线电压范围 32V （Bit13）
#define PG 0x03 // 分流电阻最大电压 ±320mV （Bit11-12）
#define BADC 0x0D // 总线电压 ADC 分辨率/平均值设置 12bit,  32 samples, 17.02ms （Bit7–10）
#define SADC 0x0D // 分压电阻 ADC 分辨率/平均值设置 12bit,  32 samples, 17.02ms （bit3-6）
#define MODE 0x07 // 连续测量分流电压和总线电压 （Bit0–2）

#define Configuration_H (RST << 7)|(NC << 6)|(BRNG << 5)|(PG << 3)|((BADC) & 0x0F)
#define Configuration_L ((BADC & 0x01) << 7)|(SADC << 3)|(MODE)

/* INA219_Calibration -------------------------------------------------------------------------------------------------*/
//4096 0x1000
#define Calibration_H 0x10
#define Calibration_L 0x00

struct rpi_ups_data
{
    struct i2c_client *client;
    /* 电池设备 */
    struct power_supply *battery;
    struct power_supply_desc battery_desc;
    struct power_supply_config battery_cfg;
    /* 缓存数据 - 使用整数避免浮点运算 */
    int current_ma; /* 电流（单位：毫安） */
    int shunt_voltage_uv; /* 分流电压（单位：10微伏） */
    int bus_voltage_mv; /* 总线电压（单位：毫伏） */
    int power_mw; /* 功率（单位：毫瓦） */
    bool charger_status; /* DC Charger 状态 */
    unsigned long last_update_jiffies; /* 最近一次数据更新时刻 */
    spinlock_t lock;                   /* 保护缓存数据 */
    struct task_struct *update_thread; /* 后台更新线程 */
};

static enum power_supply_property rpi_ups_battery_props[] = {
    POWER_SUPPLY_PROP_STATUS,             /* 电池充放电状态 */
    POWER_SUPPLY_PROP_PRESENT,            /* 电池是否存在 */
    POWER_SUPPLY_PROP_VOLTAGE_NOW,        /* 当前电池电压（单位：微伏） */
    POWER_SUPPLY_PROP_CURRENT_NOW,        /* 当前电池电流（单位：微安） */
    POWER_SUPPLY_PROP_POWER_NOW,          /* 当前电池功率（单位：微瓦） */
    POWER_SUPPLY_PROP_CAPACITY,           /* 百分比 */
    POWER_SUPPLY_PROP_MODEL_NAME,         /* 电池型号 */
    POWER_SUPPLY_PROP_MANUFACTURER,       /* 制造商 */
    POWER_SUPPLY_PROP_CAPACITY_ALERT_MIN, /* 最低电量警告 */
};

static uint8_t Get_shunt_voltage(struct i2c_client *client, int *shunt_voltage_uv)
{
    int ret = i2c_smbus_read_word_data(client, INA219_REG_SHUNTVOLTAGE);
    if (ret < 0) {
        dev_err(&client->dev, "Failed to read shunt voltage\n");
        return ret;
    }

    // 交换字节序：从小端转换为大端
    uint16_t raw = ((ret & 0xFF) << 8) | ((ret >> 8) & 0xFF);
    int16_t shunt_voltage = (int16_t)raw;
    
    // LSB = 10μV，保存为10μV为单位
    *shunt_voltage_uv = (int)shunt_voltage;

    return 0;
}

static uint8_t Get_bus_voltage(struct i2c_client *client, int *bus_voltage_mv)
{
    int ret = i2c_smbus_read_word_data(client, INA219_REG_BUSVOLTAGE);
    if (ret < 0) {
        dev_err(&client->dev, "Failed to read bus voltage\n");
        return ret;
    }

    // 交换字节序：从小端转换为大端
    uint16_t raw = ((ret & 0xFF) << 8) | ((ret >> 8) & 0xFF);
    
    // 总线电压的格式：右移3位，LSB = 4mV
    *bus_voltage_mv = (raw >> 3) * 4;

    return 0;
}

static uint8_t Get_power(struct i2c_client *client, int *power_mw)
{
    int ret = i2c_smbus_read_word_data(client, INA219_REG_POWER);
    if (ret < 0) {
        dev_err(&client->dev, "Failed to read power\n");
        return ret;
    }

    // 交换字节序：从小端转换为大端
    uint16_t raw = ((ret & 0xFF) << 8) | ((ret >> 8) & 0xFF);
    
    // 功率的格式：每个LSB = 20 * 电流LSB = 20 * 1mA = 20mW
    *power_mw = raw * 20;

    return 0;
}

static uint8_t Get_current(struct i2c_client *client, int *current_ma)
{
    int ret = i2c_smbus_read_word_data(client, INA219_REG_CURRENT);
    if (ret < 0) {
        dev_err(&client->dev, "Failed to read current\n");
        return ret;
    }

    // 交换字节序：从小端转换为大端
    uint16_t raw = ((ret & 0xFF) << 8) | ((ret >> 8) & 0xFF);
    int16_t current_raw = (int16_t)raw;
    
    // 电流的格式：根据校准值4096，LSB = 1mA
    // Current_LSB = 0.04096 / Calibration_Register = 0.04096 / 4096 = 1mA
    *current_ma = (int)current_raw;  // 直接以mA为单位

    return 0;
}

static uint8_t Initialize_ina219(struct i2c_client *client)
{
    int ret;
    uint16_t config = (Configuration_H << 8) | Configuration_L;
    uint16_t calib = (Calibration_H << 8) | Calibration_L;
    
    dev_info(&client->dev, "Initializing INA219 with config=0x%04x, calib=0x%04x\n", config, calib);
    
    // 先尝试简单的读取来验证通信
    ret = i2c_smbus_read_word_data(client, INA219_REG_CONFIG);
    if (ret < 0) {
        dev_err(&client->dev, "Failed to read from device, check I2C connection: %d\n", ret);
        return 1;
    }
    dev_info(&client->dev, "Current config register (raw): 0x%04x\n", ret);

    // 写入配置寄存器 - 需要转换字节序
    uint16_t config_swapped = ((config & 0xFF) << 8) | ((config >> 8) & 0xFF);
    ret = i2c_smbus_write_word_data(client, INA219_REG_CONFIG, config_swapped);
    if (ret < 0)
    {
        dev_err(&client->dev, "Failed to write configuration data: %d\n", ret);
        return 1;
    }

    // 写入校准寄存器 - 需要转换字节序
    uint16_t calib_swapped = ((calib & 0xFF) << 8) | ((calib >> 8) & 0xFF);
    ret = i2c_smbus_write_word_data(client, INA219_REG_CALIBRATION, calib_swapped);
    if (ret < 0)
    {
        dev_warn(&client->dev, "Failed to write calibration data: %d\n", ret);
        return 1;
    }
    
    // 短暂延时让设备稳定
    msleep(10);
    
    // 验证配置是否写入成功
    ret = i2c_smbus_read_word_data(client, INA219_REG_CONFIG);
    if (ret >= 0) {
        uint16_t readback = ((ret & 0xFF) << 8) | ((ret >> 8) & 0xFF);
        dev_info(&client->dev, "Configuration readback: 0x%04x (expected: 0x%04x)\n", readback, config);
    }
    
    // 验证校准寄存器
    ret = i2c_smbus_read_word_data(client, INA219_REG_CALIBRATION);
    if (ret >= 0) {
        uint16_t readback = ((ret & 0xFF) << 8) | ((ret >> 8) & 0xFF);
        dev_info(&client->dev, "Calibration readback: 0x%04x (expected: 0x%04x)\n", readback, calib);
    }

    // 尝试读取一些基本数据来验证设备工作
    ret = i2c_smbus_read_word_data(client, INA219_REG_BUSVOLTAGE);
    if (ret >= 0) {
        uint16_t raw = ((ret & 0xFF) << 8) | ((ret >> 8) & 0xFF);
        dev_info(&client->dev, "Bus voltage register: 0x%04x (%d mV)\n", raw, ((raw >> 3) * 4));
    }

    return 0;
}

/*
 * 更新线程：定时读取寄存器数据
 */
static int rpi_ups_update_thread(void *arg)
{
    struct rpi_ups_data *data = arg;
    if (!data)
        return -EINVAL;
    int ret;
    unsigned long flags;
    unsigned long last_update_jiffies = 0; /* 上次更新，默认超时 */
    int status;

    while (!kthread_should_stop())
    {
        // 读取分流电压
        ret = Get_shunt_voltage(data->client, &status);
        if (ret == 0) {
           spin_lock_irqsave(&data->lock, flags);
           data->shunt_voltage_uv = status; // 更新分流电压
           spin_unlock_irqrestore(&data->lock, flags);
        }
        // 读取总线电压
        ret = Get_bus_voltage(data->client, &status);
        if (ret == 0) {
            spin_lock_irqsave(&data->lock, flags);
            data->bus_voltage_mv = status;
            spin_unlock_irqrestore(&data->lock, flags);
        }
        // 读取电流
        ret = Get_current(data->client, &status);
        if (ret == 0) {
            bool charger_status_temp = false;
            if (status > 50) { // 如果电流大于50mA，认为正在充电
                charger_status_temp = true;
            }
            spin_lock_irqsave(&data->lock, flags);
            data->current_ma = status;
            data->charger_status = charger_status_temp;
            spin_unlock_irqrestore(&data->lock, flags);
        }
        // 读取功率
        ret = Get_power(data->client, &status);
        if (ret == 0) {
            spin_lock_irqsave(&data->lock, flags);
            data->power_mw = status;
            spin_unlock_irqrestore(&data->lock, flags);
        }
        // 更新数据时间戳
        if (ret == 0)
        {
            spin_lock_irqsave(&data->lock, flags);
            data->last_update_jiffies = jiffies;
            last_update_jiffies = data->last_update_jiffies;
            spin_unlock_irqrestore(&data->lock, flags);
        }
        msleep(1000); /* 每 1 秒执行一次循环 */
    }
    return 0;
}

/*
 * 电池设备 get_property：从缓存数据中返回信息，若数据超时则将 PRESENT 返回为 0
 */
static int rpi_ups_battery_get_property(struct power_supply *psy,
                                        enum power_supply_property psp,
                                        union power_supply_propval *val)
{
    struct rpi_ups_data *data = power_supply_get_drvdata(psy);
    unsigned long flags;
    unsigned long now = jiffies;

    // 检查缓存数据是否过时
    spin_lock_irqsave(&data->lock, flags);
    if (time_after(now, data->last_update_jiffies + msecs_to_jiffies(DATA_TIMEOUT_MS)))
    {
        if (psp == POWER_SUPPLY_PROP_PRESENT)
        {
            val->intval = 0;
            spin_unlock_irqrestore(&data->lock, flags);
            return 0;
        }
    }
    // 从缓存中获取数据
    switch (psp)
    {
    case POWER_SUPPLY_PROP_STATUS:
    {
        val->intval = data->charger_status == true ? POWER_SUPPLY_STATUS_CHARGING : POWER_SUPPLY_STATUS_DISCHARGING;
        break;
    }
    case POWER_SUPPLY_PROP_PRESENT:
    {
        if (time_after(now, data->last_update_jiffies + msecs_to_jiffies(DATA_TIMEOUT_MS)))
            val->intval = 0;
        else
            val->intval = 1;
        break;
    }
    case POWER_SUPPLY_PROP_VOLTAGE_NOW:
        val->intval = data->bus_voltage_mv * 1000; // 转换为微伏
        break;
    case POWER_SUPPLY_PROP_CURRENT_NOW:
    {
        val->intval = data->current_ma * 1000; // 转换为微安
        break;
    }
    case POWER_SUPPLY_PROP_CAPACITY:
    {
        int voltage = data->bus_voltage_mv;
        int min_voltage = DESIGN_MIN_VOLTAGE_mV * 3; // 3节电池串联
        int max_voltage = DESIGN_MAX_VOLTAGE_mV * 3; // 3节电池串联
        int percent = ((voltage - min_voltage) * 100) / (max_voltage - min_voltage);
        val->intval = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
        break;
    }
    case POWER_SUPPLY_PROP_MODEL_NAME:
        val->strval = DRIVER_NAME; // 电池型号
        break;
    case POWER_SUPPLY_PROP_MANUFACTURER:
        val->strval = DRIVER_MANUFACTURER; // 制造商
        break;
    case POWER_SUPPLY_PROP_POWER_NOW:
    {
        int raw_power = data->power_mw * 1000; // 转换为微瓦
        val->intval = raw_power;
        break;
    }
    case POWER_SUPPLY_PROP_CAPACITY_ALERT_MIN:
        val->intval = 5; /* 5% */
        break;
    default:
        spin_unlock_irqrestore(&data->lock, flags);
        return -EINVAL;
    }
    spin_unlock_irqrestore(&data->lock, flags);
    return 0;
}

static int rpi_ups_probe(struct i2c_client *client)
{
    struct rpi_ups_data *data;
    int ret;

    data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;

    data->client = client;
    i2c_set_clientdata(client, data);
    spin_lock_init(&data->lock);
    data->last_update_jiffies = jiffies;

    /* 初始化 INA219 */
    ret = Initialize_ina219(client);
    if (ret != 0) {
        dev_err(&client->dev, "Failed to initialize INA219\n");
        return ret;
    }

    /* 注册电池设备 */
    data->battery_desc.name = DRIVER_NAME;
    data->battery_desc.type = POWER_SUPPLY_TYPE_BATTERY;
    data->battery_desc.properties = rpi_ups_battery_props;
    data->battery_desc.num_properties = ARRAY_SIZE(rpi_ups_battery_props);
    data->battery_desc.get_property = rpi_ups_battery_get_property;
    data->battery_cfg.drv_data = data;

    data->battery = power_supply_register(&client->dev,
                                          &data->battery_desc,
                                          &data->battery_cfg);
    if (IS_ERR(data->battery))
    {
        ret = PTR_ERR(data->battery);
        dev_err(&client->dev, "Failed to register battery: %d\n", ret);
        return ret;
    }

    /* 启动更新线程 */
    data->update_thread = kthread_run(rpi_ups_update_thread, data, "rpi_ups_update");
    if (IS_ERR(data->update_thread))
    {
        ret = PTR_ERR(data->update_thread);
        dev_err(&client->dev, "Failed to start update thread: %d\n", ret);
        power_supply_unregister(data->battery);
        return ret;
    }

    dev_info(&client->dev, "Raspberrypi UPS driver probed\n");
    return 0;
}

static void rpi_ups_remove(struct i2c_client *client)
{
    struct rpi_ups_data *data = i2c_get_clientdata(client);
    if (data->update_thread)
        kthread_stop(data->update_thread);
    power_supply_unregister(data->battery);
    dev_info(&client->dev, "Raspberrypi UPS driver removed\n");
}

static const struct i2c_device_id rpi_ups_id[] = {
    {DRIVER_NAME, 0},
    {},
};
MODULE_DEVICE_TABLE(i2c, rpi_ups_id);

static const struct of_device_id rpi_ups_of_match[] = {
    {
        .compatible = "rpi,ups",
    },
    {}};
MODULE_DEVICE_TABLE(of, rpi_ups_of_match);

static struct i2c_driver rpi_ups_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = rpi_ups_of_match,
    },
    .probe = rpi_ups_probe,
    .remove = rpi_ups_remove,
    .id_table = rpi_ups_id,
};

module_i2c_driver(rpi_ups_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Twilightly");
MODULE_DESCRIPTION("Waveshare UPS Module 3S for RaspberryPi Driver");
