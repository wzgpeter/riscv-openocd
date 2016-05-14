/***************************************************************************
 *   Copyright (C) 2006 by Magnus Lundin                                   *
 *   lundin@mlu.mine.nu                                                    *
 *                                                                         *
 *   Copyright (C) 2008 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 *                                                                         *
 *   Copyright (C) 2009-2010 by Oyvind Harboe                              *
 *   oyvind.harboe@zylin.com                                               *
 *                                                                         *
 *   Copyright (C) 2009-2010 by David Brownell                             *
 *                                                                         *
 *   Copyright (C) 2013 by Andreas Fritiofson                              *
 *   andreas.fritiofson@gmail.com                                          *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

/**
 * @file
 * This file implements support for the ARM Debug Interface version 5 (ADIv5)
 * debugging architecture.  Compared with previous versions, this includes
 * a low pin-count Serial Wire Debug (SWD) alternative to JTAG for message
 * transport, and focusses on memory mapped resources as defined by the
 * CoreSight architecture.
 *
 * A key concept in ADIv5 is the Debug Access Port, or DAP.  A DAP has two
 * basic components:  a Debug Port (DP) transporting messages to and from a
 * debugger, and an Access Port (AP) accessing resources.  Three types of DP
 * are defined.  One uses only JTAG for communication, and is called JTAG-DP.
 * One uses only SWD for communication, and is called SW-DP.  The third can
 * use either SWD or JTAG, and is called SWJ-DP.  The most common type of AP
 * is used to access memory mapped resources and is called a MEM-AP.  Also a
 * JTAG-AP is also defined, bridging to JTAG resources; those are uncommon.
 *
 * This programming interface allows DAP pipelined operations through a
 * transaction queue.  This primarily affects AP operations (such as using
 * a MEM-AP to access memory or registers).  If the current transaction has
 * not finished by the time the next one must begin, and the ORUNDETECT bit
 * is set in the DP_CTRL_STAT register, the SSTICKYORUN status is set and
 * further AP operations will fail.  There are two basic methods to avoid
 * such overrun errors.  One involves polling for status instead of using
 * transaction piplining.  The other involves adding delays to ensure the
 * AP has enough time to complete one operation before starting the next
 * one.  (For JTAG these delays are controlled by memaccess_tck.)
 */

/*
 * Relevant specifications from ARM include:
 *
 * ARM(tm) Debug Interface v5 Architecture Specification    ARM IHI 0031A
 * CoreSight(tm) v1.0 Architecture Specification            ARM IHI 0029B
 *
 * CoreSight(tm) DAP-Lite TRM, ARM DDI 0316D
 * Cortex-M3(tm) TRM, ARM DDI 0337G
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "jtag/interface.h"
#include "arm.h"
#include "arm_adi_v5.h"
#include <helper/jep106.h>
#include <helper/time_support.h>
#include <helper/list.h>

/* ARM ADI Specification requires at least 10 bits used for TAR autoincrement  */

/*
	uint32_t tar_block_size(uint32_t address)
	Return the largest block starting at address that does not cross a tar block size alignment boundary
*/
static uint32_t max_tar_block_size(uint32_t tar_autoincr_block, uint32_t address)
{
	return tar_autoincr_block - ((tar_autoincr_block - 1) & address);
}

/***************************************************************************
 *                                                                         *
 * DP and MEM-AP  register access  through APACC and DPACC                 *
 *                                                                         *
***************************************************************************/

static int mem_ap_setup_csw(struct adiv5_ap *ap, uint32_t csw)
{
	csw = csw | CSW_DBGSWENABLE | CSW_MASTER_DEBUG | CSW_HPROT |
		ap->csw_default;

	if (csw != ap->csw_value) {
		/* LOG_DEBUG("DAP: Set CSW %x",csw); */
		int retval = dap_queue_ap_write(ap, MEM_AP_REG_CSW, csw);
		if (retval != ERROR_OK)
			return retval;
		ap->csw_value = csw;
	}
	return ERROR_OK;
}

static int mem_ap_setup_tar(struct adiv5_ap *ap, uint32_t tar)
{
	if (tar != ap->tar_value ||
			(ap->csw_value & CSW_ADDRINC_MASK)) {
		/* LOG_DEBUG("DAP: Set TAR %x",tar); */
		int retval = dap_queue_ap_write(ap, MEM_AP_REG_TAR, tar);
		if (retval != ERROR_OK)
			return retval;
		ap->tar_value = tar;
	}
	return ERROR_OK;
}

/**
 * Queue transactions setting up transfer parameters for the
 * currently selected MEM-AP.
 *
 * Subsequent transfers using registers like MEM_AP_REG_DRW or MEM_AP_REG_BD2
 * initiate data reads or writes using memory or peripheral addresses.
 * If the CSW is configured for it, the TAR may be automatically
 * incremented after each transfer.
 *
 * @param ap The MEM-AP.
 * @param csw MEM-AP Control/Status Word (CSW) register to assign.  If this
 *	matches the cached value, the register is not changed.
 * @param tar MEM-AP Transfer Address Register (TAR) to assign.  If this
 *	matches the cached address, the register is not changed.
 *
 * @return ERROR_OK if the transaction was properly queued, else a fault code.
 */
static int mem_ap_setup_transfer(struct adiv5_ap *ap, uint32_t csw, uint32_t tar)
{
	int retval;
	retval = mem_ap_setup_csw(ap, csw);
	if (retval != ERROR_OK)
		return retval;
	retval = mem_ap_setup_tar(ap, tar);
	if (retval != ERROR_OK)
		return retval;
	return ERROR_OK;
}

/**
 * Asynchronous (queued) read of a word from memory or a system register.
 *
 * @param ap The MEM-AP to access.
 * @param address Address of the 32-bit word to read; it must be
 *	readable by the currently selected MEM-AP.
 * @param value points to where the word will be stored when the
 *	transaction queue is flushed (assuming no errors).
 *
 * @return ERROR_OK for success.  Otherwise a fault code.
 */
int mem_ap_read_u32(struct adiv5_ap *ap, uint32_t address,
		uint32_t *value)
{
	int retval;

	/* Use banked addressing (REG_BDx) to avoid some link traffic
	 * (updating TAR) when reading several consecutive addresses.
	 */
	retval = mem_ap_setup_transfer(ap, CSW_32BIT | CSW_ADDRINC_OFF,
			address & 0xFFFFFFF0);
	if (retval != ERROR_OK)
		return retval;

	return dap_queue_ap_read(ap, MEM_AP_REG_BD0 | (address & 0xC), value);
}

/**
 * Synchronous read of a word from memory or a system register.
 * As a side effect, this flushes any queued transactions.
 *
 * @param ap The MEM-AP to access.
 * @param address Address of the 32-bit word to read; it must be
 *	readable by the currently selected MEM-AP.
 * @param value points to where the result will be stored.
 *
 * @return ERROR_OK for success; *value holds the result.
 * Otherwise a fault code.
 */
int mem_ap_read_atomic_u32(struct adiv5_ap *ap, uint32_t address,
		uint32_t *value)
{
	int retval;

	retval = mem_ap_read_u32(ap, address, value);
	if (retval != ERROR_OK)
		return retval;

	return dap_run(ap->dap);
}

/**
 * Asynchronous (queued) write of a word to memory or a system register.
 *
 * @param ap The MEM-AP to access.
 * @param address Address to be written; it must be writable by
 *	the currently selected MEM-AP.
 * @param value Word that will be written to the address when transaction
 *	queue is flushed (assuming no errors).
 *
 * @return ERROR_OK for success.  Otherwise a fault code.
 */
