# Nexmon GCC plugin compiled for arm-none-eabi toolchain with GCC 7 on Kali armel (or other armel distros)

## Nexmon gcc plugin compiled for arm-none-eabi toolchain on armel host (armv5 or v6 without hardfloat)

Plugin is compiled with arm-linux-gnueabi gcc-7, because Kali for armel ships the
arm-none-eabi bare metal crosscompile toolchain with gcc-7 (at time of this writing).

The imports of the plugin have been fixed to apply to gcc-7 plugin API, as the original code
of nexmon.c only compiled on gcc-5.

To compile the nexmon firmware/driver on Kali armel (online compilation for RPi0/RPi1, which both use Kali armel)
the arm-none-eabi toolchain has to be installed.

The setup_env.sh script of this branch has been modified to use the arm-none-eabi toolchain and the precompiled
nexmon plugin from this folder, in case the arch is detected as "armv6l" (so this shouldn't be used
on distros with hardfloat support for ARMv6, like Raspbian).

## Hints on build toolchain

Install embedded GNU arm toolchain (for online nexmon compilation) with:

```
apt-get install binutils-arm-none-eabi gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib gcc-arm-none-eabi-source 
```

Install arm-linux-gnueabi gcc-7 and g++-7 with plugin headers for nexmon plugin compilation (note: the plugin in this folder is pre-compiled)

```
apt-get install gcc-7 g++-7 gcc-7-plugin-dev
```

