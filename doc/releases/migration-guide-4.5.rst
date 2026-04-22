:orphan:

..
  See
  https://docs.zephyrproject.org/latest/releases/index.html#migration-guides
  for details of what is supposed to go into this document.

.. _migration_4.5:

Migration guide to Zephyr v4.5.0 (Working Draft)
################################################

This document describes the changes required when migrating your application from Zephyr v4.4.0 to
Zephyr v4.5.0.

Any other changes (not directly related to migrating applications) can be found in
the :ref:`release notes<zephyr_4.5>`.

.. contents::
    :local:
    :depth: 2

Common
******

Build System
************

Kernel
******

Boards
******

Device Drivers and Devicetree
*****************************

.. Group contents in this section by subsystem, e.g.:
..
.. ADC
.. ===
..
.. ...

.. zephyr-keep-sorted-start re(^\w) ignorecase

Clock Control
=============

* The :dtcompatible:`nxp,imxrt11xx-arm-pll` binding now uses ``loop-div`` and
  ``post-div`` for ARM PLL configuration. The legacy ``clock-mult`` and
  ``clock-div`` properties remain supported but are deprecated. Existing
  RT11xx overlays should be updated using the mapping
  ``loop-div = clock-mult * 2`` and ``post-div = clock-div``.

Digital Microphone
==================

* The DMIC driver backend API now uses :c:struct:`dmic_driver_api` instead of ``struct _dmic_ops``.

  Out-of-tree DMIC drivers must rename their backend API struct definitions and switch their API
  instances to ``DEVICE_API(dmic, ...)``. See :github:`107695` for examples of how in-tree drivers
  have been updated. Application code using :c:func:`dmic_configure`, :c:func:`dmic_trigger`, and
  :c:func:`dmic_read` is not impacted.

Ethernet
========

* ``ETHERNET_CONFIG_TYPE_T1S_PARAM`` and the related ``NET_REQUEST_ETHERNET_SET_T1S_PARAM`` has
  been removed. :c:func:`phy_set_plca_cfg` together with :c:func:`net_eth_get_phy` should be
  used instead to set these parameters (:github:`108136`).

* In the functions implemented by the :c:struct:`ethernet_api` a additional argument was added for
  a pointer to :c:struct:`net_if`. This api is not directly exposed to the application, so only
  out-of-tree drivers need to be updated. (:github:`106086`)

Flash
=====
* :dtcompatible:`jedec,spi-nand` now requires a ``plane-bytes`` property, which indicates the size
  of each plane in the flash device. For devices with a single plane, this should be set to the
  same value as ``size-bytes``.

GPIO
====

* The STM32 GPIO driver now returns ``-EINVAL`` when attempting to configure a GPIO pin in disabled
  state with a pull-up/pull-down resistor using :c:func:`gpio_pin_configure`. The driver would
  previously return ``0`` without actually honoring those flags (no PU/PD resistor was enabled).
  Applications encountering this error should remove :c:macro:`GPIO_PULL_UP`/ :c:macro:`GPIO_PULL_DOWN`
  from the ``flags`` they provide to :c:func:`gpio_pin_configure`; this will result in the same
  behavior as before since these flags were effectively ignored. (:github:`104690`)

* On STM32F1 series, GPIO output pins now use 50 MHz max. speed instead of 10 MHz. (:github:`104690`)

STM32
=====

* SoC DTSI files now consistently use interrupt priority zero for all peripherals.
  Applications must now explicitly configure interrupt priorities using Devicetree
  if they previously relied on the values found in SoC DTSI files. (:github:`106188`)

WiFi
====

* In the functions implemented by the :c:struct:`net_wifi_mgmt_offload`, internally
  :c:struct:`ethernet_api` and :c:struct:`wifi_mgmt_ops`, a additional argument was added for
  a pointer to :c:struct:`net_if`. This api is not directly exposed to the application, so only
  out-of-tree drivers need to be updated. (:github:`106086`)

.. zephyr-keep-sorted-stop

Bluetooth
*********


Networking
**********

Other subsystems
****************

Modules
*******

Architectures
*************