int mem_ap_write_u32(struct adiv5_ap *ap, uint32_t address,
		uint32_t value)
{
	int retval;

	/* Use banked addressing (REG_BDx) to avoid some link traffic
	 * (updating TAR) when writing several consecutive addresses.
	 */
	retval = mem_ap_setup_transfer(ap, CSW_32BIT | CSW_ADDRINC_OFF,
			address & 0xFFFFFFF0);
	if (retval != ERROR_OK)
		return retval;

	return dap_queue_ap_write(ap, MEM_AP_REG_BD0 | (address & 0xC),
			value);
}

/**
 * Synchronous write of a word to memory or a system register.
 * As a side effect, this flushes any queued transactions.
 *
 * @param ap The MEM-AP to access.
 * @param address Address to be written; it must be writable by
 *	the currently selected MEM-AP.
 * @param value Word that will be written.
 *
 * @return ERROR_OK for success; the data was written.  Otherwise a fault code.
 */
int mem_ap_write_atomic_u32(struct adiv5_ap *ap, uint32_t address,
		uint32_t value)
{
	int retval = mem_ap_write_u32(ap, address, value);

	if (retval != ERROR_OK)
		return retval;

	return dap_run(ap->dap);
}

/**
 * Synchronous write of a block of memory, using a specific access size.
 *
 * @param ap The MEM-AP to access.
 * @param buffer The data buffer to write. No particular alignment is assumed.
 * @param size Which access size to use, in bytes. 1, 2 or 4.
 * @param count The number of writes to do (in size units, not bytes).
 * @param address Address to be written; it must be writable by the currently selected MEM-AP.
 * @param addrinc Whether the target address should be increased for each write or not. This
 *  should normally be true, except when writing to e.g. a FIFO.
 * @return ERROR_OK on success, otherwise an error code.
 */
static int mem_ap_write(struct adiv5_ap *ap, const uint8_t *buffer, uint32_t size, uint32_t count,
		uint32_t address, bool addrinc)
{
	struct adiv5_dap *dap = ap->dap;
	size_t nbytes = size * count;
	const uint32_t csw_addrincr = addrinc ? CSW_ADDRINC_SINGLE : CSW_ADDRINC_OFF;
	uint32_t csw_size;
	uint32_t addr_xor;
	int retval;

	/* TI BE-32 Quirks mode:
	 * Writes on big-endian TMS570 behave very strangely. Observed behavior:
	 *   size   write address   bytes written in order
	 *   4      TAR ^ 0         (val >> 24), (val >> 16), (val >> 8), (val)
	 *   2      TAR ^ 2         (val >> 8), (val)
	 *   1      TAR ^ 3         (val)
	 * For example, if you attempt to write a single byte to address 0, the processor
	 * will actually write a byte to address 3.
	 *
	 * To make writes of size < 4 work as expected, we xor a value with the address before
	 * setting the TAP, and we set the TAP after every transfer rather then relying on
	 * address increment. */

	if (size == 4) {
		csw_size = CSW_32BIT;
		addr_xor = 0;
	} else if (size == 2) {
		csw_size = CSW_16BIT;
		addr_xor = dap->ti_be_32_quirks ? 2 : 0;
	} else if (size == 1) {
		csw_size = CSW_8BIT;
		addr_xor = dap->ti_be_32_quirks ? 3 : 0;
	} else {
		return ERROR_TARGET_UNALIGNED_ACCESS;
	}

	if (ap->unaligned_access_bad && (address % size != 0))
		return ERROR_TARGET_UNALIGNED_ACCESS;

	retval = mem_ap_setup_tar(ap, address ^ addr_xor);
	if (retval != ERROR_OK)
		return retval;

	while (nbytes > 0) {
		uint32_t this_size = size;

		/* Select packed transfer if possible */
		if (addrinc && ap->packed_transfers && nbytes >= 4
				&& max_tar_block_size(ap->tar_autoincr_block, address) >= 4) {
			this_size = 4;
			retval = mem_ap_setup_csw(ap, csw_size | CSW_ADDRINC_PACKED);
		} else {
			retval = mem_ap_setup_csw(ap, csw_size | csw_addrincr);
		}

		if (retval != ERROR_OK)
			break;

		/* How many source bytes each transfer will consume, and their location in the DRW,
		 * depends on the type of transfer and alignment. See ARM document IHI0031C. */
		uint32_t outvalue = 0;
		if (dap->ti_be_32_quirks) {
			switch (this_size) {
			case 4:
				outvalue |= (uint32_t)*buffer++ << 8 * (3 ^ (address++ & 3) ^ addr_xor);
				outvalue |= (uint32_t)*buffer++ << 8 * (3 ^ (address++ & 3) ^ addr_xor);
				outvalue |= (uint32_t)*buffer++ << 8 * (3 ^ (address++ & 3) ^ addr_xor);
				outvalue |= (uint32_t)*buffer++ << 8 * (3 ^ (address++ & 3) ^ addr_xor);
				break;
			case 2:
				outvalue |= (uint32_t)*buffer++ << 8 * (1 ^ (address++ & 3) ^ addr_xor);
				outvalue |= (uint32_t)*buffer++ << 8 * (1 ^ (address++ & 3) ^ addr_xor);
				break;
			case 1:
				outvalue |= (uint32_t)*buffer++ << 8 * (0 ^ (address++ & 3) ^ addr_xor);
				break;
			}
		} else {
			switch (this_size) {
			case 4:
				outvalue |= (uint32_t)*buffer++ << 8 * (address++ & 3);
				outvalue |= (uint32_t)*buffer++ << 8 * (address++ & 3);
			case 2:
				outvalue |= (uint32_t)*buffer++ << 8 * (address++ & 3);
			case 1:
				outvalue |= (uint32_t)*buffer++ << 8 * (address++ & 3);
			}
		}

		nbytes -= this_size;

		retval = dap_queue_ap_write(ap, MEM_AP_REG_DRW, outvalue);
		if (retval != ERROR_OK)
			break;

		/* Rewrite TAR if it wrapped or we're xoring addresses */
		if (addrinc && (addr_xor || (address % ap->tar_autoincr_block < size && nbytes > 0))) {
			retval = mem_ap_setup_tar(ap, address ^ addr_xor);
			if (retval != ERROR_OK)
				break;
		}
	}

	/* REVISIT: Might want to have a queued version of this function that does not run. */
	if (retval == ERROR_OK)
		retval = dap_run(dap);

	if (retval != ERROR_OK) {
		uint32_t tar;
		if (dap_queue_ap_read(ap, MEM_AP_REG_TAR, &tar) == ERROR_OK
				&& dap_run(dap) == ERROR_OK)
			LOG_ERROR("Failed to write memory at 0x%08"PRIx32, tar);
		else
			LOG_ERROR("Failed to write memory and, additionally, failed to find out where");
	}

	return retval;
}

/**
 * Synchronous read of a block of memory, using a specific access size.
 *
 * @param ap The MEM-AP to access.
 * @param buffer The data buffer to receive the data. No particular alignment is assumed.
 * @param size Which access size to use, in bytes. 1, 2 or 4.
 * @param count The number of reads to do (in size units, not bytes).
 * @param address Address to be read; it must be readable by the currently selected MEM-AP.
 * @param addrinc Whether the target address should be increased after each read or not. This
 *  should normally be true, except when reading from e.g. a FIFO.
 * @return ERROR_OK on success, otherwise an error code.
 */
