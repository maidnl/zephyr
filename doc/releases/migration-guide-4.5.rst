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

* :dtcompatible:`st,stm32-sai` binding has been restructured to reflect the SAI hardware
  topology. The parent node now represents the SAI Block controller, while a new
  ``child-binding`` represents the SAI Sub-Block instances.
  The following properties shall be moved from the parent SAI node to a child sub-block node:
  ``dmas``, ``dma-names`` (now validated against ``enum: [tx, rx]``), ``pinctrl-0``,
  ``pinctrl-names``, ``mclk-enable``, ``mclk-divider``, ``synchronous``, and
  ``fifo-threshold``. (:github:`104423`)

* :dtcompatible:`st,hci-stm32wba` and :dtcompatible:`st,stm32wba-ieee802154` nodes
  (with nodelabels ``bt_hci_wba`` and ``ieee802154`` respectively) are now
  children of a top-level :dtcompatible:`st,stm32wba-radio` node with nodelabel
  ``radio``. The ``interrupts`` property is now set on the ``&radio`` node instead
  of being duplicated on both ``&bt_hci_wba`` and ``&ieee802154`` nodes. Out-of-tree
  boards which modified the ``interrupts`` property on either node must be updated
  to set the property on the top-level ``&radio`` node instead. (:github:`110546`)

* Renamed ST gpio-nexus for camera and display connectors as follow:
  ``st,dsi-lcd-qsh-030`` is renamed into :dtcompatible:`st,dsi-lcd-qsh-030-connector`
  ``st,stm32-dcmi-camera-fpu-330zh`` is renamed into :dtcompatible:`st,dvp-cam-zif-30-connector`

* :dtcompatible:`st,stm32-xspim` is now also used on STM32H5 and STM32H7RS series
  to declare and configure XSPI Manager. Boards making use of XSPI must now enable
  ``&xspim`` node in addition to the desired XSPI controller to use XSPI. (:github:`109903`)

* STM32MP13 SoC DTSI ethernet: rename labels from ``mac:`` and ``mdio:`` to ``mac0:`` and
  ``mdio0:``. The goal is to distinguish the 2 Ethernet controllers available. (:github:`108574`)

* Renamed Kconfig option ``CONFIG_STM32_MEMMAP`` to :kconfig:option:`CONFIG_FLASH_STM32_NOR_MEMMAP`.

* Using the ``stm32_lp_tick_source`` nodelabel to select an LPTIM as system timer is no longer supported
  and will trigger a build error. Use the :ref:`generic chosen <devicetree-zephyr-chosen-nodes>`
  ``zephyr,system-timer`` instead. (:github:`112999`)

Syscon
======

* The syscon API functions :c:func:`syscon_read_reg` and :c:func:`syscon_write_reg` now use
  ``uint32_t`` for the register offset parameter instead of ``uint16_t``. This allows for
  larger register offsets. Code that explicitly declares ``uint16_t`` variables for the
  register parameter or implements the syscon driver API functions may need to be updated.

Timer
=====

* :c:func:`sys_clock_set_timeout`, :c:func:`sys_clock_announce` and
  :c:func:`sys_clock_announce_locked` now take their tick count as an unsigned
  ``uint32_t`` rather than a signed ``int32_t``. Out-of-tree system timer drivers must
  update their :c:func:`sys_clock_set_timeout` definition accordingly, otherwise the build
  fails with a conflicting-types error. The kernel now also caps the requested timeout at
  ``SYS_CLOCK_MAX_WAIT`` and no longer passes ``K_TICKS_FOREVER`` to the driver, so such
  drivers no longer need to clamp the request against the :c:func:`sys_clock_announce`
  range or special-case ``K_TICKS_FOREVER``; only their own hardware cycle-count limits
  still need enforcing (:github:`111022`).

USB
===

* On STM32N6, the ``clocks`` cell which configures the USBPHYC clock mux has been moved
  from :samp:`usbotg_hs{N}` to :samp:`usbphyc{N}` nodes at SoC DTSI level. Boards which
  use an STM32N6 SoC with custom clock mux configuration must now set the ``clocks``
  property on :samp:`usbphyc{N}` instead of :samp:`usbotg_hs{N}`. (:github:`107813`)
* Indicating protocol error via ``errno`` in control transfer handlers is deprecated.
  Handlers should return error code directly. (:github:`108118`)
* When host issues control transfer with data stage from host to device, the USB control transfer
  callbacks ``control_to_dev`` in :c:struct:`usbd_class_api` and ``to_dev`` in
  :c:struct:`usbd_vreq_node` are now called with NULL ``buf`` before data stage is received.
  This allows the stack to return STALL during data stage. Out-of-tree class and vendor handlers
  need to be updated. (:github:`108840`)
* USB control transfer callbacks ``control_to_host`` in :c:struct:`usbd_class_api` and
  ``to_host`` in :c:struct:`usbd_vreq_node` are now expected to allocate the data stage buffer
  themselves. This allows allocating only as much memory as is actually needed which makes
  the worst case memory usage dependent on the handlers implementation and not on tainted wLength
  value coming from host. Out-of-tree class and vendor handlers need to be updated.
  (:github:`102491`)
* The Espressif USB-OTG full-speed controller compatible ``espressif,esp32-usb-otg`` has been
  renamed to :dtcompatible:`espressif,esp32-usb-otg-fs`. The internal PHY D+/D- pad numbers are
  now provided through the ``phy-dp-pin`` and ``phy-dm-pin`` properties. Out-of-tree devicetrees
  using the old compatible must update the node compatible and add the two pin properties.
* The ``clock-names`` property is now required on :dtcompatible:`st,stm32-usbphyc` nodes.
  A default value is provided at SoC DTSI level but *might* need to be overridden by board DTS.
  (:github:`112477`)

* The USB host controller API struct ``uhc_api`` got renamed to :c:struct:`uhc_driver_api`.
  It now also uses :c:macro:`DEVICE_API`. Out-of-tree USB host controller drivers must rename
  their API struct definitions and switch their API instances to ``DEVICE_API(uhc, ...)``.
  (:github:`108414`)

Video
=====

* The :dtcompatible:`ovti,ov7670` and :dtcompatible:`ovti,ov7675` camera drivers now assume a
  24 MHz XCLK input instead of the previous 6 MHz, matching the typical XCLK frequency listed in
  the OV7670 datasheet. Boards driving an OV7670 or OV7675 sensor must update their board-level
  XCLK clock configuration accordingly. For example, ``frdm_mcxn236`` has been switched from
  ``kFRO12M_to_CLKOUT`` (divided by 2 to yield 6 MHz) to ``kFRO_HF_to_CLKOUT`` (divided by 2 to
  yield 24 MHz), and ``frdm_mcxn947`` keeps ``kMAIN_CLK_to_CLKOUT`` but changes the CLKOUT
  divider from 25 to 6 to yield 24 MHz. (:github:`109393`)

* The APIs present in ``<zephyr/drivers/video.h>`` are now available under
  ``<zephyr/video/video.h>``. (:github:`112420`)

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
