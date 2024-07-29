# hardware-audio #

This module contains the STMicroelectronics Audio HAL source code.

It is part of the STMicroelectronics delivery for Android.

## Description ##

This module version is the updated version for STM32MPU distribution for Android
Please see the Android delivery release notes for more details.

It generates the library "audio.primary.stm" used to implement vendor-specific functionalities for the Android default Audio service.

The configuration file audio.${PRODUCT_DEVICE}.xml should be on the "/vendor/etc" or "/system/etc" folder. It defines the default configuration states (input, output...) of the driver.
More details can be found in audio.example.xml.
The xml is parsed by the "libaudiohalcm" library.

Please see the release notes for more details.

## Documentation ##

* The [release notes][] provide information on the release.
[release notes]: https://wiki.st.com/stm32mpu/wiki/STM32_MPU_OpenSTDroid_release_note_-_v5.1.0

## Dependencies ##

This module can not be used alone. It is part of the STMicroelectronics delivery for Android.
It provides the audio library used by the default implementation of android.hardware.audio@<version>-service which has to be added in device.mk as follow:
```
PRODUCT_PACKAGES += \
    libtinyalsa \
    libaudiohalcm \
    audio.primary.stm \
    android.hardware.audio@<version>-service \
    android.hardware.audio@<version>-impl
```

## Contents ##

This directory contains the sources and the associated Android makefile to generate the audio.primary.stm library.

## License ##

This module is distributed under the Apache License, Version 2.0 found in the [LICENSE](./LICENSE) file.