static int mem_ap_read(struct adiv5_ap *ap, uint8_t *buffer, uint32_t size, uint32_t count,
		uint32_t adr, bool addrinc)
{
	struct adiv5_dap *dap = ap->dap;
	size_t nbytes = size * count;
	const uint32_t csw_addrincr = addrinc ? CSW_ADDRINC_SINGLE : CSW_ADDRINC_OFF;
	uint32_t csw_size;
	uint32_t address = adr;
	int retval;

	/* TI BE-32 Quirks mode:
	 * Reads on big-endian TMS570 behave strangely differently than writes.
	 * They read from the physical address requested, but with DRW byte-reversed.
	 * For example, a byte read from address 0 will place the result in the high bytes of DRW.
	 * Also, packed 8-bit and 16-bit transfers seem to sometimes return garbage in some bytes,
	 * so avoid them. */

	if (size == 4)
		csw_size = CSW_32BIT;
	else if (size == 2)
		csw_size = CSW_16BIT;
	else if (size == 1)
		csw_size = CSW_8BIT;
	else
		return ERROR_TARGET_UNALIGNED_ACCESS;

	if (ap->unaligned_access_bad && (adr % size != 0))
		return ERROR_TARGET_UNALIGNED_ACCESS;

	/* Allocate buffer to hold the sequence of DRW reads that will be made. This is a significant
	 * over-allocation if packed transfers are going to be used, but determining the real need at
	 * this point would be messy. */
	uint32_t *read_buf = malloc(count * sizeof(uint32_t));
	uint32_t *read_ptr = read_buf;
	if (read_buf == NULL) {
		LOG_ERROR("Failed to allocate read buffer");
		return ERROR_FAIL;
	}

	retval = mem_ap_setup_tar(ap, address);
	if (retval != ERROR_OK) {
		free(read_buf);
		return retval;
	}

	/* Queue up all reads. Each read will store the entire DRW word in the read buffer. How many
	 * useful bytes it contains, and their location in the word, depends on the type of transfer
	 * and alignment. */
	while (nbytes > 0) {
		uint32_t this_size = size;

		/* Select packed transfer if possible */
		if (addrinc && ap->packed_transfers && nbytes >= 4
				&& max_tar_block_size(ap->tar_autoincr_block, address) >= 4) {
			this_size = 4;
			retval = mem_ap_setup_csw(ap, csw_size | CSW_ADDRINC_PACKED);
		} else {
			retval = mem_ap_setup_csw(ap, csw_size | csw_addrincr);
		}
		if (retval != ERROR_OK)
			break;

		retval = dap_queue_ap_read(ap, MEM_AP_REG_DRW, read_ptr++);
		if (retval != ERROR_OK)
			break;

		nbytes -= this_size;
		address += this_size;

		/* Rewrite TAR if it wrapped */
		if (addrinc && address % ap->tar_autoincr_block < size && nbytes > 0) {
			retval = mem_ap_setup_tar(ap, address);
			if (retval != ERROR_OK)
				break;
		}
	}

	if (retval == ERROR_OK)
		retval = dap_run(dap);

	/* Restore state */
	address = adr;
	nbytes = size * count;
	read_ptr = read_buf;

	/* If something failed, read TAR to find out how much data was successfully read, so we can
	 * at least give the caller what we have. */
	if (retval != ERROR_OK) {
		uint32_t tar;
		if (dap_queue_ap_read(ap, MEM_AP_REG_TAR, &tar) == ERROR_OK
				&& dap_run(dap) == ERROR_OK) {
			LOG_ERROR("Failed to read memory at 0x%08"PRIx32, tar);
			if (nbytes > tar - address)
				nbytes = tar - address;
		} else {
			LOG_ERROR("Failed to read memory and, additionally, failed to find out where");
			nbytes = 0;
		}
	}

	/* Replay loop to populate caller's buffer from the correct word and byte lane */
	while (nbytes > 0) {
		uint32_t this_size = size;

		if (addrinc && ap->packed_transfers && nbytes >= 4
				&& max_tar_block_size(ap->tar_autoincr_block, address) >= 4) {
			this_size = 4;
		}

		if (dap->ti_be_32_quirks) {
			switch (this_size) {
			case 4:
				*buffer++ = *read_ptr >> 8 * (3 - (address++ & 3));
				*buffer++ = *read_ptr >> 8 * (3 - (address++ & 3));
			case 2:
				*buffer++ = *read_ptr >> 8 * (3 - (address++ & 3));
			case 1:
				*buffer++ = *read_ptr >> 8 * (3 - (address++ & 3));
			}
		} else {
			switch (this_size) {
			case 4:
				*buffer++ = *read_ptr >> 8 * (address++ & 3);
				*buffer++ = *read_ptr >> 8 * (address++ & 3);
			case 2:
				*buffer++ = *read_ptr >> 8 * (address++ & 3);
			case 1:
				*buffer++ = *read_ptr >> 8 * (address++ & 3);
			}
		}

		read_ptr++;
		nbytes -= this_size;
	}

	free(read_buf);
	return retval;
}

int mem_ap_read_buf(struct adiv5_ap *ap,
		uint8_t *buffer, uint32_t size, uint32_t count, uint32_t address)
{
	return mem_ap_read(ap, buffer, size, count, address, true);
}

int mem_ap_write_buf(struct adiv5_ap *ap,
		const uint8_t *buffer, uint32_t size, uint32_t count, uint32_t address)
{
	return mem_ap_write(ap, buffer, size, count, address, true);
}

int mem_ap_read_buf_noincr(struct adiv5_ap *ap,
		uint8_t *buffer, uint32_t size, uint32_t count, uint32_t address)
{
	return mem_ap_read(ap, buffer, size, count, address, false);
}

int mem_ap_write_buf_noincr(struct adiv5_ap *ap,
		const uint8_t *buffer, uint32_t size, uint32_t count, uint32_t address)
{
	return mem_ap_write(ap, buffer, size, count, address, false);
}

/*--------------------------------------------------------------------------*/


#define DAP_POWER_DOMAIN_TIMEOUT (10)

/* FIXME don't import ... just initialize as
 * part of DAP transport setup
*/
extern const struct dap_ops jtag_dp_ops;

/*--------------------------------------------------------------------------*/

/**
 * Create a new DAP
 */
struct adiv5_dap *dap_init(void)
{
	struct adiv5_dap *dap = calloc(1, sizeof(struct adiv5_dap));
	int i;
	/* Set up with safe defaults */
	for (i = 0; i <= 255; i++) {
		dap->ap[i].dap = dap;
		dap->ap[i].ap_num = i;
		/* memaccess_tck max is 255 */
		dap->ap[i].memaccess_tck = 255;
		/* Number of bits for tar autoincrement, impl. dep. at least 10 */
		dap->ap[i].tar_autoincr_block = (1<<10);
	}
	INIT_LIST_HEAD(&dap->cmd_journal);
	return dap;
}

/**
 * Initialize a DAP.  This sets up the power domains, prepares the DP
 * for further use and activates overrun checking.
 *
 * @param dap The DAP being initialized.
 */
