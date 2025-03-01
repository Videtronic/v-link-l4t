## NOTE - drivers will be released upon product launch which is scheduled for April 2025.

# Videtronic V-Link drivers for Nvidia platforms 

Welcome to the official Videtronic repository for the **v-link** drivers!  

The **v-link** product line integrates advanced serializer and deserializer technology featuring the **MAX96717** and **MAX96714** to deliver seamless and high-performance video data transmission over distances of up to 15 meters, thanks to the extended range capabilities of **GMSL2**.

For Raspberry Pi support visit our **v-link-rpi** repository: [here](https://github.com/Videtronic/v-link-rpi)
## About Videtronic
At Videtronic, we design and manufacture various embedded vision products. Our goal is to create solutions that will be used in cutting-edge vision projects.

Experience our comprehensive range of services, from advanced embedded vision products to full-scale production. We deliver tailored solutions and reliable support to meet all your vision technology needs.

Visit us at https://videtronic.com/

## Features
- **Serializer**: Based on MAX96717
- **Deserializer**: Based on MAX96714
- **Platform Compatibility**: AGX Orin, Orin Nano Super
- **Camera Module Support**: All officially supported and pin compatible modules

## Why Choose V-Link?
- Seamless integration with Nvidia platforms
- High compatibility with multiple camera modules
- Easy installation process
- Reliable performance for video data transmission

## Versioning
This repository is versioned based on the Jetson Linux release. Each branch corresponds to a specific release (e.g., `l4t-r36`). Ensure you use the branch matching your version for optimal compatibility.

For more information regarding Jetson Linux, visit official Nvidia website [here](https://developer.nvidia.com/embedded/jetson-linux)

## Quickstart Guide

### Prerequisites
1. A compatible Nvidia developer kit with **Jetson Linux** installed.  
> **Note**: For AGX Orin, a camera interposer board is required. Also available at Videtronic.
2. Essential build tools, including ```make``` and a C compiler, should be installed:
   ```bash
   sudo apt install build-essential
   ```
3. Git installed (if not already):
   ```bash
   sudo apt install git
   ```

### Step 1: Jetson Linux version
Check the current kernel version on your Nvidia platform:
```bash
cat /etc/nv_tegra_release 
```
Sample output:
```
# R36 (release), REVISION: 4.3, GCID: 38968081, BOARD: generic, EABI: aarch64, DATE: Wed Jan  8 01:49:37 UTC 2025
# KERNEL_VARIANT: oot
TARGET_USERSPACE_LIB_DIR=nvidia
TARGET_USERSPACE_LIB_DIR_PATH=usr/lib/aarch64-linux-gnu/nvidia
```
This indicates you are using the Jeton Linux 36.X.Y version.

### Step 2: Clone the Appropriate Branch
Clone the repository branch matching your linux version. For the **R36** branch:
```bash
git clone --branch l4t-r36 https://github.com/Videtronic/v-link-l4t.git
cd v-link-l4t
```
### Step 3: Enter driver directory
```bash
cd v-link-ser
```
or
```bash
cd v-link-deser
```

### Step 4: Build the Drivers 
Use the provided `Makefile` to build the drivers:
```bash
make
```

### Step 5: Install the Drivers 
Install the drivers using:
```bash
sudo make install
```
**Repeat step 4 and 5 for v-link-deser**  

### Step 6: Configure the Device Tree Overlay
1. Based on developer kit, copy the desired overlay file(s) to the `/boot/` directory.  
For example:
```
sudo cp overlays/orin_nano_super/tegra234-p3767-camera-p3768-imx477-vlink-cam0.dtbo /boot
```

If required, overlays can be built using following command
```
dtc -I dts -O dtb -@ <name_of_overlay>.dts -o <name_of_overlay>.dtbo
```
You can verify if copied overlay is successfully detected by issuing the following command:
```
sudo /opt/nvidia/jetson-io/config-by-hardware.py -l
```

Sample output:
```
Header 1 [default]: Jetson 40pin Header
  Available hardware modules:
  1. Adafruit SPH0645LM4H
  2. Adafruit UDA1334A
  3. FE-PI Audio V1 and Z V2
  4. ReSpeaker 4 Mic Array
  5. ReSpeaker 4 Mic Linear Array
Header 2: Jetson 24pin CSI Connector
  Available hardware modules:
  1. Camera IMX219 (CAM0) over v-link
  2. Camera IMX219 (CAM1) over v-link
  3. Camera IMX219 Dual
  4. Camera IMX219-A
  5. Camera IMX219-A and IMX477-C
  6. Camera IMX219-C
  7. Camera IMX477 (CAM0) over v-link
  8. Camera IMX477 (CAM1) over v-link
  9. Camera IMX477 Dual
  10. Camera IMX477 Dual 4 lane
  11. Camera IMX477-A
  12. Camera IMX477-A and IMX219-C
  13. Camera IMX477-C
Header 3: Jetson M.2 Key E Slot
  No hardware configurations found!
```

If everything works as expected, You should see name of the hardware module that copied overlay adds (in this exmaple "Camera IMX477 (CAM0) over v-link").

2. Apply overlay:
  To apply overlay previously copied issue the following command:
  ```
  sudo /opt/nvidia/jetson-io/config-by-hardware.py -n 2="Camera IMX477 (CAM0) over v-link"
  ```

### Step 7: Reboot
Reboot your board for the changes to take effect:
```bash
sudo reboot
```

## Troubleshooting

- **Q: When using single camera module with v-link my camera preview is black**  
  A: Boost clocks using following script
  ```bash
  sudo chmod +x scripts/boost_clocks.sh
  sudo su
  ./boost_clocks.sh
  ```  



## Contribution Guidelines
We welcome contributions to improve our drivers and documentation! To contribute:
1. Fork the repository.
2. Create a new branch for your feature or bugfix.
3. Submit a pull request with a clear description of your changes.

## Support
If you encounter issues or have any questions, feel free to [contact us](https://www.videtronic.pl) or open an issue on this repository.

---

![Build Status](https://img.shields.io/badge/build-passing-brightgreen)  
![Nvidia](https://img.shields.io/badge/Nvidia-AGX_Orin%2COrin_Nano_Super-blue)