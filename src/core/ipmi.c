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
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ipxe/io.h>
#include <ipxe/ipmi.h>
#include <ipxe/errfile.h>

/** @file
 *
 * Intelligent Platform Management Interface (IPMI)
 *
 */



static struct ipmi_intf *current_intf = NULL;

/** KCS base address (will be detected) */
unsigned int ipmi_kcs_base = 0;

/** Flag to indicate if KCS has been detected */
static int kcs_detected = 0;

/**
 * Wait for KCS interface ready
 *
 * @ret rc	Return status code
 */
static int kcs_wait_ibf_clear(void) {
	int timeout = 100000; /* 100ms timeout */
	uint8_t status;
	
	while (timeout-- > 0) {
		status = inb(IPMI_KCS_STATUS);
		if (!(status & IPMI_KCS_IBF))
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

/**
 * Wait for KCS output buffer full
 *
 * @ret rc	Return status code
 */
static int kcs_wait_obf_set(void) {
	int timeout = 100000; /* 100ms timeout */
	uint8_t status;
	
	while (timeout-- > 0) {
		status = inb(IPMI_KCS_STATUS);
		if (status & IPMI_KCS_OBF)
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

/**
 * Get KCS state
 *
 * @ret state	KCS state
 */
static uint8_t kcs_get_state(void) {
	return inb(IPMI_KCS_STATUS) & IPMI_KCS_STATE_MASK;
}

/**
 * Send command via KCS
 *
 * @v req	IPMI request
 * @v rsp	IPMI response
 * @ret rc	Return status code
 */
static int kcs_send_request(struct ipmi_req *req, struct ipmi_rs *rsp) {
	uint8_t data;
	int i, rc;
	uint8_t write_data[3 + 256]; /* NetFn/LUN + Cmd + data */
	int write_len = 0;

	/* Build write buffer */
	write_data[write_len++] = req->netfn << 2; /* NetFn/LUN */
	write_data[write_len++] = req->cmd;
	for (i = 0; i < req->data_len; i++) {
		write_data[write_len++] = req->data[i];
	}

	/* Clear any pending data */
	if (inb(IPMI_KCS_STATUS) & IPMI_KCS_OBF) {
		data = inb(IPMI_KCS_DATA_IN); /* Clear OBF */
	}

	/* Wait for interface to be ready */
	if ((rc = kcs_wait_ibf_clear()) != 0)
		return rc;

	/* Clear OBF before starting write (critical for proper state machine) */
	if (inb(IPMI_KCS_STATUS) & IPMI_KCS_OBF) {
		data = inb(IPMI_KCS_DATA_IN);
	}

	/* Start write sequence */
	outb(IPMI_KCS_WRITE_START, IPMI_KCS_CMD);
	
	if ((rc = kcs_wait_ibf_clear()) != 0)
		return rc;
	if (kcs_get_state() != IPMI_KCS_WRITE_STATE)
		return -EIO;

	/* Clear OBF again after WRITE_START */
	if (inb(IPMI_KCS_STATUS) & IPMI_KCS_OBF) {
		data = inb(IPMI_KCS_DATA_IN);
	}

	/* Write all but last byte */
	for (i = 0; i < write_len - 1; i++) {
		outb(write_data[i], IPMI_KCS_DATA_OUT);
		
		if ((rc = kcs_wait_ibf_clear()) != 0)
			return rc;
		if (kcs_get_state() != IPMI_KCS_WRITE_STATE)
			return -EIO;
			
		/* Clear OBF if set */
		if (inb(IPMI_KCS_STATUS) & IPMI_KCS_OBF) {
			data = inb(IPMI_KCS_DATA_IN);
		}
	}
	
	/* Send write end */
	outb(IPMI_KCS_WRITE_END, IPMI_KCS_CMD);
	
	if ((rc = kcs_wait_ibf_clear()) != 0)
		return rc;
	if (kcs_get_state() != IPMI_KCS_WRITE_STATE)
		return -EIO;
		
	/* Clear OBF if set */
	if (inb(IPMI_KCS_STATUS) & IPMI_KCS_OBF) {
		data = inb(IPMI_KCS_DATA_IN);
	}
	
	/* Write last byte */
	outb(write_data[write_len - 1], IPMI_KCS_DATA_OUT);

	/* Wait for read state */
	if ((rc = kcs_wait_ibf_clear()) != 0)
		return rc;
	if (kcs_get_state() != IPMI_KCS_READ_STATE)
		return -EIO;

	/* Read response bytes */
	rsp->data_len = 0;
	i = 0;
	while (kcs_get_state() == IPMI_KCS_READ_STATE) {
		if ((rc = kcs_wait_obf_set()) != 0)
			return rc;
		data = inb(IPMI_KCS_DATA_IN);
		
		/* First byte is NetFn/LUN */
		if (i == 0) {
			/* Skip NetFn/LUN */
		}
		/* Second byte is command */
		else if (i == 1) {
			/* Skip command echo */
		}
		/* Third byte is completion code */
		else if (i == 2) {
			rsp->ccode = data;
		}
		/* Rest are response data */
		else if (rsp->data_len < sizeof(rsp->data)) {
			rsp->data[rsp->data_len++] = data;
		}
		
		i++;
		
		/* Send READ command to get next byte (to DATA register like FreeIPMI) */
		outb(IPMI_KCS_READ, IPMI_KCS_DATA_OUT);
		if ((rc = kcs_wait_ibf_clear()) != 0)
			return rc;
	}
	
	/* We should be in IDLE state now */
	if (kcs_get_state() == IPMI_KCS_IDLE_STATE) {
		/* Read and discard the final dummy byte */
		if ((rc = kcs_wait_obf_set()) != 0)
			return rc;
		data = inb(IPMI_KCS_DATA_IN);
	}

	return 0;
}

/**
 * Test if KCS interface is available at given port
 *
 * @v port	KCS port to test
 * @ret rc	Return status code (0 = available)
 */
static int kcs_detect_port(unsigned int port) {
	uint8_t status;
	uint8_t data __unused;
	int i;
	
	ipmi_kcs_base = port;
	
	/* Try reading status register multiple times to detect if it's responsive */
	for (i = 0; i < 3; i++) {
		status = inb(IPMI_KCS_STATUS);
		
		/* 0xFF usually means no hardware present */
		if (status == 0xFF)
			return -ENODEV;
			
		/* 0x00 might be valid, but check if it changes */
		if (status != 0x00)
			break;
			
		/* Small delay between reads */
		udelay(10);
	}
	
	/* If always reads 0x00, probably no hardware */
	if (status == 0x00)
		return -ENODEV;
	
	/* If in READ state from previous command, clean it up */
	if ((status & IPMI_KCS_STATE_MASK) == IPMI_KCS_READ_STATE) {
		/* Send dummy READ commands until we reach IDLE state */
		int timeout = 100;
		while (timeout-- > 0 && (status & IPMI_KCS_STATE_MASK) == IPMI_KCS_READ_STATE) {
			/* Clear OBF if set */
			if (status & IPMI_KCS_OBF) {
				data = inb(IPMI_KCS_DATA_IN);
			}
			/* Send READ to get next byte */
			outb(IPMI_KCS_READ, IPMI_KCS_DATA_OUT);  /* Note: FreeIPMI sends READ to DATA register */
			udelay(100);
			status = inb(IPMI_KCS_STATUS);
		}
		/* Now check if we reached IDLE state */
		if ((status & IPMI_KCS_STATE_MASK) == IPMI_KCS_IDLE_STATE) {
			/* Final dummy read to clear last byte */
			if (status & IPMI_KCS_OBF) {
				data = inb(IPMI_KCS_DATA_IN);
			}
			return 0;
		}
	}
	
	/* Check for valid state bits */
	if ((status & IPMI_KCS_STATE_MASK) == IPMI_KCS_IDLE_STATE ||
	    (status & IPMI_KCS_STATE_MASK) == IPMI_KCS_WRITE_STATE) {
		/* Looks like valid KCS interface */
		return 0;
	}
	
	/* If in error state, try to clear it */
	if ((status & IPMI_KCS_STATE_MASK) == IPMI_KCS_ERROR_STATE) {
		outb(IPMI_KCS_ABORT, IPMI_KCS_CMD);
		udelay(100);
		status = inb(IPMI_KCS_STATUS);
		
		/* Check if error cleared */
		if ((status & IPMI_KCS_STATE_MASK) != IPMI_KCS_ERROR_STATE)
			return 0;
	}
		
	return -ENODEV;
}

/**
 * Test if KCS interface is available
 *
 * @ret rc	Return status code (0 = available)
 */
static int kcs_detect(void) {
	/* Common KCS I/O port addresses 
	 * 0xCA2 is the IPMI spec default (used by FreeIPMI)
	 * NOTE: Port 0x60 removed - it's the keyboard controller and 
	 * causes UEFI TPL violations when accessed incorrectly
	 */
	static const unsigned int kcs_ports[] = {
		0xCA2,	/* IPMI spec default (most common) */
		0xCA0,	/* Alternative */
		0xCA8,	/* Some Dell systems */
		0xE4,	/* Some older systems */
		0xCC0,	/* Some HP systems */
	};
	unsigned int i;
	
	/* If already detected, just return success without re-testing */
	if (kcs_detected && ipmi_kcs_base != 0) {
		return 0;
	}
	
	DBGC ( &kcs_ports[0], "IPMI: Probing for KCS interface...\n" );
	
	for (i = 0; i < (sizeof(kcs_ports) / sizeof(kcs_ports[0])); i++) {
		if (kcs_detect_port(kcs_ports[i]) == 0) {
			DBGC ( &kcs_ports[0], "IPMI: Found potential KCS interface at port 0x%X\n", kcs_ports[i] );
			
			/* Try to send a Get Device ID command to verify it's working */
			struct ipmi_req req;
			struct ipmi_rs rsp;
			int rc = -EIO;  /* Initialize to error in case something goes wrong */
			
			memset(&req, 0, sizeof(req));
			memset(&rsp, 0, sizeof(rsp));
			
			req.netfn = IPMI_NETFN_APP;
			req.cmd = IPMI_CMD_GET_DEVICE_ID;
			req.data_len = 0;
			
			rc = kcs_send_request(&req, &rsp);
			if (rc == 0 && rsp.ccode == 0) {
				DBGC ( &kcs_ports[0], "IPMI: KCS interface verified at port 0x%X\n", kcs_ports[i] );
				kcs_detected = 1;  /* Mark as detected */
				return 0;
			} else {
				DBGC ( &kcs_ports[0], "IPMI: KCS at port 0x%X not responding (rc=%d, ccode=0x%02x)\n", 
				       kcs_ports[i], rc, rsp.ccode );
			}
		}
	}
	
	DBGC ( &kcs_ports[0], "IPMI: No KCS interface found\n" );
	return -ENODEV;
}

/**
 * Open KCS interface
 *
 * @ret rc	Return status code
 */
static int kcs_open(void) {
	return kcs_detect();
}

/**
 * Close KCS interface
 */
static void kcs_close(void) {
	uint8_t status;
	uint8_t data __unused;
	int timeout;
	
	/* Get current status */
	status = inb(IPMI_KCS_STATUS);
	
	/* If not in idle state, try to abort current operation */
	if ((status & IPMI_KCS_STATE_MASK) != IPMI_KCS_IDLE_STATE) {
		/* Send abort command */
		outb(IPMI_KCS_ABORT, IPMI_KCS_CMD);
		
		/* Wait for IBF to clear */
		timeout = 10000;
		while (timeout-- > 0) {
			status = inb(IPMI_KCS_STATUS);
			if (!(status & IPMI_KCS_IBF))
				break;
			udelay(1);
		}
		
		/* Clear OBF if set */
		if (status & IPMI_KCS_OBF) {
			data = inb(IPMI_KCS_DATA_IN);
		}
		
		/* Read error status byte if in READ state */
		if ((status & IPMI_KCS_STATE_MASK) == IPMI_KCS_READ_STATE) {
			timeout = 10000;
			while (timeout-- > 0) {
				status = inb(IPMI_KCS_STATUS);
				if (status & IPMI_KCS_OBF) {
					data = inb(IPMI_KCS_DATA_IN);
					break;
				}
				udelay(1);
			}
		}
	}
	
	/* Final cleanup - clear any pending data */
	status = inb(IPMI_KCS_STATUS);
	if (status & IPMI_KCS_OBF) {
		data = inb(IPMI_KCS_DATA_IN);
	}
}

/** KCS interface */
static struct ipmi_intf kcs_intf = {
	.name = "KCS",
	.open = kcs_open,
	.close = kcs_close,
	.send_req = kcs_send_request,
};

/**
 * Initialize IPMI interface
 *
 * @ret rc	Return status code
 */
int ipmi_init(void) {
	int rc;

	/* Try KCS interface first */
	if ((rc = kcs_intf.open()) == 0) {
		current_intf = &kcs_intf;
		return 0;
	}

	return -ENODEV;
}

/**
 * Cleanup IPMI interface
 */
void ipmi_cleanup(void) {
	if (current_intf && current_intf->close)
		current_intf->close();
	current_intf = NULL;
	/* Note: We intentionally keep kcs_detected and ipmi_kcs_base 
	 * so we can reuse them on the next call */
}

/**
 * Send IPMI request
 *
 * @v req	IPMI request
 * @v rsp	IPMI response  
 * @ret rc	Return status code
 */
int ipmi_send_request(struct ipmi_req *req, struct ipmi_rs *rsp) {
	if (!current_intf)
		return -ENODEV;
		
	memset(rsp, 0, sizeof(*rsp));
	return current_intf->send_req(req, rsp);
}