int dap_dp_init(struct adiv5_dap *dap)
{
	int retval;

	LOG_DEBUG(" ");
	/* JTAG-DP or SWJ-DP, in JTAG mode
	 * ... for SWD mode this is patched as part
	 * of link switchover
	 * FIXME: This should already be setup by the respective transport specific DAP creation.
	 */
	if (!dap->ops)
		dap->ops = &jtag_dp_ops;

	dap->select = DP_SELECT_INVALID;
	dap->last_read = NULL;

	for (size_t i = 0; i < 10; i++) {
		/* DP initialization */

		retval = dap_queue_dp_read(dap, DP_CTRL_STAT, NULL);
		if (retval != ERROR_OK)
			continue;

		retval = dap_queue_dp_write(dap, DP_CTRL_STAT, SSTICKYERR);
		if (retval != ERROR_OK)
			continue;

		retval = dap_queue_dp_read(dap, DP_CTRL_STAT, NULL);
		if (retval != ERROR_OK)
			continue;

		dap->dp_ctrl_stat = CDBGPWRUPREQ | CSYSPWRUPREQ;
		retval = dap_queue_dp_write(dap, DP_CTRL_STAT, dap->dp_ctrl_stat);
		if (retval != ERROR_OK)
			continue;

		/* Check that we have debug power domains activated */
		LOG_DEBUG("DAP: wait CDBGPWRUPACK");
		retval = dap_dp_poll_register(dap, DP_CTRL_STAT,
					      CDBGPWRUPACK, CDBGPWRUPACK,
					      DAP_POWER_DOMAIN_TIMEOUT);
		if (retval != ERROR_OK)
			continue;

		LOG_DEBUG("DAP: wait CSYSPWRUPACK");
		retval = dap_dp_poll_register(dap, DP_CTRL_STAT,
					      CSYSPWRUPACK, CSYSPWRUPACK,
					      DAP_POWER_DOMAIN_TIMEOUT);
		if (retval != ERROR_OK)
			continue;

		retval = dap_queue_dp_read(dap, DP_CTRL_STAT, NULL);
		if (retval != ERROR_OK)
			continue;

		/* With debug power on we can activate OVERRUN checking */
		dap->dp_ctrl_stat = CDBGPWRUPREQ | CSYSPWRUPREQ | CORUNDETECT;
		retval = dap_queue_dp_write(dap, DP_CTRL_STAT, dap->dp_ctrl_stat);
		if (retval != ERROR_OK)
			continue;
		retval = dap_queue_dp_read(dap, DP_CTRL_STAT, NULL);
		if (retval != ERROR_OK)
			continue;

		retval = dap_run(dap);
		if (retval != ERROR_OK)
			continue;

		break;
	}

	return retval;
}

/**
 * Initialize a DAP.  This sets up the power domains, prepares the DP
 * for further use, and arranges to use AP #0 for all AP operations
 * until dap_ap-select() changes that policy.
 *
 * @param ap The MEM-AP being initialized.
 */
int mem_ap_init(struct adiv5_ap *ap)
{
	/* check that we support packed transfers */
	uint32_t csw, cfg;
	int retval;
	struct adiv5_dap *dap = ap->dap;

	retval = mem_ap_setup_transfer(ap, CSW_8BIT | CSW_ADDRINC_PACKED, 0);
	if (retval != ERROR_OK)
		return retval;

	retval = dap_queue_ap_read(ap, MEM_AP_REG_CSW, &csw);
	if (retval != ERROR_OK)
		return retval;

	retval = dap_queue_ap_read(ap, MEM_AP_REG_CFG, &cfg);
	if (retval != ERROR_OK)
		return retval;

	retval = dap_run(dap);
	if (retval != ERROR_OK)
		return retval;

	if (csw & CSW_ADDRINC_PACKED)
		ap->packed_transfers = true;
	else
		ap->packed_transfers = false;

	/* Packed transfers on TI BE-32 processors do not work correctly in
	 * many cases. */
	if (dap->ti_be_32_quirks)
		ap->packed_transfers = false;

	LOG_DEBUG("MEM_AP Packed Transfers: %s",
			ap->packed_transfers ? "enabled" : "disabled");

	/* The ARM ADI spec leaves implementation-defined whether unaligned
	 * memory accesses work, only work partially, or cause a sticky error.
	 * On TI BE-32 processors, reads seem to return garbage in some bytes
	 * and unaligned writes seem to cause a sticky error.
	 * TODO: it would be nice to have a way to detect whether unaligned
	 * operations are supported on other processors. */
	ap->unaligned_access_bad = dap->ti_be_32_quirks;

	LOG_DEBUG("MEM_AP CFG: large data %d, long address %d, big-endian %d",
			!!(cfg & 0x04), !!(cfg & 0x02), !!(cfg & 0x01));

	return ERROR_OK;
}

/* CID interpretation -- see ARM IHI 0029B section 3
 * and ARM IHI 0031A table 13-3.
 */
static const char *class_description[16] = {
	"Reserved", "ROM table", "Reserved", "Reserved",
	"Reserved", "Reserved", "Reserved", "Reserved",
	"Reserved", "CoreSight component", "Reserved", "Peripheral Test Block",
	"Reserved", "OptimoDE DESS",
	"Generic IP component", "PrimeCell or System component"
};

static bool is_dap_cid_ok(uint32_t cid)
{
	return (cid & 0xffff0fff) == 0xb105000d;
}

/*
 * This function checks the ID for each access port to find the requested Access Port type
 */
int dap_find_ap(struct adiv5_dap *dap, enum ap_type type_to_find, struct adiv5_ap **ap_out)
{
	int ap_num;

	/* Maximum AP number is 255 since the SELECT register is 8 bits */
	for (ap_num = 0; ap_num <= 255; ap_num++) {

		/* read the IDR register of the Access Port */
		uint32_t id_val = 0;

		int retval = dap_queue_ap_read(dap_ap(dap, ap_num), AP_REG_IDR, &id_val);
		if (retval != ERROR_OK)
			return retval;

		retval = dap_run(dap);

		/* IDR bits:
		 * 31-28 : Revision
		 * 27-24 : JEDEC bank (0x4 for ARM)
		 * 23-17 : JEDEC code (0x3B for ARM)
		 * 16-13 : Class (0b1000=Mem-AP)
		 * 12-8  : Reserved
		 *  7-4  : AP Variant (non-zero for JTAG-AP)
		 *  3-0  : AP Type (0=JTAG-AP 1=AHB-AP 2=APB-AP 4=AXI-AP)
		 */

		/* Reading register for a non-existant AP should not cause an error,
		 * but just to be sure, try to continue searching if an error does happen.
		 */
		if ((retval == ERROR_OK) &&                  /* Register read success */
			((id_val & IDR_JEP106) == IDR_JEP106_ARM) && /* Jedec codes match */
			((id_val & IDR_TYPE) == type_to_find)) {      /* type matches*/

			LOG_DEBUG("Found %s at AP index: %d (IDR=0x%08" PRIX32 ")",
						(type_to_find == AP_TYPE_AHB_AP)  ? "AHB-AP"  :
						(type_to_find == AP_TYPE_APB_AP)  ? "APB-AP"  :
						(type_to_find == AP_TYPE_AXI_AP)  ? "AXI-AP"  :
						(type_to_find == AP_TYPE_JTAG_AP) ? "JTAG-AP" : "Unknown",
						ap_num, id_val);

			*ap_out = &dap->ap[ap_num];
			return ERROR_OK;
		}
	}

	LOG_DEBUG("No %s found",
				(type_to_find == AP_TYPE_AHB_AP)  ? "AHB-AP"  :
				(type_to_find == AP_TYPE_APB_AP)  ? "APB-AP"  :
				(type_to_find == AP_TYPE_AXI_AP)  ? "AXI-AP"  :
				(type_to_find == AP_TYPE_JTAG_AP) ? "JTAG-AP" : "Unknown");
	return ERROR_FAIL;
}

