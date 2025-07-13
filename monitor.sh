#!/bin/bash

echo "=== Waveshare UPS Module 3S Monitor ==="
echo "Press Ctrl+C to stop"
echo ""

while true; do
    clear
    echo "=== Waveshare UPS Module 3S Monitor ==="
    echo "Time: $(date)"
    echo ""
    
    # 读取所有属性
    voltage=$(cat /sys/class/power_supply/UPS-Module-3S/voltage_now 2>/dev/null)
    current=$(cat /sys/class/power_supply/UPS-Module-3S/current_now 2>/dev/null)
    power=$(cat /sys/class/power_supply/UPS-Module-3S/power_now 2>/dev/null)
    capacity=$(cat /sys/class/power_supply/UPS-Module-3S/capacity 2>/dev/null)
    status=$(cat /sys/class/power_supply/UPS-Module-3S/status 2>/dev/null)
    present=$(cat /sys/class/power_supply/UPS-Module-3S/present 2>/dev/null)
    
    if [ -n "$voltage" ]; then
        # 转换单位
        voltage_v=$(echo "scale=3; $voltage / 1000000" | bc)
        current_ma=$(echo "scale=1; $current / 1000" | bc)
        power_w=$(echo "scale=2; $power / 1000000" | bc)
        
        echo "Device Present: $present"
        echo "Status: $status"
        echo "Voltage: ${voltage_v}V (${voltage} μV)"
        echo "Current: ${current_ma}mA (${current} μA)"
        echo "Power: ${power_w}W (${power} μW)"
        echo "Capacity: ${capacity}%"
        echo ""
        echo "Per Cell Voltage: $(echo "scale=3; $voltage_v / 3" | bc)V"
        
        if [ "$current" -gt 50000 ]; then
            echo "Status: CHARGING (Current > 50mA)"
        elif [ "$current" -lt -50000 ]; then
            echo "Status: DISCHARGING (Current < -50mA)"
        else
            echo "Status: STANDBY (|Current| < 50mA)"
        fi
    else
        echo "ERROR: Cannot read UPS data. Device may not be present."
    fi
    
    sleep 2
done
