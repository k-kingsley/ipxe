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
#include <ipxe/acpi.h>
#include <ipxe/init.h>

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

/** Standard IPMI KCS I/O port addresses to try */
static unsigned int ipmi_standard_ports[] = {
	0xCA0, 0xCA4, 0xCA8, 0xCAC,  /* Standard KCS ports */
	0x0,  /* Terminator */
};

/**
 * Detect IPMI interface via ACPI SPMI table
 *
 * @ret rc	Return status code
 */
static int ipmi_acpi_detect(void) {
	const struct acpi_spmi *spmi;
	
	DBGC ( &ipmi_kcs_base, "IPMI: Searching for ACPI SPMI table...\n" );
	
	/* Look for SPMI table */
	spmi = ( const struct acpi_spmi * ) acpi_table ( SPMI_SIGNATURE, 0 );
	if ( ! spmi ) {
		DBGC ( &ipmi_kcs_base, "IPMI: No SPMI table found in ACPI\n" );
		return -ENODEV;
	}
	
	DBGC ( &ipmi_kcs_base, "IPMI: Found SPMI table at %p\n", spmi );
	DBGC ( &ipmi_kcs_base, "IPMI: SPMI interface_type=%d, spec_revision=0x%04x\n",
	       spmi->interface_type, spmi->spec_revision );
	DBGC ( &ipmi_kcs_base, "IPMI: SPMI base_address.space_id=%d, address=0x%08llx\n",
	       spmi->base_address.space_id, 
	       ( unsigned long long ) spmi->base_address.address );
	
	/* Only support KCS interface for now */
	if ( spmi->interface_type != SPMI_INTERFACE_KCS ) {
		DBGC ( &ipmi_kcs_base, "IPMI: Unsupported interface type %d (only KCS supported)\n",
		       spmi->interface_type );
		return -ENOTSUP;
	}
	
	/* Only support I/O space addresses */
	if ( spmi->base_address.space_id != 1 ) {  /* System I/O */
		DBGC ( &ipmi_kcs_base, "IPMI: Unsupported address space %d (only I/O space supported)\n",
		       spmi->base_address.space_id );
		return -ENOTSUP;
	}
	
	/* Check for reasonable I/O port address */
	if ( spmi->base_address.address > 0xFFFF ) {
		DBGC ( &ipmi_kcs_base, "IPMI: Invalid I/O port address 0x%08llx\n",
		       ( unsigned long long ) spmi->base_address.address );
		return -ENODEV;
	}
	
	ipmi_kcs_base = spmi->base_address.address & 0xFFFF;
	DBGC ( &ipmi_kcs_base, "IPMI: Successfully detected KCS interface at I/O port 0x%04x via ACPI SPMI\n",
	       ipmi_kcs_base );
	
	return 0;
}

/**
 * Detect IPMI KCS interface by probing standard I/O ports
 *
 * @ret rc	Return status code
 */
static int ipmi_detect_kcs(void) {
	unsigned int *port;
	uint8_t status;
	
	DBGC ( &ipmi_kcs_base, "IPMI: Probing standard KCS I/O ports for interface\n" );
	
	for ( port = ipmi_standard_ports; *port; port++ ) {
		DBGC ( &ipmi_kcs_base, "IPMI: Probing I/O port 0x%04x...\n", *port );
		
		/* Try to read status register */
		status = inb ( *port + 1 );
		DBGC ( &ipmi_kcs_base, "IPMI: Port 0x%04x status register = 0x%02x\n",
		       *port, status );
		
		/* Check if it looks like a real interface */
		if ( ( status & 0xFF ) != 0xFF && ( status & 0x00 ) != 0x00 ) {
			/* Basic sanity check - status should have some valid bits */
			DBGC ( &ipmi_kcs_base, "IPMI: Port 0x%04x appears to have valid hardware (status=0x%02x)\n",
			       *port, status );
			ipmi_kcs_base = *port;
			DBGC ( &ipmi_kcs_base, "IPMI: Successfully detected KCS interface at I/O port 0x%04x via probing\n",
			       ipmi_kcs_base );
			return 0;
		} else {
			DBGC ( &ipmi_kcs_base, "IPMI: Port 0x%04x shows no hardware (status=0x%02x)\n",
			       *port, status );
		}
	}
	
	DBGC ( &ipmi_kcs_base, "IPMI: No KCS interface found after probing all standard ports\n" );
	return -ENODEV;
}