int dap_get_debugbase(struct adiv5_ap *ap,
			uint32_t *dbgbase, uint32_t *apid)
{
	struct adiv5_dap *dap = ap->dap;
	int retval;

	retval = dap_queue_ap_read(ap, MEM_AP_REG_BASE, dbgbase);
	if (retval != ERROR_OK)
		return retval;
	retval = dap_queue_ap_read(ap, AP_REG_IDR, apid);
	if (retval != ERROR_OK)
		return retval;
	retval = dap_run(dap);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

int dap_lookup_cs_component(struct adiv5_ap *ap,
			uint32_t dbgbase, uint8_t type, uint32_t *addr, int32_t *idx)
{
	uint32_t romentry, entry_offset = 0, component_base, devtype;
	int retval;

	*addr = 0;

	do {
		retval = mem_ap_read_atomic_u32(ap, (dbgbase&0xFFFFF000) |
						entry_offset, &romentry);
		if (retval != ERROR_OK)
			return retval;

		component_base = (dbgbase & 0xFFFFF000)
			+ (romentry & 0xFFFFF000);

		if (romentry & 0x1) {
			uint32_t c_cid1;
			retval = mem_ap_read_atomic_u32(ap, component_base | 0xff4, &c_cid1);
			if (retval != ERROR_OK) {
				LOG_ERROR("Can't read component with base address 0x%" PRIx32
					  ", the corresponding core might be turned off", component_base);
				return retval;
			}
			if (((c_cid1 >> 4) & 0x0f) == 1) {
				retval = dap_lookup_cs_component(ap, component_base,
							type, addr, idx);
				if (retval == ERROR_OK)
					break;
				if (retval != ERROR_TARGET_RESOURCE_NOT_AVAILABLE)
					return retval;
			}

			retval = mem_ap_read_atomic_u32(ap,
					(component_base & 0xfffff000) | 0xfcc,
					&devtype);
			if (retval != ERROR_OK)
				return retval;
			if ((devtype & 0xff) == type) {
				if (!*idx) {
					*addr = component_base;
					break;
				} else
					(*idx)--;
			}
		}
		entry_offset += 4;
	} while (romentry > 0);

	if (!*addr)
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;

	return ERROR_OK;
}

static int dap_read_part_id(struct adiv5_ap *ap, uint32_t component_base, uint32_t *cid, uint64_t *pid)
{
	assert((component_base & 0xFFF) == 0);
	assert(ap != NULL && cid != NULL && pid != NULL);

	uint32_t cid0, cid1, cid2, cid3;
	uint32_t pid0, pid1, pid2, pid3, pid4;
	int retval;

	/* IDs are in last 4K section */
	retval = mem_ap_read_u32(ap, component_base + 0xFE0, &pid0);
	if (retval != ERROR_OK)
		return retval;
	retval = mem_ap_read_u32(ap, component_base + 0xFE4, &pid1);
	if (retval != ERROR_OK)
		return retval;
	retval = mem_ap_read_u32(ap, component_base + 0xFE8, &pid2);
	if (retval != ERROR_OK)
		return retval;
	retval = mem_ap_read_u32(ap, component_base + 0xFEC, &pid3);
	if (retval != ERROR_OK)
		return retval;
	retval = mem_ap_read_u32(ap, component_base + 0xFD0, &pid4);
	if (retval != ERROR_OK)
		return retval;
	retval = mem_ap_read_u32(ap, component_base + 0xFF0, &cid0);
	if (retval != ERROR_OK)
		return retval;
	retval = mem_ap_read_u32(ap, component_base + 0xFF4, &cid1);
	if (retval != ERROR_OK)
		return retval;
	retval = mem_ap_read_u32(ap, component_base + 0xFF8, &cid2);
	if (retval != ERROR_OK)
		return retval;
	retval = mem_ap_read_u32(ap, component_base + 0xFFC, &cid3);
	if (retval != ERROR_OK)
		return retval;

	retval = dap_run(ap->dap);
	if (retval != ERROR_OK)
		return retval;

	*cid = (cid3 & 0xff) << 24
			| (cid2 & 0xff) << 16
			| (cid1 & 0xff) << 8
			| (cid0 & 0xff);
	*pid = (uint64_t)(pid4 & 0xff) << 32
			| (pid3 & 0xff) << 24
			| (pid2 & 0xff) << 16
			| (pid1 & 0xff) << 8
			| (pid0 & 0xff);

	return ERROR_OK;
}

/* The designer identity code is encoded as:
 * bits 11:8 : JEP106 Bank (number of continuation codes), only valid when bit 7 is 1.
 * bit 7     : Set when bits 6:0 represent a JEP106 ID and cleared when bits 6:0 represent
 *             a legacy ASCII Identity Code.
 * bits 6:0  : JEP106 Identity Code (without parity) or legacy ASCII code according to bit 7.
 * JEP106 is a standard available from jedec.org
 */

/* Part number interpretations are from Cortex
 * core specs, the CoreSight components TRM
 * (ARM DDI 0314H), CoreSight System Design
 * Guide (ARM DGI 0012D) and ETM specs; also
 * from chip observation (e.g. TI SDTI).
 */

/* The legacy code only used the part number field to identify CoreSight peripherals.
 * This meant that the same part number from two different manufacturers looked the same.
 * It is desirable for all future additions to identify with both part number and JEP106.
 * "ANY_ID" is a wildcard (any JEP106) only to preserve legacy behavior for legacy entries.
 */

#define ANY_ID 0x1000

#define ARM_ID 0x4BB

static const struct {
	uint16_t designer_id;
	uint16_t part_num;
	const char *type;
	const char *full;
} dap_partnums[] = {
	{ ARM_ID, 0x000, "Cortex-M3 SCS",              "(System Control Space)", },
	{ ARM_ID, 0x001, "Cortex-M3 ITM",              "(Instrumentation Trace Module)", },
	{ ARM_ID, 0x002, "Cortex-M3 DWT",              "(Data Watchpoint and Trace)", },
	{ ARM_ID, 0x003, "Cortex-M3 FPB",              "(Flash Patch and Breakpoint)", },
	{ ARM_ID, 0x008, "Cortex-M0 SCS",              "(System Control Space)", },
	{ ARM_ID, 0x00a, "Cortex-M0 DWT",              "(Data Watchpoint and Trace)", },
	{ ARM_ID, 0x00b, "Cortex-M0 BPU",              "(Breakpoint Unit)", },
	{ ARM_ID, 0x00c, "Cortex-M4 SCS",              "(System Control Space)", },
	{ ARM_ID, 0x00d, "CoreSight ETM11",            "(Embedded Trace)", },
	{ ARM_ID, 0x00e, "Cortex-M7 FPB",              "(Flash Patch and Breakpoint)", },
	{ ARM_ID, 0x490, "Cortex-A15 GIC",             "(Generic Interrupt Controller)", },
	{ ARM_ID, 0x4a1, "Cortex-A53 ROM",             "(v8 Memory Map ROM Table)", },
	{ ARM_ID, 0x4a2, "Cortex-A57 ROM",             "(ROM Table)", },
	{ ARM_ID, 0x4a3, "Cortex-A53 ROM",             "(v7 Memory Map ROM Table)", },
	{ ARM_ID, 0x4a4, "Cortex-A72 ROM",             "(ROM Table)", },
	{ ARM_ID, 0x4af, "Cortex-A15 ROM",             "(ROM Table)", },
	{ ARM_ID, 0x4c0, "Cortex-M0+ ROM",             "(ROM Table)", },
	{ ARM_ID, 0x4c3, "Cortex-M3 ROM",              "(ROM Table)", },
	{ ARM_ID, 0x4c4, "Cortex-M4 ROM",              "(ROM Table)", },
	{ ARM_ID, 0x4c7, "Cortex-M7 PPB ROM",          "(Private Peripheral Bus ROM Table)", },
	{ ARM_ID, 0x4c8, "Cortex-M7 ROM",              "(ROM Table)", },
	{ ARM_ID, 0x470, "Cortex-M1 ROM",              "(ROM Table)", },
	{ ARM_ID, 0x471, "Cortex-M0 ROM",              "(ROM Table)", },
	{ ARM_ID, 0x906, "CoreSight CTI",              "(Cross Trigger)", },
	{ ARM_ID, 0x907, "CoreSight ETB",              "(Trace Buffer)", },
	{ ARM_ID, 0x908, "CoreSight CSTF",             "(Trace Funnel)", },
	{ ARM_ID, 0x909, "CoreSight ATBR",             "(Advanced Trace Bus Replicator)", },
	{ ARM_ID, 0x910, "CoreSight ETM9",             "(Embedded Trace)", },
	{ ARM_ID, 0x912, "CoreSight TPIU",             "(Trace Port Interface Unit)", },
	{ ARM_ID, 0x913, "CoreSight ITM",              "(Instrumentation Trace Macrocell)", },
	{ ARM_ID, 0x914, "CoreSight SWO",              "(Single Wire Output)", },
	{ ARM_ID, 0x917, "CoreSight HTM",              "(AHB Trace Macrocell)", },
	{ ARM_ID, 0x920, "CoreSight ETM11",            "(Embedded Trace)", },
	{ ARM_ID, 0x921, "Cortex-A8 ETM",              "(Embedded Trace)", },
	{ ARM_ID, 0x922, "Cortex-A8 CTI",              "(Cross Trigger)", },
	{ ARM_ID, 0x923, "Cortex-M3 TPIU",             "(Trace Port Interface Unit)", },
	{ ARM_ID, 0x924, "Cortex-M3 ETM",              "(Embedded Trace)", },
	{ ARM_ID, 0x925, "Cortex-M4 ETM",              "(Embedded Trace)", },
	{ ARM_ID, 0x930, "Cortex-R4 ETM",              "(Embedded Trace)", },
	{ ARM_ID, 0x931, "Cortex-R5 ETM",              "(Embedded Trace)", },
	{ ARM_ID, 0x932, "CoreSight MTB-M0+",          "(Micro Trace Buffer)", },
	{ ARM_ID, 0x941, "CoreSight TPIU-Lite",        "(Trace Port Interface Unit)", },
	{ ARM_ID, 0x950, "Cortex-A9 PTM",              "(Program Trace Macrocell)", },
	{ ARM_ID, 0x955, "Cortex-A5 ETM",              "(Embedded Trace)", },
	{ ARM_ID, 0x95a, "Cortex-A72 ETM",             "(Embedded Trace)", },
	{ ARM_ID, 0x95b, "Cortex-A17 PTM",             "(Program Trace Macrocell)", },
	{ ARM_ID, 0x95d, "Cortex-A53 ETM",             "(Embedded Trace)", },
	{ ARM_ID, 0x95e, "Cortex-A57 ETM",             "(Embedded Trace)", },
	{ ARM_ID, 0x95f, "Cortex-A15 PTM",             "(Program Trace Macrocell)", },
	{ ARM_ID, 0x961, "CoreSight TMC",              "(Trace Memory Controller)", },
	{ ARM_ID, 0x962, "CoreSight STM",              "(System Trace Macrocell)", },
	{ ARM_ID, 0x975, "Cortex-M7 ETM",              "(Embedded Trace)", },
	{ ARM_ID, 0x9a0, "CoreSight PMU",              "(Performance Monitoring Unit)", },
	{ ARM_ID, 0x9a1, "Cortex-M4 TPIU",             "(Trace Port Interface Unit)", },
	{ ARM_ID, 0x9a4, "CoreSight GPR",              "(Granular Power Requester)", },
	{ ARM_ID, 0x9a5, "Cortex-A5 PMU",              "(Performance Monitor Unit)", },
	{ ARM_ID, 0x9a7, "Cortex-A7 PMU",              "(Performance Monitor Unit)", },
	{ ARM_ID, 0x9a8, "Cortex-A53 CTI",             "(Cross Trigger)", },
	{ ARM_ID, 0x9a9, "Cortex-M7 TPIU",             "(Trace Port Interface Unit)", },
	{ ARM_ID, 0x9ae, "Cortex-A17 PMU",             "(Performance Monitor Unit)", },
	{ ARM_ID, 0x9af, "Cortex-A15 PMU",             "(Performance Monitor Unit)", },
	{ ARM_ID, 0x9b7, "Cortex-R7 PMU",              "(Performance Monitoring Unit)", },
	{ ARM_ID, 0x9d3, "Cortex-A53 PMU",             "(Performance Monitor Unit)", },
	{ ARM_ID, 0x9d7, "Cortex-A57 PMU",             "(Performance Monitor Unit)", },
	{ ARM_ID, 0x9d8, "Cortex-A72 PMU",             "(Performance Monitor Unit)", },
	{ ARM_ID, 0xc05, "Cortex-A5 Debug",            "(Debug Unit)", },
	{ ARM_ID, 0xc07, "Cortex-A7 Debug",            "(Debug Unit)", },
	{ ARM_ID, 0xc08, "Cortex-A8 Debug",            "(Debug Unit)", },
	{ ARM_ID, 0xc09, "Cortex-A9 Debug",            "(Debug Unit)", },
	{ ARM_ID, 0xc0e, "Cortex-A17 Debug",           "(Debug Unit)", },
	{ ARM_ID, 0xc0f, "Cortex-A15 Debug",           "(Debug Unit)", },
	{ ARM_ID, 0xc14, "Cortex-R4 Debug",            "(Debug Unit)", },
	{ ARM_ID, 0xc15, "Cortex-R5 Debug",            "(Debug Unit)", },
	{ ARM_ID, 0xc17, "Cortex-R7 Debug",            "(Debug Unit)", },
	{ ARM_ID, 0xd03, "Cortex-A53 Debug",           "(Debug Unit)", },
	{ ARM_ID, 0xd07, "Cortex-A57 Debug",           "(Debug Unit)", },
	{ ARM_ID, 0xd08, "Cortex-A72 Debug",           "(Debug Unit)", },
	{ 0x09f,  0xcd0, "Atmel CPU with DSU",         "(CPU)" },
	{ 0x0E5,  0x000, "SHARC+/Blackfin+",           "", },
	{ 0x0F0,  0x440, "Qualcomm QDSS Component v1", "(Qualcomm Designed CoreSight Component v1)", },
	/* legacy comment: 0x113: what? */
	{ ANY_ID, 0x120, "TI SDTI",                    "(System Debug Trace Interface)", }, /* from OMAP3 memmap */
	{ ANY_ID, 0x343, "TI DAPCTL",                  "", }, /* from OMAP3 memmap */
};

static int dap_rom_display(struct command_context *cmd_ctx,
				struct adiv5_ap *ap, uint32_t dbgbase, int depth)
{
	int retval;
	uint64_t pid;
	uint32_t cid;
	char tabs[7] = "";

	if (depth > 16) {
		command_print(cmd_ctx, "\tTables too deep");
		return ERROR_FAIL;
	}

	if (depth)
		snprintf(tabs, sizeof(tabs), "[L%02d] ", depth);

	uint32_t base_addr = dbgbase & 0xFFFFF000;
	command_print(cmd_ctx, "\t\tComponent base address 0x%08" PRIx32, base_addr);

	retval = dap_read_part_id(ap, base_addr, &cid, &pid);
	if (retval != ERROR_OK) {
		command_print(cmd_ctx, "\t\tCan't read component, the corresponding core might be turned off");
		return ERROR_OK; /* Don't abort recursion */
	}

	if (!is_dap_cid_ok(cid)) {
		command_print(cmd_ctx, "\t\tInvalid CID 0x%08" PRIx32, cid);
		return ERROR_OK; /* Don't abort recursion */
	}

	/* component may take multiple 4K pages */
	uint32_t size = (pid >> 36) & 0xf;
	if (size > 0)
		command_print(cmd_ctx, "\t\tStart address 0x%08" PRIx32, (uint32_t)(base_addr - 0x1000 * size));

	command_print(cmd_ctx, "\t\tPeripheral ID 0x%010" PRIx64, pid);

	uint8_t class = (cid >> 12) & 0xf;
	uint16_t part_num = pid & 0xfff;
	uint16_t designer_id = ((pid >> 32) & 0xf) << 8 | ((pid >> 12) & 0xff);

	if (designer_id & 0x80) {
		/* JEP106 code */
		command_print(cmd_ctx, "\t\tDesigner is 0x%03" PRIx16 ", %s",
				designer_id, jep106_manufacturer(designer_id >> 8, designer_id & 0x7f));
	} else {
		/* Legacy ASCII ID, clear invalid bits */
		designer_id &= 0x7f;
		command_print(cmd_ctx, "\t\tDesigner ASCII code 0x%02" PRIx16 ", %s",
				designer_id, designer_id == 0x41 ? "ARM" : "<unknown>");
	}

	/* default values to be overwritten upon finding a match */
	const char *type = "Unrecognized";
	const char *full = "";

	/* search dap_partnums[] array for a match */
	for (unsigned entry = 0; entry < ARRAY_SIZE(dap_partnums); entry++) {

		if ((dap_partnums[entry].designer_id != designer_id) && (dap_partnums[entry].designer_id != ANY_ID))
			continue;

		if (dap_partnums[entry].part_num != part_num)
			continue;

		type = dap_partnums[entry].type;
		full = dap_partnums[entry].full;
		break;
	}

	command_print(cmd_ctx, "\t\tPart is 0x%" PRIx16", %s %s", part_num, type, full);
	command_print(cmd_ctx, "\t\tComponent class is 0x%" PRIx8 ", %s", class, class_description[class]);

	if (class == 1) { /* ROM Table */
		uint32_t memtype;
		retval = mem_ap_read_atomic_u32(ap, base_addr | 0xFCC, &memtype);
		if (retval != ERROR_OK)
			return retval;

		if (memtype & 0x01)
			command_print(cmd_ctx, "\t\tMEMTYPE system memory present on bus");
		else
			command_print(cmd_ctx, "\t\tMEMTYPE system memory not present: dedicated debug bus");

		/* Read ROM table entries from base address until we get 0x00000000 or reach the reserved area */
		for (uint16_t entry_offset = 0; entry_offset < 0xF00; entry_offset += 4) {
			uint32_t romentry;
			retval = mem_ap_read_atomic_u32(ap, base_addr | entry_offset, &romentry);
			if (retval != ERROR_OK)
				return retval;
			command_print(cmd_ctx, "\t%sROMTABLE[0x%x] = 0x%" PRIx32 "",
					tabs, entry_offset, romentry);
			if (romentry & 0x01) {
				/* Recurse */
				retval = dap_rom_display(cmd_ctx, ap, base_addr + (romentry & 0xFFFFF000), depth + 1);
				if (retval != ERROR_OK)
					return retval;
			} else if (romentry != 0) {
				command_print(cmd_ctx, "\t\tComponent not present");
			} else {
				command_print(cmd_ctx, "\t%s\tEnd of ROM table", tabs);
				break;
			}
		}
	} else if (class == 9) { /* CoreSight component */
		const char *major = "Reserved", *subtype = "Reserved";

		uint32_t devtype;
		retval = mem_ap_read_atomic_u32(ap, base_addr | 0xFCC, &devtype);
		if (retval != ERROR_OK)
			return retval;
		unsigned minor = (devtype >> 4) & 0x0f;
		switch (devtype & 0x0f) {
		case 0:
			major = "Miscellaneous";
			switch (minor) {
			case 0:
				subtype = "other";
				break;
			case 4:
				subtype = "Validation component";
				break;
			}
			break;
		case 1:
			major = "Trace Sink";
			switch (minor) {
			case 0:
				subtype = "other";
				break;
			case 1:
				subtype = "Port";
				break;
			case 2:
				subtype = "Buffer";
				break;
			case 3:
				subtype = "Router";
				break;
			}
			break;
		case 2:
			major = "Trace Link";
			switch (minor) {
			case 0:
				subtype = "other";
				break;
			case 1:
				subtype = "Funnel, router";
				break;
			case 2:
				subtype = "Filter";
				break;
			case 3:
				subtype = "FIFO, buffer";
				break;
			}
			break;
		case 3:
			major = "Trace Source";
			switch (minor) {
			case 0:
				subtype = "other";
				break;
			case 1:
				subtype = "Processor";
				break;
			case 2:
				subtype = "DSP";
				break;
			case 3:
				subtype = "Engine/Coprocessor";
				break;
			case 4:
				subtype = "Bus";
				break;
			case 6:
				subtype = "Software";
				break;
			}
			break;
		case 4:
			major = "Debug Control";
			switch (minor) {
			case 0:
				subtype = "other";
				break;
			case 1:
				subtype = "Trigger Matrix";
				break;
			case 2:
				subtype = "Debug Auth";
				break;
			case 3:
				subtype = "Power Requestor";
				break;
			}
			break;
		case 5:
			major = "Debug Logic";
			switch (minor) {
			case 0:
				subtype = "other";
				break;
			case 1:
				subtype = "Processor";
				break;
			case 2:
				subtype = "DSP";
				break;
			case 3:
				subtype = "Engine/Coprocessor";
				break;
			case 4:
				subtype = "Bus";
				break;
			case 5:
				subtype = "Memory";
				break;
			}
			break;
		case 6:
			major = "Perfomance Monitor";
			switch (minor) {
			case 0:
				subtype = "other";
				break;
			case 1:
				subtype = "Processor";
				break;
			case 2:
				subtype = "DSP";
				break;
			case 3:
				subtype = "Engine/Coprocessor";
				break;
			case 4:
				subtype = "Bus";
				break;
			case 5:
				subtype = "Memory";
				break;
			}
			break;
		}
		command_print(cmd_ctx, "\t\tType is 0x%02" PRIx8 ", %s, %s",
				(uint8_t)(devtype & 0xff),
				major, subtype);
		/* REVISIT also show 0xfc8 DevId */
	}

	return ERROR_OK;
}

static int dap_info_command(struct command_context *cmd_ctx,
		struct adiv5_ap *ap)
{
	int retval;
	uint32_t dbgbase, apid;
	uint8_t mem_ap;

	/* Now we read ROM table ID registers, ref. ARM IHI 0029B sec  */
	retval = dap_get_debugbase(ap, &dbgbase, &apid);
	if (retval != ERROR_OK)
		return retval;

	command_print(cmd_ctx, "AP ID register 0x%8.8" PRIx32, apid);
	if (apid == 0) {
		command_print(cmd_ctx, "No AP found at this ap 0x%x", ap->ap_num);
		return ERROR_FAIL;
	}

	switch (apid & (IDR_JEP106 | IDR_TYPE)) {
	case IDR_JEP106_ARM | AP_TYPE_JTAG_AP:
		command_print(cmd_ctx, "\tType is JTAG-AP");
		break;
	case IDR_JEP106_ARM | AP_TYPE_AHB_AP:
		command_print(cmd_ctx, "\tType is MEM-AP AHB");
		break;
	case IDR_JEP106_ARM | AP_TYPE_APB_AP:
		command_print(cmd_ctx, "\tType is MEM-AP APB");
		break;
	case IDR_JEP106_ARM | AP_TYPE_AXI_AP:
		command_print(cmd_ctx, "\tType is MEM-AP AXI");
		break;
	default:
		command_print(cmd_ctx, "\tUnknown AP type");
		break;
	}

	/* NOTE: a MEM-AP may have a single CoreSight component that's
	 * not a ROM table ... or have no such components at all.
	 */
	mem_ap = (apid & IDR_CLASS) == AP_CLASS_MEM_AP;
	if (mem_ap) {
		command_print(cmd_ctx, "MEM-AP BASE 0x%8.8" PRIx32, dbgbase);

		if (dbgbase == 0xFFFFFFFF || (dbgbase & 0x3) == 0x2) {
			command_print(cmd_ctx, "\tNo ROM table present");
		} else {
			if (dbgbase & 0x01)
				command_print(cmd_ctx, "\tValid ROM table present");
			else
				command_print(cmd_ctx, "\tROM table in legacy format");

			dap_rom_display(cmd_ctx, ap, dbgbase & 0xFFFFF000, 0);
		}
	}

	return ERROR_OK;
}

COMMAND_HANDLER(handle_dap_info_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct arm *arm = target_to_arm(target);
	struct adiv5_dap *dap = arm->dap;
	uint32_t apsel;

	switch (CMD_ARGC) {
	case 0:
		apsel = dap->apsel;
		break;
	case 1:
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], apsel);
		if (apsel >= 256)
			return ERROR_COMMAND_SYNTAX_ERROR;
		break;
	default:
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	return dap_info_command(CMD_CTX, &dap->ap[apsel]);
}

