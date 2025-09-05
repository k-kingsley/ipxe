/*
 * Copyright (C) 2024 iPXE Contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

FILE_LICENCE ( GPL2_OR_LATER_OR_UBDL );

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <ipxe/ipmi.h>
#include <ipxe/errfile.h>
#include <ipxe/in.h>
#include <ipxe/if_ether.h>

/** @file
 *
 * IPMI LAN Configuration
 *
 */



struct lan_param {
	uint8_t param;
	uint8_t len;
	const char *desc;
};

/** LAN configuration parameters */
static const struct lan_param lan_params[] __unused = {
	{ IPMI_LAN_SET_IN_PROGRESS,	1, "Set in Progress" },
	{ IPMI_LAN_AUTH_TYPE,		1, "Auth Type Support" },
	{ IPMI_LAN_AUTH_TYPE_ENABLE,	5, "Auth Type Enable" },
	{ IPMI_LAN_IP_ADDR,		4, "IP Address" },
	{ IPMI_LAN_IP_ADDR_SRC,		1, "IP Address Source" },
	{ IPMI_LAN_MAC_ADDR,		6, "MAC Address" },
	{ IPMI_LAN_SUBNET_MASK,		4, "Subnet Mask" },
	{ IPMI_LAN_IP_HEADER,		4, "IP Header" },
	{ IPMI_LAN_PRI_RMCP_PORT,	2, "Primary RMCP Port" },
	{ IPMI_LAN_SEC_RMCP_PORT,	2, "Secondary RMCP Port" },
	{ IPMI_LAN_BMC_ARP_CTRL,	1, "BMC ARP Control" },
	{ IPMI_LAN_GRAT_ARP_INTERVAL,	1, "Gratuitous ARP Intrvl" },
	{ IPMI_LAN_DEF_GATEWAY_IP,	4, "Default Gateway IP" },
	{ IPMI_LAN_DEF_GATEWAY_MAC,	6, "Default Gateway MAC" },
	{ IPMI_LAN_BAK_GATEWAY_IP,	4, "Backup Gateway IP" },
	{ IPMI_LAN_BAK_GATEWAY_MAC,	6, "Backup Gateway MAC" },
	{ IPMI_LAN_VLAN_ID,		3, "802.1q VLAN ID" },
	{ IPMI_LAN_VLAN_PRIORITY,	1, "802.1q VLAN Priority" },
	{ 0, 0, NULL }
};

/**
 * Get LAN configuration parameter
 *
 * @v channel	Channel number
 * @v param	Parameter selector
 * @v set	Parameter set
 * @v block	Parameter block
 * @v rsp	Response structure
 * @ret rc	Return status code
 */
static int get_lan_param(uint8_t channel, uint8_t param, uint8_t set, 
			uint8_t block, struct ipmi_rs *rsp) {
	struct ipmi_req req;
	
	memset(&req, 0, sizeof(req));
	req.netfn = IPMI_NETFN_TRANSPORT;
	req.cmd = IPMI_CMD_GET_LAN_CONFIG;
	req.data_len = 4;
	req.data[0] = channel;
	req.data[1] = param;
	req.data[2] = set;
	req.data[3] = block;
	
	return ipmi_send_request(&req, rsp);
}

/**
 * Print IP address
 *
 * @v data	IP address data
 */
static void print_ip_addr(const uint8_t *data) {
	printf("%d.%d.%d.%d", data[0], data[1], data[2], data[3]);
}

/**
 * Print MAC address
 *
 * @v data	MAC address data
 */
static void print_mac_addr(const uint8_t *data) {
	printf("%02x:%02x:%02x:%02x:%02x:%02x", 
		data[0], data[1], data[2], data[3], data[4], data[5]);
}

/**
 * Print authentication types
 *
 * @v auth_type	Authentication type mask
 */
static void print_auth_types(uint8_t auth_type) {
	int first = 1;
	
	if (auth_type & 0x01) {
		printf("%sNONE", first ? "" : " ");
		first = 0;
	}
	if (auth_type & 0x02) {
		printf("%sMD2", first ? "" : " ");
		first = 0;
	}
	if (auth_type & 0x04) {
		printf("%sMD5", first ? "" : " ");
		first = 0;
	}
	if (auth_type & 0x10) {
		printf("%sPASSWORD", first ? "" : " ");
		first = 0;
	}
	if (auth_type & 0x20) {
		printf("%sOEM", first ? "" : " ");
		first = 0;
	}
}

/**
 * Print IP address source
 *
 * @v source	IP address source
 */
