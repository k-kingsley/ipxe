#ifndef _IPXE_IPMI_H
#define _IPXE_IPMI_H

/** @file
 *
 * Intelligent Platform Management Interface (IPMI)
 *
 */

FILE_LICENCE ( GPL2_OR_LATER_OR_UBDL );

#include <stdint.h>
#include <ipxe/io.h>
#include <ipxe/acpi.h>

/** IPMI NetFn values */
#define IPMI_NETFN_APP			0x06
#define IPMI_NETFN_TRANSPORT		0x0C
#define IPMI_NETFN_DCGRP		0x2C

/** IPMI Commands */
#define IPMI_CMD_GET_DEVICE_ID		0x01
#define IPMI_CMD_GET_LAN_CONFIG		0x02
#define IPMI_CMD_GET_CHANNEL_INFO	0x42

/** DCMI Commands */
#define IPMI_DCMI_GET_ASSET_TAG		0x06

/** IPMI completion codes */
#define IPMI_CC_OK			0x00
#define IPMI_CC_NODE_BUSY		0xC0
#define IPMI_CC_INVALID_CMD		0xC1
#define IPMI_CC_TIMEOUT			0xC3

/** SPMI (Service Processor Management Interface) table signature */
#define SPMI_SIGNATURE ACPI_SIGNATURE ( 'S', 'P', 'M', 'I' )

/** ACPI SPMI table */
struct acpi_spmi {
	/** ACPI header */
	struct acpi_header acpi;
	/** Interface type */
	uint8_t interface_type;
	/** Reserved */
	uint8_t reserved1;
	/** Specification revision */
	uint16_t spec_revision;
	/** Interrupt type */
	uint8_t interrupt_type;
	/** GPE number */
	uint8_t gpe;
	/** Reserved */
	uint8_t reserved2;
	/** PCI device flag */
	uint8_t pci_device_flag;
	/** Global system interrupt */
	uint32_t global_system_interrupt;
	/** Base address */
	struct {
		uint8_t space_id;
		uint8_t register_bit_width;
		uint8_t register_bit_offset;
		uint8_t reserved;
		uint64_t address;
	} __attribute__ (( packed )) base_address;
	/** PCI segment/bus/device/function */
	union {
		struct {
			uint8_t segment;
			uint8_t bus;
			uint8_t device;
			uint8_t function;
		} __attribute__ (( packed ));
		uint32_t pci_bdf;
	};
	/** Reserved */
	uint8_t reserved3;
} __attribute__ (( packed ));

/** SPMI interface types */
#define SPMI_INTERFACE_KCS		0x01
#define SPMI_INTERFACE_SMIC		0x02  
#define SPMI_INTERFACE_BT		0x03
#define SPMI_INTERFACE_SSIF		0x04

/** KCS Interface registers - will be set dynamically */
extern unsigned int ipmi_kcs_base;
#define IPMI_KCS_DATA_IN		(ipmi_kcs_base + 0)
#define IPMI_KCS_DATA_OUT		(ipmi_kcs_base + 0)
#define IPMI_KCS_CMD			(ipmi_kcs_base + 1)
#define IPMI_KCS_STATUS			(ipmi_kcs_base + 1)

/** KCS Status bits */
#define IPMI_KCS_OBF			0x01
#define IPMI_KCS_IBF			0x02
#define IPMI_KCS_STATE_MASK		0xC0
#define IPMI_KCS_IDLE_STATE		0x00
#define IPMI_KCS_READ_STATE		0x40
#define IPMI_KCS_WRITE_STATE		0x80
#define IPMI_KCS_ERROR_STATE		0xC0

/** KCS Commands */
#define IPMI_KCS_WRITE_START		0x61
#define IPMI_KCS_WRITE_END		0x62
#define IPMI_KCS_GET_STATUS		0x60
#define IPMI_KCS_ABORT			0x60
#define IPMI_KCS_READ			0x68

/** IPMI LAN Parameters */
#define IPMI_LAN_SET_IN_PROGRESS	0
#define IPMI_LAN_AUTH_TYPE		1
#define IPMI_LAN_AUTH_TYPE_ENABLE	2
#define IPMI_LAN_IP_ADDR		3
#define IPMI_LAN_IP_ADDR_SRC		4
#define IPMI_LAN_MAC_ADDR		5
#define IPMI_LAN_SUBNET_MASK		6
#define IPMI_LAN_IP_HEADER		7
#define IPMI_LAN_PRI_RMCP_PORT		8
#define IPMI_LAN_SEC_RMCP_PORT		9
#define IPMI_LAN_BMC_ARP_CTRL		10
#define IPMI_LAN_GRAT_ARP_INTERVAL	11
#define IPMI_LAN_DEF_GATEWAY_IP		12
#define IPMI_LAN_DEF_GATEWAY_MAC	13
#define IPMI_LAN_BAK_GATEWAY_IP		14
#define IPMI_LAN_BAK_GATEWAY_MAC	15
#define IPMI_LAN_VLAN_ID		20
#define IPMI_LAN_VLAN_PRIORITY		21

/** IPMI request structure */
struct ipmi_req {
	uint8_t netfn;
	uint8_t cmd;
	uint8_t data_len;
	uint8_t data[256];
};

/** IPMI response structure */
struct ipmi_rs {
	uint8_t ccode;
	uint16_t data_len;
	uint8_t data[256];
};

/** IPMI interface structure */
struct ipmi_intf {
	const char *name;
	int (*open)(void);
	void (*close)(void);
	int (*send_req)(struct ipmi_req *req, struct ipmi_rs *rsp);
};

/* Function declarations */

/**
 * Detect IPMI hardware interface
 *
 * @ret rc		Return status code
 */
extern int ipmi_detect_interface(void);

/**
 * Initialize IPMI interface
 *
 * @ret rc		Return status code
 */
extern int ipmi_init(void);

/**
 * Cleanup IPMI interface
 *
 */
extern void ipmi_cleanup(void);

/**
 * Send IPMI request
 *
 * @v req		IPMI request
 * @v rsp		IPMI response
 * @ret rc		Return status code
 */
extern int ipmi_send_request(struct ipmi_req *req, struct ipmi_rs *rsp);

/**
 * Print IPMI LAN configuration
 *
 * @v argc		Argument count
 * @v argv		Argument list
 * @ret rc		Return status code
 */
extern int ipmi_lan_print(int argc, char **argv);

/**
 * Get IPMI DCMI asset tag
 *
 * @v argc		Argument count
 * @v argv		Argument list
 * @ret rc		Return status code
 */
extern int ipmi_dcmi_asset_tag(int argc, char **argv);

/* Include necessary headers for settings */
#include <ipxe/in.h>

/* IPMI settings functions */

/**
 * Store IPMI LAN settings
 *
 * @v ip_addr		IP address
 * @v mac_addr		MAC address
 * @v netmask		Subnet mask  
 * @v gateway		Default gateway
 * @ret rc		Return status code
 */
extern int ipmi_store_lan_settings ( struct in_addr ip_addr, uint8_t *mac_addr,
				      struct in_addr netmask, struct in_addr gateway );

/**
 * Store IPMI asset tag setting
 *
 * @v tag		Asset tag string
 * @ret rc		Return status code
 */
extern int ipmi_store_tag_setting ( const char *tag );

#endif /* _IPXE_IPMI_H */