COMMAND_HANDLER(dap_baseaddr_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct arm *arm = target_to_arm(target);
	struct adiv5_dap *dap = arm->dap;

	uint32_t apsel, baseaddr;
	int retval;

	switch (CMD_ARGC) {
	case 0:
		apsel = dap->apsel;
		break;
	case 1:
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], apsel);
		/* AP address is in bits 31:24 of DP_SELECT */
		if (apsel >= 256)
			return ERROR_COMMAND_SYNTAX_ERROR;
		break;
	default:
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	/* NOTE:  assumes we're talking to a MEM-AP, which
	 * has a base address.  There are other kinds of AP,
	 * though they're not common for now.  This should
	 * use the ID register to verify it's a MEM-AP.
	 */
	retval = dap_queue_ap_read(dap_ap(dap, apsel), MEM_AP_REG_BASE, &baseaddr);
	if (retval != ERROR_OK)
		return retval;
	retval = dap_run(dap);
	if (retval != ERROR_OK)
		return retval;

	command_print(CMD_CTX, "0x%8.8" PRIx32, baseaddr);

	return retval;
}

COMMAND_HANDLER(dap_memaccess_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct arm *arm = target_to_arm(target);
	struct adiv5_dap *dap = arm->dap;

	uint32_t memaccess_tck;

	switch (CMD_ARGC) {
	case 0:
		memaccess_tck = dap->ap[dap->apsel].memaccess_tck;
		break;
	case 1:
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], memaccess_tck);
		break;
	default:
		return ERROR_COMMAND_SYNTAX_ERROR;
	}
	dap->ap[dap->apsel].memaccess_tck = memaccess_tck;

	command_print(CMD_CTX, "memory bus access delay set to %" PRIi32 " tck",
			dap->ap[dap->apsel].memaccess_tck);

	return ERROR_OK;
}

