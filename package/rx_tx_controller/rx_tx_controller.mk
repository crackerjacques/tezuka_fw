################################################################################
#
# rx_tx_controller
#
################################################################################

RX_TX_CONTROLLER_VERSION = 1.0
RX_TX_CONTROLLER_SITE = $(BR2_EXTERNAL_PLUTOSDR_PATH)/package/rx_tx_controller/src
RX_TX_CONTROLLER_SITE_METHOD = local
RX_TX_CONTROLLER_LICENSE = proprietary

define RX_TX_CONTROLLER_BUILD_CMDS
    $(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
        $(@D)/rx_tx_controller.c -o $(@D)/rx_tx_controller
endef

define RX_TX_CONTROLLER_INSTALL_TARGET_CMDS
    $(INSTALL) -D -m 0755 $(@D)/rx_tx_controller $(TARGET_DIR)/usr/sbin/rx_tx_controller
    $(INSTALL) -D -m 0755 $(@D)/switch_rfinput.sh $(TARGET_DIR)/usr/bin/switch_rfinput.sh
    $(INSTALL) -D -m 0755 $(@D)/switch_rfoutput.sh $(TARGET_DIR)/usr/bin/switch_rfoutput.sh
endef

define RX_TX_CONTROLLER_INSTALL_INIT_SYSV
    $(INSTALL) -D -m 0755 $(@D)/S97rx_tx_controller \
        $(TARGET_DIR)/etc/init.d/S97rx_tx_controller
endef

$(eval $(generic-package))