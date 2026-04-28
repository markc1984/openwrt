#
# Copyright (C) 2026 Mark Comerford <markcomerford@gmail.com>
#
# This is free software, licensed under the Apache License, Version 2.0 .
#

include $(TOPDIR)/rules.mk

LUCI_TITLE:=LuCI support for SFP diagnostics
LUCI_DEPENDS:=+luci-base +rpcd-mod-ucode +ethtool-full
LUCI_PKGARCH:=all

PKG_LICENSE:=Apache-2.0
PKG_MAINTAINER:=Mark Comerford <markcomerford@gmail.com>

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