COMMAND_HANDLER(dap_apsel_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct arm *arm = target_to_arm(target);
	struct adiv5_dap *dap = arm->dap;

	uint32_t apsel, apid;
	int retval;

	switch (CMD_ARGC) {
	case 0:
		apsel = dap->apsel;
		break;
	case 1:
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], apsel);
		/* AP address is in bits 31:24 of DP_SELECT */
		if (apsel >= 256)
			return ERROR_COMMAND_SYNTAX_ERROR;
		break;
	default:
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	dap->apsel = apsel;

	retval = dap_queue_ap_read(dap_ap(dap, apsel), AP_REG_IDR, &apid);
	if (retval != ERROR_OK)
		return retval;
	retval = dap_run(dap);
	if (retval != ERROR_OK)
		return retval;

	command_print(CMD_CTX, "ap %" PRIi32 " selected, identification register 0x%8.8" PRIx32,
			apsel, apid);

	return retval;
}

COMMAND_HANDLER(dap_apcsw_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct arm *arm = target_to_arm(target);
	struct adiv5_dap *dap = arm->dap;

	uint32_t apcsw = dap->ap[dap->apsel].csw_default, sprot = 0;

	switch (CMD_ARGC) {
	case 0:
		command_print(CMD_CTX, "apsel %" PRIi32 " selected, csw 0x%8.8" PRIx32,
			(dap->apsel), apcsw);
		break;
	case 1:
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], sprot);
		/* AP address is in bits 31:24 of DP_SELECT */
		if (sprot > 1)
			return ERROR_COMMAND_SYNTAX_ERROR;
		if (sprot)
			apcsw |= CSW_SPROT;
		else
			apcsw &= ~CSW_SPROT;
		break;
	default:
		return ERROR_COMMAND_SYNTAX_ERROR;
	}
	dap->ap[dap->apsel].csw_default = apcsw;

	return 0;
}