static void print_ip_source(uint8_t source) {
	switch (source & 0x0F) {
	case 0x00:
		printf("Unspecified");
		break;
	case 0x01:
		printf("Static");
		break;
	case 0x02:
		printf("DHCP");
		break;
	case 0x03:
		printf("BIOS");
		break;
	default:
		printf("Unknown (0x%02x)", source);
		break;
	}
}

/**
 * Print set in progress status
 *
 * @v status	Set in progress status
 */
static void print_set_in_progress(uint8_t status) {
	switch (status & 0x03) {
	case 0x00:
		printf("Set Complete");
		break;
	case 0x01:
		printf("Set In Progress");
		break;
	case 0x02:
		printf("Commit Write");
		break;
	case 0x03:
		printf("Reserved");
		break;
	}
}

/**
 * Print VLAN ID
 *
 * @v data	VLAN data
 */
static void print_vlan_id(const uint8_t *data) {
	uint16_t vlan_id;
	
	if (!(data[2] & 0x80)) {
		printf("Disabled");
		return;
	}
	
	vlan_id = data[0] | ((data[1] & 0x0F) << 8);
	printf("%d", vlan_id);
}

/**
 * Find LAN channel
 *
 * @ret channel	LAN channel number or negative error
 */
static int find_lan_channel(void) {
	struct ipmi_req req;
	struct ipmi_rs rsp;
	int channel;
	
	/* Try channels 1-14 */
	for (channel = 1; channel <= 14; channel++) {
		memset(&req, 0, sizeof(req));
		req.netfn = IPMI_NETFN_APP;
		req.cmd = IPMI_CMD_GET_CHANNEL_INFO;
		req.data_len = 1;
		req.data[0] = channel;
		
		if (ipmi_send_request(&req, &rsp) == 0 && rsp.ccode == IPMI_CC_OK) {
			/* Check if this is a LAN channel (medium type 0x04) */
			if (rsp.data_len >= 2 && (rsp.data[1] & 0x7F) == 0x04) {
				return channel;
			}
		}
	}
	
	return -ENODEV;
}

/**
 * IPMI LAN print command
 *
 * @v argc	Argument count
 * @v argv	Argument list
 * @ret rc	Return status code
 */