/**
 * Detect IPMI hardware interface
 *
 * @ret rc	Return status code
 */
int ipmi_detect_interface(void) {
	int rc;
	
	DBGC ( &ipmi_kcs_base, "IPMI: Starting hardware interface detection\n" );
	
	/* Reset detection state */
	ipmi_kcs_base = 0;
	kcs_detected = 0;
	
	/* Try ACPI detection first */
	DBGC ( &ipmi_kcs_base, "IPMI: Attempting ACPI-based detection...\n" );
	rc = ipmi_acpi_detect();
	if ( rc == 0 ) {
		kcs_detected = 1;
		DBGC ( &ipmi_kcs_base, "IPMI: Hardware detection completed successfully via ACPI\n" );
		return 0;
	}
	DBGC ( &ipmi_kcs_base, "IPMI: ACPI detection failed: %s\n", strerror ( rc ) );
	
	/* Fall back to standard port detection */
	DBGC ( &ipmi_kcs_base, "IPMI: Attempting fallback port probing...\n" );
	rc = ipmi_detect_kcs();
	if ( rc == 0 ) {
		kcs_detected = 1;
		DBGC ( &ipmi_kcs_base, "IPMI: Hardware detection completed successfully via port probing\n" );
		return 0;
	}
	DBGC ( &ipmi_kcs_base, "IPMI: Port probing failed: %s\n", strerror ( rc ) );
	
	/* Ensure clean state on detection failure */
	ipmi_kcs_base = 0;
	kcs_detected = 0;
	
	DBGC ( &ipmi_kcs_base, "IPMI: No interface detected after trying all methods\n" );
	return -ENODEV;
}

/**
 * Wait for KCS interface ready
 *
 * @ret rc	Return status code
 */
