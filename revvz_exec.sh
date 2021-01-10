#!/system/bin/sh

#This file may change on a newer REVVZ version.
#Do not attempt to modify.

#Default settings

#Schedutil changes
echo 1500 > /sys/devices/system/cpu/cpufreq/schedutil/up_rate_limit_us
echo 1000 > /sys/devices/system/cpu/cpufreq/schedutil/down_rate_limit_us
echo 1401600 > /sys/devices/system/cpu/cpufreq/schedutil/hispeed_freq
echo 85 > /sys/devices/system/cpu/cpufreq/schedutil/hispeed_load

#GPU(KGSL) changes
echo 6 > /sys/devices/platform/soc/1c00000.qcom,kgsl-3d0/kgsl/kgsl-3d0/default_pwrlevel

#VM Changes
echo 40 > /proc/sys/vm/dirty_ratio
echo 60 > /proc/sys/vm/vfs_cache_pressure
echo 85 > /proc/sys/vm/swappiness
echo 3600 > /proc/sys/vm/stat_interval

#Misc changes
sysctl -w net.ipv4.tcp_congestion_control=westwood

#Uclamp changes (reduces jitter,apparently)

#system-wide
sysctl -w kernel.sched_rt_default_util_clamp_min=500
sysctl -w kernel.sched_util_clamp_min=128

#top-app
echo max > /dev/cpuset/top-app/uclamp.max
echo 85 > /dev/cpuset/top-app/uclamp.min
echo 0   > /dev/cpuset/top-app/uclamp.latency_sensitive 

#foreground
echo 50 > /dev/cpuset/foreground/uclamp.max
echo 20 > /dev/cpuset/foreground/uclamp.min
echo 0  > /dev/cpuset/foreground/uclamp.latency_sensitive

#background
echo max > /dev/cpuset/background/uclamp.max
echo 20 > /dev/cpuset/background/uclamp.min
echo 0  > /dev/cpuset/background/uclamp.latency_sensitive

#system-background
echo 50 > /dev/cpuset/system-background/uclamp.max
echo 10 > /dev/cpuset/system-background/uclamp.min
echo 0  > /dev/cpuset/system-background/uclamp.latency_sensitive

#camera-daemon
echo 20 > /dev/cpuset/camera-daemon/uclamp.max
echo 0 > /dev/cpuset/camera-daemon/uclamp.min
echo 0  > /dev/cpuset/camera-daemon/uclamp.latency_sensitive

#Default settings END

#Launch onboot.sh

if [[ ! -f /vendor/bin/onboot.sh ]]; then
mount -o remount,rw /vendor
echo "# Enter your custom onboot commands here" > /vendor/bin/onboot.sh
mount -o remount,ro /vendor
fi

/system/bin/sh -e /vendor/bin/onboot.sh
