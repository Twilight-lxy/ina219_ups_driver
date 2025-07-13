# 快速安装指南

## 简化安装步骤

### 1. 准备环境
```bash
sudo apt update
sudo apt install -y dkms build-essential raspberrypi-kernel-headers
```

### 2. 启用I2C (编辑 /boot/firmware/config.txt)
```ini
dtparam=i2c_arm=on
```

### 3. 验证硬件 (重启后)
```bash
sudo i2cdetect -y 1  # 应该看到地址0x41
```

### 4. 编译并安装
```bash
# 编译驱动
make clean && make

# 安装设备树
sudo cp rpi-ups-pi5.dts /boot/firmware/
sudo dtc -I dts -O dtb -o /boot/firmware/overlays/rpi-ups-pi5.dtbo /boot/firmware/rpi-ups-pi5.dts

# DKMS安装
sudo cp -r . /usr/src/rpi-ups-0.4/
sudo dkms add -m rpi-ups -v 0.4
sudo dkms build -m rpi-ups -v 0.4
sudo dkms install -m rpi-ups -v 0.4

# 启用设备树叠加 (编辑 /boot/firmware/config.txt)
echo "dtoverlay=rpi-ups-pi5" >> /boot/firmware/config.txt
```

### 5. 重启并测试
```bash
sudo reboot

# 测试
./test_driver.sh
```

详细说明请参考 README.md