COMMAND_HANDLER(dap_apid_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct arm *arm = target_to_arm(target);
	struct adiv5_dap *dap = arm->dap;

	uint32_t apsel, apid;
	int retval;

	switch (CMD_ARGC) {
	case 0:
		apsel = dap->apsel;
		break;
	case 1:
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], apsel);
		/* AP address is in bits 31:24 of DP_SELECT */
		if (apsel >= 256)
			return ERROR_COMMAND_SYNTAX_ERROR;
		break;
	default:
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	retval = dap_queue_ap_read(dap_ap(dap, apsel), AP_REG_IDR, &apid);
	if (retval != ERROR_OK)
		return retval;
	retval = dap_run(dap);
	if (retval != ERROR_OK)
		return retval;

	command_print(CMD_CTX, "0x%8.8" PRIx32, apid);

	return retval;
}

COMMAND_HANDLER(dap_ti_be_32_quirks_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct arm *arm = target_to_arm(target);
	struct adiv5_dap *dap = arm->dap;

	uint32_t enable = dap->ti_be_32_quirks;

	switch (CMD_ARGC) {
	case 0:
		break;
	case 1:
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], enable);
		if (enable > 1)
			return ERROR_COMMAND_SYNTAX_ERROR;
		break;
	default:
		return ERROR_COMMAND_SYNTAX_ERROR;
	}
	dap->ti_be_32_quirks = enable;
	command_print(CMD_CTX, "TI BE-32 quirks mode %s",
		enable ? "enabled" : "disabled");

	return 0;
}

static const struct command_registration dap_commands[] = {
	{
		.name = "info",
		.handler = handle_dap_info_command,
		.mode = COMMAND_EXEC,
		.help = "display ROM table for MEM-AP "
			"(default currently selected AP)",
		.usage = "[ap_num]",
	},
	{
		.name = "apsel",
		.handler = dap_apsel_command,
		.mode = COMMAND_EXEC,
		.help = "Set the currently selected AP (default 0) "
			"and display the result",
		.usage = "[ap_num]",
	},
	{
		.name = "apcsw",
		.handler = dap_apcsw_command,
		.mode = COMMAND_EXEC,
		.help = "Set csw access bit ",
		.usage = "[sprot]",
	},

	{
		.name = "apid",
		.handler = dap_apid_command,
		.mode = COMMAND_EXEC,
		.help = "return ID register from AP "
			"(default currently selected AP)",
		.usage = "[ap_num]",
	},
	{
		.name = "baseaddr",
		.handler = dap_baseaddr_command,
		.mode = COMMAND_EXEC,
		.help = "return debug base address from MEM-AP "
			"(default currently selected AP)",
		.usage = "[ap_num]",
	},
	{
		.name = "memaccess",
		.handler = dap_memaccess_command,
		.mode = COMMAND_EXEC,
		.help = "set/get number of extra tck for MEM-AP memory "
			"bus access [0-255]",
		.usage = "[cycles]",
	},
	{
		.name = "ti_be_32_quirks",
		.handler = dap_ti_be_32_quirks_command,
		.mode = COMMAND_CONFIG,
		.help = "set/get quirks mode for TI TMS450/TMS570 processors",
		.usage = "[enable]",
	},
	COMMAND_REGISTRATION_DONE
};

const struct command_registration dap_command_handlers[] = {
	{
		.name = "dap",
		.mode = COMMAND_EXEC,
		.help = "DAP command group",
		.usage = "",
		.chain = dap_commands,
	},
	COMMAND_REGISTRATION_DONE
};