static int kcs_wait_ibf_clear(void) {
	int timeout = 100000; /* 100ms timeout */
	uint8_t status;
	int initial_timeout = timeout;
	
	while (timeout-- > 0) {
		status = inb(IPMI_KCS_STATUS);
		if (!(status & IPMI_KCS_IBF)) {
			if ( ( initial_timeout - timeout ) > 1000 ) { /* Log if took >1ms */
				DBGC ( &ipmi_kcs_base, "IPMI: IBF cleared after %d μs (status=0x%02x)\n",
				       initial_timeout - timeout, status );
			}
			return 0;
		}
		udelay(1);
	}
	
	DBGC ( &ipmi_kcs_base, "IPMI: Timeout waiting for IBF clear (final status=0x%02x)\n", status );
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
	int initial_timeout = timeout;
	
	while (timeout-- > 0) {
		status = inb(IPMI_KCS_STATUS);
		if (status & IPMI_KCS_OBF) {
			if ( ( initial_timeout - timeout ) > 1000 ) { /* Log if took >1ms */
				DBGC ( &ipmi_kcs_base, "IPMI: OBF set after %d μs (status=0x%02x)\n",
				       initial_timeout - timeout, status );
			}
			return 0;
		}
		udelay(1);
	}
	
	DBGC ( &ipmi_kcs_base, "IPMI: Timeout waiting for OBF set (final status=0x%02x)\n", status );
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
 * Reset KCS interface to IDLE state
 *
 * @ret rc	Return status code
 */
static int kcs_reset_to_idle(void) {
	uint8_t status, data;
	int timeout = 1000; /* 1ms timeout for reset attempts */
	int attempts = 0;
	const int max_attempts = 3;
	
	DBGC ( &ipmi_kcs_base, "IPMI: Attempting to reset KCS interface to IDLE state\n" );
	
	while ( attempts < max_attempts ) {
		status = inb ( IPMI_KCS_STATUS );
		DBGC ( &ipmi_kcs_base, "IPMI: KCS reset attempt %d, current state=0x%02x\n",
		       attempts + 1, status & IPMI_KCS_STATE_MASK );
		
		/* If already IDLE, we're done */
		if ( ( status & IPMI_KCS_STATE_MASK ) == IPMI_KCS_IDLE_STATE ) {
			/* Clear any pending output data */
			if ( status & IPMI_KCS_OBF ) {
				data = inb ( IPMI_KCS_DATA_IN );
				DBGC ( &ipmi_kcs_base, "IPMI: Cleared pending data 0x%02x\n", data );
			}
			DBGC ( &ipmi_kcs_base, "IPMI: KCS interface reset to IDLE successfully\n" );
			return 0;
		}
		
		/* Try to abort current operation */
		outb ( IPMI_KCS_ABORT, IPMI_KCS_CMD );
		
		/* Wait for abort to take effect */
		timeout = 1000;
		while ( timeout-- > 0 ) {
			status = inb ( IPMI_KCS_STATUS );
			if ( ! ( status & IPMI_KCS_IBF ) )
				break;
			udelay ( 1 );
		}
		
		/* Check if we're in IDLE or ERROR state after abort */
		status = inb ( IPMI_KCS_STATUS );
		if ( ( status & IPMI_KCS_STATE_MASK ) == IPMI_KCS_IDLE_STATE ) {
			DBGC ( &ipmi_kcs_base, "IPMI: Abort successful, reached IDLE state\n" );
			return 0;
		}
		
		/* If in ERROR state, try to clear it by reading status */
		if ( ( status & IPMI_KCS_STATE_MASK ) == IPMI_KCS_ERROR_STATE ) {
			DBGC ( &ipmi_kcs_base, "IPMI: In ERROR state, attempting to clear\n" );
			/* Read any pending error data */
			if ( status & IPMI_KCS_OBF ) {
				data = inb ( IPMI_KCS_DATA_IN );
				DBGC ( &ipmi_kcs_base, "IPMI: Read error data 0x%02x\n", data );
			}
		}
		
		attempts++;
		if ( attempts < max_attempts ) {
			DBGC ( &ipmi_kcs_base, "IPMI: Reset attempt %d failed, retrying...\n", attempts );
			udelay ( 1000 ); /* 1ms delay before retry */
		}
	}
	
	DBGC ( &ipmi_kcs_base, "IPMI: Failed to reset KCS interface after %d attempts\n", max_attempts );
	return -EIO;
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
	uint8_t initial_status;

	DBGC ( &ipmi_kcs_base, "IPMI: Sending KCS request NetFn=0x%02x Cmd=0x%02x DataLen=%d\n",
	       req->netfn, req->cmd, req->data_len );

	/* Check initial KCS state */
	initial_status = inb(IPMI_KCS_STATUS);
	DBGC ( &ipmi_kcs_base, "IPMI: Initial KCS status=0x%02x state=%s\n", initial_status,
	       (initial_status & IPMI_KCS_STATE_MASK) == IPMI_KCS_IDLE_STATE ? "IDLE" :
	       (initial_status & IPMI_KCS_STATE_MASK) == IPMI_KCS_READ_STATE ? "READ" :
	       (initial_status & IPMI_KCS_STATE_MASK) == IPMI_KCS_WRITE_STATE ? "WRITE" :
	       (initial_status & IPMI_KCS_STATE_MASK) == IPMI_KCS_ERROR_STATE ? "ERROR" : "UNKNOWN" );

	/* Build write buffer */
	write_data[write_len++] = req->netfn << 2; /* NetFn/LUN */
	write_data[write_len++] = req->cmd;
	for (i = 0; i < req->data_len; i++) {
		write_data[write_len++] = req->data[i];
	}
	
	DBGC ( &ipmi_kcs_base, "IPMI: Built %d-byte write buffer: NetFn/LUN=0x%02x Cmd=0x%02x\n",
	       write_len, write_data[0], write_data[1] );

	/* Clear any pending data */
	if (inb(IPMI_KCS_STATUS) & IPMI_KCS_OBF) {
		data = inb(IPMI_KCS_DATA_IN); /* Clear OBF */
		DBGC ( &ipmi_kcs_base, "IPMI: Cleared pending OBF data=0x%02x\n", data );
	}

	/* Wait for interface to be ready */
	DBGC ( &ipmi_kcs_base, "IPMI: Waiting for interface ready...\n" );
	if ((rc = kcs_wait_ibf_clear()) != 0) {
		DBGC ( &ipmi_kcs_base, "IPMI: Interface not ready: %s\n", strerror ( rc ) );
		return rc;
	}

	/* Clear OBF before starting write (critical for proper state machine) */
	if (inb(IPMI_KCS_STATUS) & IPMI_KCS_OBF) {
		data = inb(IPMI_KCS_DATA_IN);
		DBGC ( &ipmi_kcs_base, "IPMI: Cleared OBF before write start: data=0x%02x\n", data );
	}

	/* Start write sequence */
	DBGC ( &ipmi_kcs_base, "IPMI: Starting write sequence (WRITE_START)\n" );
	outb(IPMI_KCS_WRITE_START, IPMI_KCS_CMD);
	
	if ((rc = kcs_wait_ibf_clear()) != 0) {
		DBGC ( &ipmi_kcs_base, "IPMI: WRITE_START failed: %s\n", strerror ( rc ) );
		return rc;
	}
	if (kcs_get_state() != IPMI_KCS_WRITE_STATE) {
		DBGC ( &ipmi_kcs_base, "IPMI: Expected WRITE state after WRITE_START, got state=0x%02x\n",
		       kcs_get_state() );
		DBGC ( &ipmi_kcs_base, "IPMI: Attempting state recovery...\n" );
		if ( kcs_reset_to_idle() != 0 ) {
			DBGC ( &ipmi_kcs_base, "IPMI: State recovery failed\n" );
			return -EIO;
		}
		DBGC ( &ipmi_kcs_base, "IPMI: State recovery successful, aborting current request\n" );
		return -EIO; /* Still fail the current request, but interface is recovered */
	}
	DBGC ( &ipmi_kcs_base, "IPMI: Successfully entered WRITE state\n" );

	/* Clear OBF again after WRITE_START */
	if (inb(IPMI_KCS_STATUS) & IPMI_KCS_OBF) {
		data = inb(IPMI_KCS_DATA_IN);
	}

	/* Write all but last byte */
	for (i = 0; i < write_len - 1; i++) {
		outb(write_data[i], IPMI_KCS_DATA_OUT);
		
		if ((rc = kcs_wait_ibf_clear()) != 0)
			return rc;
		if (kcs_get_state() != IPMI_KCS_WRITE_STATE) {
			DBGC ( &ipmi_kcs_base, "IPMI: Unexpected state during data write, attempting recovery\n" );
			kcs_reset_to_idle(); /* Try to recover for next request */
			return -EIO;
		}
			
		/* Clear OBF if set */
		if (inb(IPMI_KCS_STATUS) & IPMI_KCS_OBF) {
			data = inb(IPMI_KCS_DATA_IN);
		}
	}
	
	/* Send write end */
	outb(IPMI_KCS_WRITE_END, IPMI_KCS_CMD);
	
	if ((rc = kcs_wait_ibf_clear()) != 0)
		return rc;
	if (kcs_get_state() != IPMI_KCS_WRITE_STATE) {
		DBGC ( &ipmi_kcs_base, "IPMI: Unexpected state after WRITE_END, attempting recovery\n" );
		kcs_reset_to_idle(); /* Try to recover for next request */
		return -EIO;
	}
		
	/* Clear OBF if set */
	if (inb(IPMI_KCS_STATUS) & IPMI_KCS_OBF) {
		data = inb(IPMI_KCS_DATA_IN);
	}
	
	/* Write last byte */
	outb(write_data[write_len - 1], IPMI_KCS_DATA_OUT);

	/* Wait for read state */
	DBGC ( &ipmi_kcs_base, "IPMI: Waiting for READ state after sending last byte...\n" );
	if ((rc = kcs_wait_ibf_clear()) != 0) {
		DBGC ( &ipmi_kcs_base, "IPMI: Failed waiting for READ state: %s\n", strerror ( rc ) );
		return rc;
	}
	if (kcs_get_state() != IPMI_KCS_READ_STATE) {
		DBGC ( &ipmi_kcs_base, "IPMI: Expected READ state, got state=0x%02x\n", kcs_get_state() );
		DBGC ( &ipmi_kcs_base, "IPMI: Attempting recovery before failing request\n" );
		kcs_reset_to_idle(); /* Try to recover for next request */
		return -EIO;
	}
	DBGC ( &ipmi_kcs_base, "IPMI: Successfully entered READ state, starting response read\n" );

	/* Read response bytes */
	rsp->data_len = 0;
	i = 0;
	while (kcs_get_state() == IPMI_KCS_READ_STATE) {
		if ((rc = kcs_wait_obf_set()) != 0) {
			DBGC ( &ipmi_kcs_base, "IPMI: Timeout waiting for response byte %d: %s\n",
			       i, strerror ( rc ) );
			return rc;
		}
		data = inb(IPMI_KCS_DATA_IN);
		
		/* First byte is NetFn/LUN */
		if (i == 0) {
			DBGC ( &ipmi_kcs_base, "IPMI: Response NetFn/LUN=0x%02x\n", data );
		}
		/* Second byte is command */
		else if (i == 1) {
			DBGC ( &ipmi_kcs_base, "IPMI: Response command echo=0x%02x\n", data );
		}
		/* Third byte is completion code */
		else if (i == 2) {
			rsp->ccode = data;
			DBGC ( &ipmi_kcs_base, "IPMI: Response completion code=0x%02x\n", data );
		}
		/* Rest are response data */
		else if (rsp->data_len < sizeof(rsp->data)) {
			rsp->data[rsp->data_len++] = data;
			DBGC ( &ipmi_kcs_base, "IPMI: Response data[%d]=0x%02x\n", rsp->data_len - 1, data );
		}
		
		i++;
		
		/* Send READ command to get next byte (to DATA register like FreeIPMI) */
		outb(IPMI_KCS_READ, IPMI_KCS_DATA_OUT);
		if ((rc = kcs_wait_ibf_clear()) != 0)
			return rc;
	}
	
	DBGC ( &ipmi_kcs_base, "IPMI: Finished reading response, checking final state\n" );
	
	/* We should be in IDLE state now */
	if (kcs_get_state() == IPMI_KCS_IDLE_STATE) {
		DBGC ( &ipmi_kcs_base, "IPMI: KCS returned to IDLE state, reading final dummy byte\n" );
		/* Read and discard the final dummy byte */
		if ((rc = kcs_wait_obf_set()) != 0) {
			DBGC ( &ipmi_kcs_base, "IPMI: Timeout waiting for final dummy byte: %s\n",
			       strerror ( rc ) );
			return rc;
		}
		data = inb(IPMI_KCS_DATA_IN);
		DBGC ( &ipmi_kcs_base, "IPMI: Final dummy byte=0x%02x\n", data );
	} else {
		DBGC ( &ipmi_kcs_base, "IPMI: Warning: KCS not in IDLE state (state=0x%02x)\n",
		       kcs_get_state() );
	}

	DBGC ( &ipmi_kcs_base, "IPMI: KCS request completed successfully, ccode=0x%02x, %d data bytes\n",
	       rsp->ccode, rsp->data_len );
	return 0;
}

/**
 * Open KCS interface
 *
 * @ret rc	Return status code
 */
static int kcs_open(void) {
	/* Check if interface has been detected */
	if ( ! kcs_detected ) {
		return -ENODEV;
	}
	
	/* Ensure we have a valid base address */
	if ( ipmi_kcs_base == 0 ) {
		return -ENODEV;
	}
	
	return 0;
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

	DBGC ( &ipmi_kcs_base, "IPMI: Starting IPMI interface initialization\n" );

	/* Detect hardware interface if not already done */
	if ( ! kcs_detected ) {
		DBGC ( &ipmi_kcs_base, "IPMI: Hardware not detected, running detection...\n" );
		rc = ipmi_detect_interface();
		if ( rc != 0 ) {
			DBGC ( &ipmi_kcs_base, "IPMI: Hardware detection failed: %s\n", strerror ( rc ) );
			return rc;
		}
		DBGC ( &ipmi_kcs_base, "IPMI: Hardware detection completed successfully\n" );
	} else {
		DBGC ( &ipmi_kcs_base, "IPMI: Using previously detected hardware at 0x%04x\n", ipmi_kcs_base );
	}

	/* Try KCS interface first */
	DBGC ( &ipmi_kcs_base, "IPMI: Opening KCS interface...\n" );
	if ((rc = kcs_intf.open()) == 0) {
		current_intf = &kcs_intf;
		DBGC ( &ipmi_kcs_base, "IPMI: KCS interface opened successfully\n" );
		DBGC ( &ipmi_kcs_base, "IPMI: Initialization completed, interface ready for use\n" );
		return 0;
	}

	/* Cleanup on initialization failure */
	DBGC ( &ipmi_kcs_base, "IPMI: KCS interface open failed: %s\n", strerror ( rc ) );
	DBGC ( &ipmi_kcs_base, "IPMI: Performing cleanup after initialization failure\n" );
	
	/* Reset interface state */
	current_intf = NULL;
	
	/* Try to reset KCS to a known state for future attempts */
	if ( ipmi_kcs_base != 0 ) {
		DBGC ( &ipmi_kcs_base, "IPMI: Attempting to reset interface to clean state\n" );
		kcs_reset_to_idle(); /* Best effort - ignore errors */
	}
	
	DBGC ( &ipmi_kcs_base, "IPMI: Initialization failed, cleanup completed\n" );
	return -ENODEV;
}

/**
 * Cleanup IPMI interface
 */
void ipmi_cleanup(void) {
	DBGC ( &ipmi_kcs_base, "IPMI: Starting interface cleanup\n" );
	
	/* Close current interface if open */
	if (current_intf && current_intf->close) {
		DBGC ( &ipmi_kcs_base, "IPMI: Closing %s interface\n", current_intf->name );
		current_intf->close();
	}
	
	/* Reset interface pointer */
	current_intf = NULL;
	
	/* Try to ensure hardware is in a clean state for next use */
	if ( ipmi_kcs_base != 0 ) {
		DBGC ( &ipmi_kcs_base, "IPMI: Resetting hardware to clean state\n" );
		kcs_reset_to_idle(); /* Best effort - ignore errors */
	}
	
	DBGC ( &ipmi_kcs_base, "IPMI: Cleanup completed\n" );
	/* Note: We intentionally keep kcs_detected and ipmi_kcs_base 
	 * so we can reuse them on the next call */
}

/**
 * Send IPMI request with retry logic
 *
 * @v req	IPMI request
 * @v rsp	IPMI response  
 * @ret rc	Return status code
 */
int ipmi_send_request(struct ipmi_req *req, struct ipmi_rs *rsp) {
	int rc;
	int attempts = 0;
	const int max_attempts = 3;
	const int retry_delay_ms = 10; /* 10ms delay between retries */
	
	if (!current_intf)
		return -ENODEV;
	
	DBGC ( &ipmi_kcs_base, "IPMI: Sending request NetFn=0x%02x Cmd=0x%02x (max %d attempts)\n",
	       req->netfn, req->cmd, max_attempts );
	
	while ( attempts < max_attempts ) {
		memset(rsp, 0, sizeof(*rsp));
		
		DBGC ( &ipmi_kcs_base, "IPMI: Request attempt %d/%d\n", attempts + 1, max_attempts );
		rc = current_intf->send_req(req, rsp);
		
		/* Success - return immediately */
		if ( rc == 0 ) {
			if ( attempts > 0 ) {
				DBGC ( &ipmi_kcs_base, "IPMI: Request succeeded on attempt %d\n", attempts + 1 );
			}
			return 0;
		}
		
		attempts++;
		
		/* Determine if error is retryable */
		if ( rc == -ETIMEDOUT || rc == -EIO ) {
			/* Retryable errors - timeout or I/O issues */
			if ( attempts < max_attempts ) {
				DBGC ( &ipmi_kcs_base, "IPMI: Request failed with %s, retrying in %dms (%d/%d)\n",
				       strerror ( rc ), retry_delay_ms, attempts, max_attempts );
				mdelay ( retry_delay_ms );
				continue;
			}
		} else {
			/* Non-retryable error - fail immediately */
			DBGC ( &ipmi_kcs_base, "IPMI: Request failed with non-retryable error: %s\n", strerror ( rc ) );
			break;
		}
	}
	
	DBGC ( &ipmi_kcs_base, "IPMI: Request failed after %d attempts: %s\n", attempts, strerror ( rc ) );
	return rc;
}

/**
 * Start up IPMI interface
 */
static void ipmi_startup(void) {
	int rc;
	
	DBGC ( &ipmi_kcs_base, "IPMI: iPXE startup - initializing IPMI subsystem\n" );
	
	/* Detect and initialize IPMI interface */
	DBGC ( &ipmi_kcs_base, "IPMI: Starting hardware detection at boot time\n" );
	rc = ipmi_detect_interface();
	if ( rc != 0 ) {
		DBGC ( &ipmi_kcs_base, "IPMI: No interface detected during startup (%s)\n",
		       strerror ( rc ) );
		DBGC ( &ipmi_kcs_base, "IPMI: IPMI subsystem will be unavailable\n" );
		return;
	}
	
	/* Initialize the interface */
	DBGC ( &ipmi_kcs_base, "IPMI: Hardware detected, initializing interface\n" );
	rc = ipmi_init();
	if ( rc != 0 ) {
		DBGC ( &ipmi_kcs_base, "IPMI: Interface initialization failed: %s\n",
		       strerror ( rc ) );
		DBGC ( &ipmi_kcs_base, "IPMI: IPMI commands will be unavailable\n" );
		return;
	}
	
	DBGC ( &ipmi_kcs_base, "IPMI: Subsystem initialized successfully, commands available\n" );
}

/**
 * Shut down IPMI interface
 *
 * @v booting		True if system is booting, false if shutting down
 */
static void ipmi_shutdown(int booting __unused) {
	DBGC ( &ipmi_kcs_base, "IPMI: Shutting down IPMI subsystem\n" );
	ipmi_cleanup();
	DBGC ( &ipmi_kcs_base, "IPMI: Shutdown completed\n" );
}

/** IPMI startup function */
struct startup_fn ipmi_startup_fn __startup_fn ( STARTUP_NORMAL ) = {
	.name = "ipmi",
	.startup = ipmi_startup,
	.shutdown = ipmi_shutdown,
};