int ipmi_lan_print(int argc, char **argv) {
	struct ipmi_rs rsp;
	const struct lan_param *param __unused;
	int channel = 0;
	int rc, i;
	
	/* Parse arguments */
	if (argc > 1) {
		channel = strtoul(argv[1], NULL, 0);
		if (channel < 1 || channel > 14) {
			printf("Invalid channel %d\n", channel);
			return -EINVAL;
		}
	} else {
		/* Find first LAN channel */
		channel = find_lan_channel();
		if (channel < 0) {
			printf("No LAN channel found\n");
			return channel;
		}
	}
	
	printf("Set in Progress         : ");
	if ((rc = get_lan_param(channel, IPMI_LAN_SET_IN_PROGRESS, 0, 0, &rsp)) == 0 
	    && rsp.ccode == IPMI_CC_OK && rsp.data_len >= 2) {
		print_set_in_progress(rsp.data[1]);
	} else {
		printf("Unknown");
	}
	printf("\n");
	
	printf("Auth Type Support      : ");
	if ((rc = get_lan_param(channel, IPMI_LAN_AUTH_TYPE, 0, 0, &rsp)) == 0
	    && rsp.ccode == IPMI_CC_OK && rsp.data_len >= 2) {
		print_auth_types(rsp.data[1]);
	} else {
		printf("Unknown");
	}
	printf("\n");
	
	/* Auth Type Enable for each privilege level */
	const char *priv_levels[] = {"Callback ", "User     ", "Operator ", "Admin    ", "OEM      "};
	for (i = 0; i < 5; i++) {
		printf("Auth Type Enable        : %s: ", priv_levels[i]);
		if ((rc = get_lan_param(channel, IPMI_LAN_AUTH_TYPE_ENABLE, 0, 0, &rsp)) == 0
		    && rsp.ccode == IPMI_CC_OK && rsp.data_len >= 6) {
			print_auth_types(rsp.data[1 + i]);
		} else {
			printf("Unknown");
		}
		printf("\n");
	}
	
	struct in_addr ip_addr = { 0 };
	printf("IP Address              : ");
	if ((rc = get_lan_param(channel, IPMI_LAN_IP_ADDR, 0, 0, &rsp)) == 0
	    && rsp.ccode == IPMI_CC_OK && rsp.data_len >= 5) {
		print_ip_addr(&rsp.data[1]);
		memcpy(&ip_addr, &rsp.data[1], sizeof(ip_addr));
	} else {
		printf("0.0.0.0");
	}
	printf("\n");
	
	printf("IP Address Source       : ");
	if ((rc = get_lan_param(channel, IPMI_LAN_IP_ADDR_SRC, 0, 0, &rsp)) == 0
	    && rsp.ccode == IPMI_CC_OK && rsp.data_len >= 2) {
		print_ip_source(rsp.data[1]);
	} else {
		printf("Unknown");
	}
	printf("\n");
	
	uint8_t mac_addr[6] = { 0 };
	printf("MAC Address             : ");
	if ((rc = get_lan_param(channel, IPMI_LAN_MAC_ADDR, 0, 0, &rsp)) == 0
	    && rsp.ccode == IPMI_CC_OK && rsp.data_len >= 7) {
		print_mac_addr(&rsp.data[1]);
		memcpy(mac_addr, &rsp.data[1], sizeof(mac_addr));
	} else {
		printf("00:00:00:00:00:00");
	}
	printf("\n");
	
	struct in_addr netmask = { 0 };
	printf("Subnet Mask             : ");
	if ((rc = get_lan_param(channel, IPMI_LAN_SUBNET_MASK, 0, 0, &rsp)) == 0
	    && rsp.ccode == IPMI_CC_OK && rsp.data_len >= 5) {
		print_ip_addr(&rsp.data[1]);
		memcpy(&netmask, &rsp.data[1], sizeof(netmask));
	} else {
		printf("0.0.0.0");
	}
	printf("\n");
	
	struct in_addr gateway = { 0 };
	printf("Default Gateway IP      : ");
	if ((rc = get_lan_param(channel, IPMI_LAN_DEF_GATEWAY_IP, 0, 0, &rsp)) == 0
	    && rsp.ccode == IPMI_CC_OK && rsp.data_len >= 5) {
		print_ip_addr(&rsp.data[1]);
		memcpy(&gateway, &rsp.data[1], sizeof(gateway));
	} else {
		printf("0.0.0.0");
	}
	printf("\n");
	
	printf("Default Gateway MAC     : ");
	if ((rc = get_lan_param(channel, IPMI_LAN_DEF_GATEWAY_MAC, 0, 0, &rsp)) == 0
	    && rsp.ccode == IPMI_CC_OK && rsp.data_len >= 7) {
		print_mac_addr(&rsp.data[1]);
	} else {
		printf("00:00:00:00:00:00");
	}
	printf("\n");
	
	printf("Backup Gateway IP       : ");
	if ((rc = get_lan_param(channel, IPMI_LAN_BAK_GATEWAY_IP, 0, 0, &rsp)) == 0
	    && rsp.ccode == IPMI_CC_OK && rsp.data_len >= 5) {
		print_ip_addr(&rsp.data[1]);
	} else {
		printf("0.0.0.0");
	}
	printf("\n");
	
	printf("Backup Gateway MAC      : ");
	if ((rc = get_lan_param(channel, IPMI_LAN_BAK_GATEWAY_MAC, 0, 0, &rsp)) == 0
	    && rsp.ccode == IPMI_CC_OK && rsp.data_len >= 7) {
		print_mac_addr(&rsp.data[1]);
	} else {
		printf("00:00:00:00:00:00");
	}
	printf("\n");
	
	printf("802.1q VLAN ID          : ");
	if ((rc = get_lan_param(channel, IPMI_LAN_VLAN_ID, 0, 0, &rsp)) == 0
	    && rsp.ccode == IPMI_CC_OK && rsp.data_len >= 4) {
		print_vlan_id(&rsp.data[1]);
	} else {
		printf("Disabled");
	}
	printf("\n");
	
	printf("802.1q VLAN Priority    : ");
	if ((rc = get_lan_param(channel, IPMI_LAN_VLAN_PRIORITY, 0, 0, &rsp)) == 0
	    && rsp.ccode == IPMI_CC_OK && rsp.data_len >= 2) {
		printf("%d", rsp.data[1] & 0x07);
	} else {
		printf("0");
	}
	printf("\n");
	
	/* Store settings in iPXE variables */
	if ( ip_addr.s_addr || mac_addr[0] || mac_addr[1] || mac_addr[2] ||
	     mac_addr[3] || mac_addr[4] || mac_addr[5] ||
	     netmask.s_addr || gateway.s_addr ) {
		ipmi_store_lan_settings ( ip_addr, mac_addr, netmask, gateway );
	}
	
	return 0;
}