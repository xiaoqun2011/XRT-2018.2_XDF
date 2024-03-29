/*
 * A GEM style device manager for MPSoC based OpenCL accelerators.
 *
 * Copyright (C) 2017 Xilinx, Inc. All rights reserved.
 *
 * Authors:
 *    Soren Soe <soren.soe@xilinx.com>
 *    Min Ma <min.ma@xilinx.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/bitmap.h>
#include <linux/list.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include "sched_exec.h"

#define SCHED_VERBOSE
#if defined(__GNUC__)
#define SCHED_UNUSED __attribute__((unused))
#endif

#define sched_error_on(exec, expr, msg) \
	({ \
		unsigned int ret = 0; \
		if ((expr)) { \
			DRM_INFO("Assertion failed: %s:%d:%s:%s %s\n", \
			__FILE__, __LINE__, __func__, #expr, msg); \
			exec->scheduler->error = 1; \
			ret = 1; \
		} \
		(ret); \
	})

#ifdef SCHED_VERBOSE
# define SCHED_DEBUG(format, ...) DRM_DEBUG(format, ##__VA_ARGS__)
#else
# define SCHED_DEBUG(format, ...)
#endif

static struct scheduler global_scheduler0;
/* Scheduler call schedule() every MAX_SCHED_LOOP loop*/
#define MAX_SCHED_LOOP 8
static int    sched_loop_cnt;

static struct sched_ops penguin_ops;
static struct sched_ops ps_ert_ops;

/**
 * List of free sched_cmd objects.
 *
 * @free_cmds: populated with recycled sched_cmd objects
 * @cmd_mutex: mutex lock for cmd_list
 *
 * Command objects are recycled for later use and only freed when kernel
 * module is unloaded.
 */
static LIST_HEAD(free_cmds);
static DEFINE_MUTEX(free_cmds_mutex);

/**
 * List of new pending sched_cmd objects
 *
 * @pending_cmds: populated from user space with new commands for buffer objects
 * @num_pending: number of pending commands
 *
 * Scheduler copies pending commands to its private queue when necessary
 */
static LIST_HEAD(pending_cmds);
static DEFINE_MUTEX(pending_cmds_mutex);
static atomic_t num_pending = ATOMIC_INIT(0);

/**
 * is_ert() - Check if running in embedded (ert) mode.
 *
 * Return: %true of ert mode, %false otherwise
 */
inline unsigned int
is_ert(struct drm_device *dev)
{
	struct drm_zocl_dev *zdev = dev->dev_private;

	return zdev->exec->ops == &ps_ert_ops;
}

/**
 * ffs_or_neg_one() - Find first set bit in a 32 bit mask.
 *
 * @mask: mask to check
 *
 * First LSBit is at position 0.
 *
 * Return: Position of first set bit, or -1 if none
 */
inline int
ffs_or_neg_one(u32 mask)
{
	if (!mask)
		return -1;
	return ffs(mask)-1;
}

/**
 * ffz_or_neg_one() - Find first zero bit in bit mask
 *
 * @mask: mask to check
 * Return: Position of first zero bit, or -1 if none
 */
inline int
ffz_or_neg_one(u32 mask)
{
	if (mask == U32_MASK)
		return -1;
	return ffz(mask);
}

/**
 * slot_size() - slot size per device configuration
 *
 * Return: Command queue slot size
 */
inline unsigned int
slot_size(struct drm_device *dev)
{
	struct drm_zocl_dev *zdev = dev->dev_private;

	return CQ_SIZE / zdev->exec->num_slots;
}

/**
 * cu_mask_idx() - CU mask index for a given cu index
 *
 * @cu_idx: Global [0..127] index of a CU
 * Return: Index of the CU mask containing the CU with cu_idx
 */
inline unsigned int
cu_mask_idx(unsigned int cu_idx)
{
	return cu_idx >> 5; /* 32 cus per mask */
}

/**
 * cu_idx_in_mask() - CU idx within its mask
 *
 * @cu_idx: Global [0..127] index of a CU
 * Return: Index of the CU within the mask that contains it
 */
inline unsigned int
cu_idx_in_mask(unsigned int cu_idx)
{
	return cu_idx - (cu_mask_idx(cu_idx) << 5);
}

/**
 * cu_idx_from_mask() - Get CU's global idx [0..127] by CU idx in a mask
 *
 * @cu_idx: Index of CU with mask identified by mask_idx
 * @mask_idx: Mask index of the has CU with cu_idx
 * Return: Global cu_idx [0..127]
 */
inline unsigned int
cu_idx_from_mask(unsigned int cu_idx, unsigned int mask_idx)
{
	return cu_idx + (mask_idx << 5);
}

/**
 * slot_mask_idx() - Slot mask idx index for a given slot_idx
 *
 * @slot_idx: Global [0..127] index of a CQ slot
 * Return: Index of the slot mask containing the slot_idx
 */
inline unsigned int
slot_mask_idx(unsigned int slot_idx)
{
	return slot_idx >> 5;
}

/**
 * slot_idx_in_mask() - Index of CQ slot within the mask that contains it
 *
 * @slot_idx: Global [0..127] index of a CQ slot
 * Return: Index of slot within the mask that contains it
 */
inline unsigned int
slot_idx_in_mask(unsigned int slot_idx)
{
	return slot_idx - (slot_mask_idx(slot_idx) << 5);
}

/**
 * slot_idx_from_mask_idx() - Get slot global idx [0..127] by slot idx in mask
 *
 * @slot_idx: Index of slot with mask identified by mask_idx
 * @mask_idx: Mask index of the mask hat has slot with slot_idx
 * Return: Global slot_idx [0..127]
 */
inline unsigned int
slot_idx_from_mask_idx(unsigned int slot_idx, unsigned int mask_idx)
{
	return slot_idx + (mask_idx << 5);
}


/**
 * opcode() - Command opcode
 *
 * @cmd: Command object
 * Return: Opcode per command packet
 */
inline u32
opcode(struct sched_cmd *cmd)
{
	return cmd->packet->opcode;
}

/**
 * payload_size() - Command payload size
 *
 * @cmd: Command object
 * Return: Size in number of words of command packet payload
 */
inline u32
payload_size(struct sched_cmd *cmd)
{
	return cmd->packet->count;
}

/**
 * packet_size() - Command packet size
 *
 * @cmd: Command object
 * Return: Size in number of words of command packet
 */
inline u32
packet_size(struct sched_cmd *cmd)
{
	return payload_size(cmd) + 1;
}

/**
 * cu_masks() - Number of command packet cu_masks
 *
 * @cmd: Command object
 * Return: Total number of CU masks in command packet
 */
inline u32
cu_masks(struct sched_cmd *cmd)
{
	struct start_kernel_cmd *sk;

	if (opcode(cmd) != OP_START_KERNEL)
		return 0;
	sk = (struct start_kernel_cmd *)cmd->packet;
	return 1 + sk->extra_cu_masks;
}

/**
 * regmap_size() - Size of regmap
 *
 * @xcmd: Command object
 * Return: Size of register map in number of words
 */
inline u32
regmap_size(struct sched_cmd *cmd)
{
	return payload_size(cmd) - cu_masks(cmd);
}

/**
 * cu_idx_to_addr() - Convert CU idx into it physical address.
 *
 * @dev: Device handle
 * @cu_idx: Global CU idx
 * Return: Address of CU relative to bar
 */
inline u32
cu_idx_to_addr(struct drm_device *dev, unsigned int cu_idx)
{
	struct drm_zocl_dev *zdev = dev->dev_private;

	return (cu_idx << zdev->exec->cu_shift_offset)
		     + zdev->exec->cu_base_addr;
}

/**
 * cu_idx_to_offset() - Convert CU idx into address offset
 *
 * @dev: Device handle
 * @cu_idx: Global CU idx
 * Return: Address of CU relative to bar
 */
inline u32
cu_idx_to_offset(struct drm_device *dev, unsigned int cu_idx)
{
	struct drm_zocl_dev *zdev = dev->dev_private;

	return cu_idx << zdev->exec->cu_shift_offset;
}

/**
 * set_cmd_int_state() - Set internal command state used by scheduler only
 *
 * @xcmd: command to change internal state on
 * @state: new command state per ert.h
 */
inline void
set_cmd_int_state(struct sched_cmd *cmd, enum cmd_state state)
{
	SCHED_DEBUG("-> set_cmd_int_state(,%d)\n", state);
	cmd->state = state;
	SCHED_DEBUG("<- set_cmd_int_state\n");
}

/**
 * setup_ert_hw() - Setup Embedded Hardware HW IP
 *
 * This function will be called by configure()
 */
void setup_ert_hw(struct drm_zocl_dev *zdev)
{
	char *ert_hw = zdev->ert->hw_ioremap;
	struct sched_exec_core *exec = zdev->exec;

	SCHED_DEBUG("slot_size = 0x%x\n", slot_size(zdev->ddev));
	SCHED_DEBUG("num_slots = %d\n", exec->num_slots);
	SCHED_DEBUG("num_slot_masks = %d\n", exec->num_slot_masks);
	SCHED_DEBUG("num_cus = %d\n", exec->num_cus);
	SCHED_DEBUG("num_cu_masks = %d\n", exec->num_cu_masks);
	SCHED_DEBUG("cu_offset = %d\n", exec->cu_shift_offset);
	SCHED_DEBUG("cu_base_address = 0x%x\n", exec->cu_base_addr);
	SCHED_DEBUG("cu_dma = %d\n", exec->cu_dma);
	SCHED_DEBUG("cu_isr = %d\n", exec->cu_isr);
	SCHED_DEBUG("cq_interrupt = %d\n", exec->cq_interrupt);
	SCHED_DEBUG("polling_mode = %d\n", exec->polling_mode);

	/* Set slot size(4K) */
	iowrite32(slot_size(zdev->ddev)/4, ert_hw + ERT_CQ_SLOT_SIZE_REG);

	/* CU offset in shift value */
	iowrite32(exec->cu_shift_offset, ert_hw + ERT_CU_OFFSET_REG);

	/* Number of command slots */
	iowrite32(exec->num_slots, ert_hw + ERT_CQ_NUM_OF_SLOTS_REG);

	/* CU physical address */
	/* TODO: Think about how to make the address mapping correct */
	iowrite32(0x81800000/4, ert_hw + ERT_CU_BASE_ADDR_REG);

	/* Command queue physical address */
	iowrite32(0x80190000/4, ert_hw + ERT_CQ_BASE_ADDR_REG);

	/* Number of CUs */
	iowrite32(exec->num_cus, ert_hw + ERT_NUM_OF_CU_REG);

	/* Enable/Disable CU_DMA module */
	iowrite32(exec->cu_dma, ert_hw + ERT_CU_DMA_ENABLE);

	/* For cu dma 5.2, need to configure cuisr. Ignore it for Fidus 5.1 */

	/* Enable cu interupts (cu -> cu_isr -> PS interrupt) */

	/* Enable interrupt from host to PS when new commands are ready */

	/* Enable C2H interrupts */
	if (!exec->polling_mode)
		iowrite32(0x1, ert_hw + ERT_HOST_INT_ENABLE);
	else
		iowrite32(0x0, ert_hw + ERT_HOST_INT_ENABLE);
}

/**
 * configure() - Configure the scheduler from user space command
 *
 * Process the configure command sent from user space. Only one process can
 * configure the scheduler, so if scheduler is already configured, the
 * function should verify that another process doesn't expect different
 * configuration.
 *
 * Future may need ability to query current configuration so as to keep
 * multiple processes in sync.
 *
 * Return: 0 on success, 1 on failure
 */
static int
configure(struct sched_cmd *cmd)
{
	struct drm_zocl_dev *zdev = cmd->ddev->dev_private;
	struct sched_exec_core *exec = zdev->exec;
	struct configure_cmd *cfg;

	if (sched_error_on(exec, opcode(cmd) != OP_CONFIGURE, "expected configure command"))
		return 1;

	if (!list_empty(&pending_cmds)) {
		DRM_INFO("cannnot configure scheduler when there are pending commands\n");
		return 1;
	}

	if (!list_is_singular(&global_scheduler0.command_queue)) {
		DRM_INFO("cannnot configure scheduler when there are queued commands\n");
		return 1;
	}

	cfg = (struct configure_cmd *)(cmd->packet);

	if (exec->configured != 0) {
		DRM_INFO("reconfiguration of scheduler not supported, using existing configuration\n");
		return 1;
	}

	SCHED_DEBUG("configuring scheduler\n");
	exec->num_slots       = CQ_SIZE / cfg->slot_size;
	exec->num_cus         = cfg->num_cus;
	exec->cu_shift_offset = cfg->cu_shift;
	exec->cu_base_addr    = cfg->cu_base_addr;
	exec->num_cu_masks    = ((exec->num_cus-1)>>5) + 1;

	if (!zdev->ert) {
		if (cfg->ert) {
			DRM_INFO("No ERT scheduler on MPSoC, using KDS\n");
		} else {
			SCHED_DEBUG("++ configuring penguin scheduler mode\n");
			exec->ops = &penguin_ops;
			exec->polling_mode = 1;
			exec->configured = 1;
		}
	} else {
		SCHED_DEBUG("++ configuring PS ERT mode\n");
		exec->ops = &ps_ert_ops;
		exec->polling_mode = cfg->polling;
		exec->cq_interrupt = cfg->cq_int;
		exec->cu_dma = cfg->cu_dma;
		exec->cu_isr = cfg->cu_isr;
		DRM_INFO("PS ERT feature: host_polling_mode(%d), cq_interrupt(%d), cu_dma(%d), cu_isr(%d)\n", exec->polling_mode, exec->cq_interrupt, exec->cu_dma, exec->cu_isr);
		setup_ert_hw(zdev);
		exec->configured = 1;
	}

	DRM_INFO("scheduler config ert(%d) slots(%d), cus(%d), cu_shift(%d), cu_base(0x%x), cu_masks(%d)\n", is_ert(cmd->ddev), exec->num_slots, exec->num_cus, exec->cu_shift_offset, exec->cu_base_addr, exec->num_cu_masks);
	return 0;
}

/**
 * set_cmd_state() - Set both internal and external state of a command
 *
 * The state is reflected externally through the command packet
 * as well as being captured in internal state variable
 *
 * @cmd: command object
 * @state: new state
 */
inline void
set_cmd_state(struct sched_cmd *cmd, enum cmd_state state)
{
	SCHED_DEBUG("-> set_cmd_state(,%d)\n", state);
	cmd->state = state;
	cmd->packet->state = state;
	SCHED_DEBUG("<- set_cmd_state\n");
}

/**
 * acquire_slot_idx() - Acquire a slot index if available.
 *                      Update slot status to busy so it cannot be reacquired.
 *
 * This function is called from scheduler thread
 *
 * Return: Command queue slot index, or -1 if none avaiable
 */
static int
acquire_slot_idx(struct drm_device *dev)
{
	struct drm_zocl_dev *zdev = dev->dev_private;
	unsigned int mask_idx = 0, slot_idx = -1;
	u32 mask;

	SCHED_DEBUG("-> acquire_slot_idx\n");
	for (mask_idx = 0; mask_idx < zdev->exec->num_slot_masks; ++mask_idx) {
		mask = zdev->exec->slot_status[mask_idx];
		slot_idx = ffz_or_neg_one(mask);
		if (slot_idx == -1 || slot_idx_from_mask_idx(slot_idx, mask_idx) >= zdev->exec->num_slots)
			continue;
		zdev->exec->slot_status[mask_idx] ^= (1<<slot_idx);
		SCHED_DEBUG("<- acquire_slot_idx returns %d\n", slot_idx_from_mask_idx(slot_idx, mask_idx));
		return slot_idx_from_mask_idx(slot_idx, mask_idx);
	}
	SCHED_DEBUG("<- acquire_slot_idx returns -1\n");
	return -1;
}

/**
 * release_slot_idx() - Release a slot index
 *
 * Update slot status mask for slot index. Notify scheduler in case
 * release is via ISR
 *
 * @dev: scheduler
 * @slot_idx: the slot index to release
 */
static void
release_slot_idx(struct drm_device *dev, unsigned int slot_idx)
{
	struct drm_zocl_dev *zdev = dev->dev_private;
	unsigned int mask_idx = slot_mask_idx(slot_idx);
	unsigned int pos = slot_idx_in_mask(slot_idx);

	SCHED_DEBUG("<-> release_slot_idx slot_status[%d]=0x%x, pos=%d\n", mask_idx, zdev->exec->slot_status[mask_idx], pos);
	zdev->exec->slot_status[mask_idx] ^= (1<<pos);
}

/**
 * get_cu_idx() - Get index of CU executing command at idx
 *
 * This function is called in polling mode only and
 * the command at cmd_idx is guaranteed to have been
 * started on a CU
 *
 * Return: Index of CU, or -1 on error
 */
inline unsigned int
get_cu_idx(struct drm_device *dev, unsigned int cmd_idx)
{
	struct drm_zocl_dev *zdev = dev->dev_private;
	struct sched_cmd *cmd = zdev->exec->submitted_cmds[cmd_idx];

	if (sched_error_on(zdev->exec, !cmd, "no submtted cmd"))
		return -1;
	return cmd->cu_idx;
}

/**
 * cu_done() - Check status of CU
 *
 * @cu_idx: Index of cu to check
 *
 * This function is called in polling mode only. The cu_idx
 * is guaranteed to have been started
 *
 * Return: %true if cu done, %false otherwise
 */
inline int
cu_done(struct drm_device *dev, unsigned int cu_idx)
{
	struct drm_zocl_dev *zdev = dev->dev_private;
	u32 *virt_addr = zdev->regs + cu_idx_to_offset(dev, cu_idx);

	SCHED_DEBUG("-> cu_done(,%d) checks cu at address 0x%p\n", cu_idx, virt_addr);
	/* done is indicated by AP_DONE(2) alone or by AP_DONE(2) | AP_IDLE(4)
	 * but not by AP_IDLE itself.  Since 0x10 | (0x10 | 0x100) = 0x110
	 * checking for 0x10 is sufficient.
	 */
	if (*virt_addr & 2) {
		unsigned int mask_idx = cu_mask_idx(cu_idx);
		unsigned int pos = cu_idx_in_mask(cu_idx);

		zdev->exec->cu_status[mask_idx] ^= 1<<pos;
		SCHED_DEBUG("<- cu_done returns 1\n");
		return true;
	}
	SCHED_DEBUG("<- cu_done returns 0\n");
	return false;
}

/**
 * ert_cu_done() - Check status of CU in ERT way
 *
 * @cu_idx: Index of cu to check
 *
 * This function is called in polling mode only. The cu_idx
 * is guaranteed to have been started
 *
 * Return: %true if cu done, %false otherwise
 */
inline int
ert_cu_done(struct drm_device *dev, unsigned int cu_idx)
{
	struct drm_zocl_dev *zdev = dev->dev_private;
	u32 *virt_addr = zdev->regs + cu_idx_to_offset(dev, cu_idx);

	SCHED_DEBUG("-> ert_cu_done(,%d) checks cu at address 0x%p\n", cu_idx, virt_addr);
	if (*virt_addr & 2) {
		unsigned int mask_idx = cu_mask_idx(cu_idx);
		unsigned int pos = cu_idx_in_mask(cu_idx);

		zdev->exec->cu_status[mask_idx] ^= 1<<pos;
		SCHED_DEBUG("<- ert_cu_done returns 1\n");
		return true;
	}
	SCHED_DEBUG("<- ert_cu_done returns 0\n");
	return false;
}

/**
 * notify_host() - Notify user space that a command is complete.
 */
static void
notify_host(struct sched_cmd *cmd)
{
	struct list_head *ptr;
	struct sched_client_ctx *entry;
	struct drm_zocl_dev *zdev = cmd->ddev->dev_private;
	unsigned long flags = 0;

	SCHED_DEBUG("-> notify_host\n");
	if (!zdev->ert) {
		/* now for each client update the trigger counter in the context */
		spin_lock_irqsave(&zdev->exec->ctx_list_lock, flags);
		list_for_each(ptr, &zdev->exec->ctx_list) {
			entry = list_entry(ptr, struct sched_client_ctx, link);
			atomic_inc(&entry->trigger);
		}
		spin_unlock_irqrestore(&zdev->exec->ctx_list_lock, flags);
		/* wake up all the clients */
		wake_up_interruptible(&zdev->exec->poll_wait_queue);
	} else {
		uint32_t cmd_mask_idx = slot_mask_idx(cmd->cq_slot_idx);
		uint32_t csr_offset = ERT_STATUS_REG + (cmd_mask_idx<<2);
		uint32_t pos = slot_idx_in_mask(cmd->cq_slot_idx);

		iowrite32(1<<pos, zdev->ert->hw_ioremap + csr_offset);
	}
	SCHED_DEBUG("<- notify_host\n");
}

/**
 * mark_cmd_complete() - Move a command to complete state
 *
 * Commands are marked complete in two ways
 *  1. Through polling of CUs or polling of MB status register
 *  2. Through interrupts from MB
 * In both cases, the completed commands are residing in the completed_cmds
 * list and the number of completed commands is reflected in num_completed.
 *
 * @xcmd: Command to mark complete
 *
 * The command is removed from the slot it occupies in the device command
 * queue. The slot is released so new commands can be submitted. The host
 * is notified that some command has completed.
 */
static void
mark_cmd_complete(struct sched_cmd *cmd)
{
	struct drm_zocl_dev *zdev = cmd->ddev->dev_private;

	SCHED_DEBUG("-> mark_cmd_complete(,%d)\n", cmd->slot_idx);
	zdev->exec->submitted_cmds[cmd->slot_idx] = NULL;
	set_cmd_state(cmd, CMD_STATE_COMPLETED);
	if (zdev->ert || zdev->exec->polling_mode)
		--cmd->sched->poll;
	release_slot_idx(cmd->ddev, cmd->slot_idx);
	notify_host(cmd);
	SCHED_DEBUG("<- mark_cmd_complete\n");
}

/**
 * get_free_sched_cmd() - Get a free command object
 *
 * Get from free/recycled list or allocate a new command if necessary.
 *
 * Return: Free command object
 */
static struct sched_cmd*
get_free_sched_cmd(void)
{
	struct sched_cmd *cmd;

	SCHED_DEBUG("-> get_free_sched_cmd\n");
	mutex_lock(&free_cmds_mutex);
	cmd = list_first_entry_or_null(&free_cmds, struct sched_cmd, list);
	if (cmd)
		list_del(&cmd->list);
	mutex_unlock(&free_cmds_mutex);
	if (!cmd)
		cmd = kmalloc(sizeof(struct sched_cmd), GFP_KERNEL);
	if (!cmd)
		return ERR_PTR(-ENOMEM);
	SCHED_DEBUG("<- get_free_sched_cmd %p\n", cmd);
	return cmd;
}

/**
 * zocl_gem_object_unref() - unreference a drm object
 *
 * @cmd: cmd owning the drm object to be unreference,
 *       it could be CMA or normal gem buffer
 *
 * Use the correct way to unreference a gem object
 *
 */
void zocl_gem_object_unref(struct sched_cmd *cmd)
{
	struct drm_zocl_dev *zdev = cmd->ddev->dev_private;
	struct drm_zocl_bo *bo = cmd->buffer;

	if (zdev->domain)
		drm_gem_object_unreference_unlocked(&bo->gem_base);
	else
		drm_gem_object_unreference_unlocked(&bo->cma_base.base);
}

/*
 * add_cmd() - Add a new command to the pending list
 *
 * @ddev: drm device owning adding the buffer object
 * @bo: buffer objects from user space from which new command is created
 *
 * Scheduler copies pending commands to its internal command queue.
 *
 * Return: 0 on success, -errno on failure
 */
static int
add_cmd(struct sched_cmd *cmd)
{
	int ret = 0;

	SCHED_DEBUG("-> add_cmd\n");
	cmd->cu_idx = -1;
	cmd->slot_idx = -1;
	DRM_INFO("packet header 0x%08x, data 0x%08x\n", cmd->packet->header, cmd->packet->data[0]);
	set_cmd_state(cmd, CMD_STATE_NEW);
	mutex_lock(&pending_cmds_mutex);
	list_add_tail(&cmd->list, &pending_cmds);
	mutex_unlock(&pending_cmds_mutex);

	/* wake scheduler */
	atomic_inc(&num_pending);
	wake_up_interruptible(&cmd->sched->wait_queue);

	SCHED_DEBUG("<- add_cmd\n");
	return ret;
}

/*
 * fill_cmd_by_gem_bo() - fill a command by gem buffer object
 *
 * @ddev: drm device owning adding the buffer object
 * @bo: buffer objects from user space from which new command is created
 *
 * Get a free scheduler command and initial it by gem buffer object.
 * After all, add this command to pending list.
 *
 * Return: 0 on success, -errno on failure
 */
static int
fill_cmd_by_gem_bo(struct drm_device *dev, struct drm_zocl_bo *bo)
{
	struct sched_cmd *cmd = get_free_sched_cmd();
	struct drm_zocl_dev *zdev = dev->dev_private;
	struct sched_packet *packet;
	int ret;

	SCHED_DEBUG("-> fill_cmd_by_bo\n");
	cmd->ddev = dev;
	cmd->sched = zdev->exec->scheduler;
	cmd->buffer = (void *)bo;
	if (zdev->domain)
		packet = (struct sched_packet *)bo->vmapping;
	else
		packet = (struct sched_packet *)bo->cma_base.vaddr;
	cmd->packet = packet;
	cmd->cq_slot_idx = 0;
	cmd->free_buffer = zocl_gem_object_unref;

	ret = add_cmd(cmd);

	SCHED_DEBUG("<- fill_cmd_by_bo\n");
	return ret;
}

/**
 * recycle_cmd() - recycle a command objects
 *
 * @cmd: command object to recycle
 *
 * Command object is added to the freelist
 *
 * Return: 0
 */
static int
recycle_cmd(struct sched_cmd *cmd)
{
	SCHED_DEBUG("recycle %p\n", cmd);
	mutex_lock(&free_cmds_mutex);
	list_move_tail(&cmd->list, &free_cmds);
	mutex_unlock(&free_cmds_mutex);
	return 0;
}

/**
 * delete_cmd_list() - reclaim memory for all allocated command objects
 */
static void
delete_cmd_list(void)
{
	struct sched_cmd *cmd;
	struct list_head *pos, *next;

	mutex_lock(&free_cmds_mutex);
	list_for_each_safe(pos, next, &free_cmds) {
		cmd = list_entry(pos, struct sched_cmd, list);
		list_del(pos);
		kfree(cmd);
	}
	mutex_unlock(&free_cmds_mutex);
}

/**
 * reset_exec() - Reset the scheduler
 *
 * @exec: Execution core (device) to reset
 *
 * Clear stale command object associated with execution core.
 * This can occur if the HW for some reason hangs.
 */
SCHED_UNUSED
static void
reset_exec(struct sched_exec_core *exec)
{
	struct list_head *pos, *next;
	struct drm_zocl_dev *zdev;
	struct sched_cmd *cmd;

	/* clear stale command objects if any */
	list_for_each_safe(pos, next, &pending_cmds) {
		cmd = list_entry(pos, struct sched_cmd, list);
		zdev = cmd->ddev->dev_private;
		if (zdev->exec != exec)
			continue;
		DRM_INFO("deleting stale pending cmd\n");
		cmd->free_buffer(cmd);
		recycle_cmd(cmd);
	}
	list_for_each_safe(pos, next, &global_scheduler0.command_queue) {
		cmd = list_entry(pos, struct sched_cmd, list);
		zdev = cmd->ddev->dev_private;
		if (zdev->exec != exec)
			continue;
		DRM_INFO("deleting stale scheduler cmd\n");
		cmd->free_buffer(cmd);
		recycle_cmd(cmd);
	}
}

/**
 * reset_all() - Reset the scheduler
 *
 * Clear stale command objects if any. This can occur if the HW for
 * some reason hangs.
 */
static void
reset_all(void)
{
	struct drm_zocl_dev *zdev;

	/* clear stale command object if any */
	while (!list_empty(&pending_cmds)) {
		struct sched_cmd *cmd = list_first_entry(&pending_cmds, struct sched_cmd, list);

		zdev = cmd->ddev->dev_private;
		DRM_INFO("deleting stale pending cmd\n");
		cmd->free_buffer(cmd);
		recycle_cmd(cmd);
	}
	while (!list_empty(&global_scheduler0.command_queue)) {
		struct sched_cmd *cmd = list_first_entry(&global_scheduler0.command_queue, struct sched_cmd, list);

		DRM_INFO("deleting stale scheduler cmd\n");
		cmd->free_buffer(cmd);
		recycle_cmd(cmd);
	}
}
/**
 * get_free_cu() - get index of first available CU per command cu mask
 *
 * @cmd: command containing CUs to check for availability
 *
 * This function is called kernel software scheduler mode only, in embedded
 * scheduler mode, the hardware scheduler handles the commands directly.
 *
 * Return: Index of free CU, -1 of no CU is available.
 */
static int
get_free_cu(struct sched_cmd *cmd)
{
	int mask_idx = 0;
	struct drm_zocl_dev *zdev = cmd->ddev->dev_private;

	SCHED_DEBUG("-> get_free_cu\n");
	for (mask_idx = 0; mask_idx < zdev->exec->num_cu_masks; ++mask_idx) {
		u32 cmd_mask = cmd->packet->data[mask_idx]; /* skip header */
		u32 busy_mask = zdev->exec->cu_status[mask_idx];
		int cu_idx = ffs_or_neg_one((cmd_mask | busy_mask) ^ busy_mask);

		if (cu_idx >= 0) {
			zdev->exec->cu_status[mask_idx] ^= 1 << cu_idx;
			SCHED_DEBUG("<- get_free_cu returns %d\n", cu_idx_from_mask(cu_idx, mask_idx));
			return cu_idx_from_mask(cu_idx, mask_idx);
		}
	}
	SCHED_DEBUG("<- get_free_cu returns -1\n");
	return -1;
}

/**
 * configure_cu() - transfer command register map to specified CU and
 *                  start the CU.
 *
 * @cmd: command with register map to transfer to CU
 * @cu_idx: index of CU to configure
 *
 * This function is called in kernel software scheduler mode only.
 */
static void
configure_cu(struct sched_cmd *cmd, int cu_idx)
{
	struct drm_zocl_dev *zdev = cmd->ddev->dev_private;
	u32 i, size = regmap_size(cmd);
	u32 *virt_addr = zdev->regs + cu_idx_to_offset(cmd->ddev, cu_idx);
	struct start_kernel_cmd *sk = (struct start_kernel_cmd *)cmd->packet;

	SCHED_DEBUG("-> configure_cu cu_idx=%d, cu_addr=0x%p, regmap_size=%d\n", cu_idx, virt_addr, size);
	/* write register map, but skip first word (AP_START) */
	/* can't get memcpy_toio to work */
	/* memcpy_toio(user_bar + cu_addr + 4, sk->data + sk->extra_cu_masks + 1, (size-1)*4);
	 */
	for (i = 1; i < size; ++i)
		virt_addr[i] = *(sk->data + sk->extra_cu_masks + i);

	/* start CU at base + 0x0 */
	virt_addr[0] = 0x1;

	SCHED_DEBUG("<- configure_cu\n");
}

/**
 * ert_configure_cu() - transfer command register map to specified CU and start the CU.
 *
 * @cmd: command with register map to transfer to CU
 * @cu_idx: index of CU to configure
 *
 * This function is called in kernel software scheduler mode only.
 */
static void
ert_configure_cu(struct sched_cmd *cmd, int cu_idx)
{
	struct drm_zocl_dev *zdev = cmd->ddev->dev_private;
	u32 i, size = regmap_size(cmd);
	u32 *virt_addr = zdev->regs + cu_idx_to_offset(cmd->ddev, cu_idx);
	struct start_kernel_cmd *sk = (struct start_kernel_cmd *)cmd->packet;

	SCHED_DEBUG("-> ert_configure_cu cu_idx=%d, cu_addr=0x%p, regmap_size=%d\n", cu_idx, virt_addr, size);

	for (i = 1; i < size; ++i)
		virt_addr[i] = *(sk->data + sk->extra_cu_masks + i);

	/* start CU at base + 0x0 */
	virt_addr[0] = 0x1;

	SCHED_DEBUG("<- ert_configure_cu\n");
}

/**
 * queued_to_running() - Move a command from queued to running state if possible
 *
 * @cmd: Command to start
 *
 * Upon success, the command is not necessarily running. In ert mode the
 * command will have been submitted to the embedded scheduler, whereas in
 * penguin mode the command has been started on a CU.
 *
 * Return: %true if command was submitted to device, %false otherwise
 */
static int
queued_to_running(struct sched_cmd *cmd)
{
	struct drm_zocl_dev *zdev = cmd->ddev->dev_private;
	int retval = false;

	SCHED_DEBUG("-> queued_to_running\n");
	if (opcode(cmd) == OP_CONFIGURE)
		configure(cmd);

	if (zdev->exec->ops->submit(cmd)) {
		set_cmd_int_state(cmd, CMD_STATE_RUNNING);
		if (zdev->ert || zdev->exec->polling_mode)
			++cmd->sched->poll;
		zdev->exec->submitted_cmds[cmd->slot_idx] = cmd;
		retval = true;
	}
	SCHED_DEBUG("<- queued_to_running returns %d\n", retval);

	return retval;
}

/**
 * running_to_complete() - Check status of running commands
 *
 * @cmd: Command is in running state
 *
 * If a command is found to be complete, it marked complete prior to return
 * from this function.
 */
static void
running_to_complete(struct sched_cmd *cmd)
{
	struct drm_zocl_dev *zdev = cmd->ddev->dev_private;

	SCHED_DEBUG("-> running_to_complete\n");
	zdev->exec->ops->query(cmd);
	SCHED_DEBUG("<- running_to_complete\n");
}

/**
 * complete_to_free() - Recycle a complete command objects
 *
 * @xcmd: Command is in complete state
 */
static void
complete_to_free(struct sched_cmd *cmd)
{
	SCHED_DEBUG("-> complete_to_free\n");
	cmd->free_buffer(cmd);
	recycle_cmd(cmd);
	SCHED_DEBUG("<- complete_to_free\n");
}


/**
 * scheduler_queue_cmds() - Queue any pending commands
 *
 * The scheduler copies pending commands to its internal command queue where
 * is is now in queued state.
 */
static void
scheduler_queue_cmds(struct scheduler *sched)
{
	struct sched_cmd *cmd;
	struct list_head *pos, *next;

	SCHED_DEBUG("-> scheduler_queue_cmds\n");
	mutex_lock(&pending_cmds_mutex);
	list_for_each_safe(pos, next, &pending_cmds) {
		cmd = list_entry(pos, struct sched_cmd, list);
		if (cmd->sched != sched)
			continue;
		list_del(&cmd->list);
		list_add_tail(&cmd->list, &sched->command_queue);
		set_cmd_int_state(cmd, CMD_STATE_QUEUED);
		atomic_dec(&num_pending);
	}
	mutex_unlock(&pending_cmds_mutex);
	SCHED_DEBUG("<- scheduler_queue_cmds\n");
}

/**
 * scheduler_iterator_cmds() - Iterate all commands in scheduler command queue
 */
static void
scheduler_iterate_cmds(struct scheduler *sched)
{
	struct sched_cmd *cmd;
	struct list_head *pos, *next;

	SCHED_DEBUG("-> scheduler_iterate_cmds\n");
	list_for_each_safe(pos, next, &sched->command_queue) {
		cmd = list_entry(pos, struct sched_cmd, list);

		if (cmd->state == CMD_STATE_QUEUED)
			queued_to_running(cmd);
		if (cmd->state == CMD_STATE_RUNNING)
			running_to_complete(cmd);
		if (cmd->state == CMD_STATE_COMPLETED)
			complete_to_free(cmd);
	}
	SCHED_DEBUG("<- scheduler_iterate_cmds\n");
}

/**
 * scheduler_wait_condition() - Check status of scheduler wait condition
 *
 * Scheduler must wait (sleep) if
 *  1. there are no pending commands
 *  2. no pending interrupt from embedded scheduler
 *  3. no pending complete commands in polling mode
 *
 * Return: 1 if scheduler must wait, 0 othewise
 */
static int
scheduler_wait_condition(struct scheduler *sched)
{
	if (kthread_should_stop() || sched->error) {
		sched->stop = 1;
		SCHED_DEBUG("scheduler wakes kthread_should_stop\n");
		return 0;
	}

	if (atomic_read(&num_pending)) {
		SCHED_DEBUG("scheduler wakes to copy new pending commands\n");
		return 0;
	}

	if (sched->poll) {
		SCHED_DEBUG("scheduler wakes to poll\n");
		return 0;
	}

	SCHED_DEBUG("scheduler waits ...\n");
	return 1;
}

/**
 * scheduler_wait() - check if scheduler should wait
 *
 * See scheduler_wait_condition().
 */
static void
scheduler_wait(struct scheduler *sched)
{
	wait_event_interruptible(sched->wait_queue, scheduler_wait_condition(sched) == 0);
}

/**
 * scheduler_loop() - Run one loop of the scheduler
 */
	static void
scheduler_loop(struct scheduler *sched)
{
	SCHED_DEBUG("scheduler_loop\n");

	scheduler_wait(sched);

	if (sched->stop) {
		if (sched->error)
			DRM_INFO("scheduler encountered unexpected error and exits\n");
		return;
	}

	/* queue new pending commands */
	scheduler_queue_cmds(sched);

	/* iterate all commands */
	scheduler_iterate_cmds(sched);

	if (sched_loop_cnt < MAX_SCHED_LOOP)
		sched_loop_cnt++;
	else {
		sched_loop_cnt = 0;
		schedule();
	}
}

/**
 * scheduler() - Command scheduler thread routine
 */
static int
scheduler(void *data)
{
	struct scheduler *sched = (struct scheduler *)data;

	while (!sched->stop)
		scheduler_loop(sched);
	DRM_DEBUG("%s scheduler thread exits with value %d\n", __func__, sched->error);
	return sched->error;
}

/**
 * init_scheduler_thread() - Initialize scheduler thread if necessary
 *
 * Return: 0 on success, -errno otherwise
 */
static int
init_scheduler_thread(void)
{
	SCHED_DEBUG("init_scheduler_thread use_count=%d\n", global_scheduler0.use_count);
	if (global_scheduler0.use_count++)
		return 0;

	init_waitqueue_head(&global_scheduler0.wait_queue);
	global_scheduler0.error = 0;
	global_scheduler0.stop = 0;
	sched_loop_cnt = 0;

	INIT_LIST_HEAD(&global_scheduler0.command_queue);
	global_scheduler0.poll = 0;

	global_scheduler0.scheduler_thread = kthread_run(scheduler, (void *)&global_scheduler0, "zocl-scheduler-thread0");
	if (IS_ERR(global_scheduler0.scheduler_thread)) {
		int ret = PTR_ERR(global_scheduler0.scheduler_thread);

		DRM_ERROR(__func__);
		return ret;
	}
	return 0;
}

/**
 * fini_scheduler_thread() - Finalize scheduler thread if unused
 *
 * Return: 0 on success, -errno otherwise
 */
static int
fini_scheduler_thread(void)
{
	int retval = 0;

	SCHED_DEBUG("fini_scheduler_thread use_count=%d\n", global_scheduler0.use_count);
	if (--global_scheduler0.use_count)
		return 0;

	retval = kthread_stop(global_scheduler0.scheduler_thread);

	/* clear stale command objects if any */
	reset_all();

	/* reclaim memory for allocate command objects */
	delete_cmd_list();

	return retval;
}

/**
 * penguin_query() - Check command status of argument command
 *
 * @cmd: Command to check
 *
 * Function is called in penguin mode (no embedded scheduler).
 */
static void
penguin_query(struct sched_cmd *cmd)
{
	u32 opc = opcode(cmd);

	SCHED_DEBUG("-> penguin_queury() slot_idx=%d\n", cmd->slot_idx);
	if (opc == OP_CONFIGURE || (opc == OP_START_CU && cu_done(cmd->ddev, get_cu_idx(cmd->ddev, cmd->slot_idx))))
		mark_cmd_complete(cmd);
	SCHED_DEBUG("<- penguin_queury\n");
}

/**
 * penguin_submit() - penguin submit of a command
 *
 * @cmd: command to submit
 *
 * Special processing for configure command. Configuration itself is
 * done/called by queued_to_running before calling penguin_submit. In penguin
 * mode configuration need to ensure that the command is retired properly by
 * scheduler, so assign it a slot index and let normal flow continue.
 *
 * Return: %true on successful submit, %false otherwise
 */
static int
penguin_submit(struct sched_cmd *cmd)
{
	SCHED_DEBUG("-> penguin_submit\n");

	/* configuration was done by submit_cmds, ensure the cmd retired properly */
	if (opcode(cmd) == OP_CONFIGURE) {
		cmd->slot_idx = acquire_slot_idx(cmd->ddev);
		SCHED_DEBUG("<- penguin_submit (configure)\n");
		return true;
	}

	if (opcode(cmd) != OP_START_CU)
		return false;

	/* extract cu list */
	cmd->cu_idx = get_free_cu(cmd);
	if (cmd->cu_idx < 0)
		return false;

	cmd->slot_idx = acquire_slot_idx(cmd->ddev);
	if (cmd->slot_idx < 0)
		return false;

	/* found free cu, transfer regmap and start it */
	configure_cu(cmd, cmd->cu_idx);

	SCHED_DEBUG("<- penguin_submit cu_idx=%d slot=%d\n", cmd->cu_idx, cmd->slot_idx);

	return true;
}

/**
 * penguin_ops: operations for kernel mode scheduling
 */
static struct sched_ops penguin_ops = {
	.submit = penguin_submit,
	.query = penguin_query,
};

/**
 * ps_ert_query() - Check command status of argument command
 *
 * @cmd: Command to check
 *
 * Function is called in penguin mode (no embedded scheduler).
 */
static void
ps_ert_query(struct sched_cmd *cmd)
{
	u32 opc = opcode(cmd);

	SCHED_DEBUG("-> ps_ert_queury() slot_idx=%d\n", cmd->slot_idx);
	if (opc == OP_CONFIGURE || (opc == OP_START_CU && ert_cu_done(cmd->ddev, get_cu_idx(cmd->ddev, cmd->slot_idx))))
		mark_cmd_complete(cmd);
	SCHED_DEBUG("<- ps_ert_queury\n");
}

/**
 * ps_ert_submit() - PS ERT submit of a command
 *
 * @cmd: command to submit
 *
 * Special processing for configure command. Configuration itself is
 * done/called by queued_to_running before calling penguin_submit. In penguin
 * mode configuration need to ensure that the command is retired properly by
 * scheduler, so assign it a slot index and let normal flow continue.
 *
 * Return: %true on successful submit, %false otherwise
 */
static int
ps_ert_submit(struct sched_cmd *cmd)
{
	SCHED_DEBUG("-> ps_ert_submit()\n");
	/* configuration was done by submit_cmds, ensure the cmd retired properly */
	if (opcode(cmd) == OP_CONFIGURE) {
		cmd->slot_idx = acquire_slot_idx(cmd->ddev);
		SCHED_DEBUG("<- ps_ert_submit (configure)\n");
		return true;
	}

	if (opcode(cmd) != OP_START_CU)
		return false;

	/* extract cu list */
	cmd->cu_idx = get_free_cu(cmd);
	if (cmd->cu_idx < 0)
		return false;

	cmd->slot_idx = acquire_slot_idx(cmd->ddev);
	if (cmd->slot_idx < 0)
		return false;

	/* found free cu, transfer regmap and start it */
	ert_configure_cu(cmd, cmd->cu_idx);

	SCHED_DEBUG("<- ps_ert_submit() cu_idx=%d slot=%d cq_slot=%d\n", cmd->cu_idx, cmd->slot_idx, cmd->cq_slot_idx);
	return true;
}

/**
 * ps_ert_ops: operations for ps scheduling
 */
static struct sched_ops ps_ert_ops = {
	.submit = ps_ert_submit,
	.query = ps_ert_query,
};

/**
 * zocl_execbuf_ioctl() - Entry point for exec buffer.
 *
 * @dev: Device node calling execbuf
 * @data: Payload
 * @filp:
 *
 * Function adds exec buffer to the pending list of commands
 *
 * Return: 0 on success, -errno otherwise
 */
int zocl_execbuf_ioctl(struct drm_device *dev, void *data, struct drm_file *filp)
{
	struct drm_gem_object *gem_obj;
	struct drm_zocl_bo *zocl_bo;
	struct drm_zocl_dev *zdev = dev->dev_private;
	struct drm_zocl_execbuf *args = data;
	int ret = 0;

	SCHED_DEBUG("-> zocl_execbuf_ioctl\n");
	gem_obj = zocl_gem_object_lookup(dev, filp, args->exec_bo_handle);
	if (!gem_obj) {
		DRM_ERROR("Failed to look up GEM BO %d\n", args->exec_bo_handle);
		return -EINVAL;
	}

	zocl_bo = to_zocl_bo(gem_obj);
	if (!zocl_bo_execbuf(zocl_bo)) {
		ret = -EINVAL;
		goto out;
	}

	if (fill_cmd_by_gem_bo(dev, zocl_bo)) {
		ret = -EINVAL;
		goto out;
	}

	SCHED_DEBUG("<- zocl_execbuf_ioctl\n");
	return ret;

out:
	if (zdev->domain)
		drm_gem_cma_free_object(&zocl_bo->gem_base);
	else
		drm_gem_cma_free_object(&zocl_bo->cma_base.base);
	return ret;
}

struct sched_packet *get_next_packet(struct sched_packet *packet, unsigned int size)
{
	char *bytes = (char *)packet;

	return (struct sched_packet *)(bytes+size);
}

void zocl_cmd_buffer_free(struct sched_cmd *cmd)
{
	SCHED_DEBUG("-> zocl_cmd_buffer_free");
	kfree(cmd->buffer);
	SCHED_DEBUG("<- zocl_cmd_buffer_free");
}

static unsigned int
get_packet_size(struct sched_packet *packet)
{
	unsigned int payload;

	SCHED_DEBUG("-> get_packet_size");
	switch (packet->opcode)
	case OP_CONFIGURE:
		SCHED_DEBUG("configure cmd");
		payload = 5 + packet->count;
		break;
	case OP_START_CU:
		SCHED_DEBUG("start CU/Kernel cmd");
		payload = packet->count;
		break;
	case OP_STOP:
	case OP_ABORT:
		SCHED_DEBUG("abort or stop cmd");
	default:
		payload = 0;

	SCHED_DEBUG("<- get_packet_size");
	return 1 + payload;
}

/*
 * fill_cmd_by_buffer() - fill a command by buffer
 *
 * @ddev: drm device owning adding the buffer object
 * @buffer: buffer
 * @cq_idx: index of the CQ slot in BRAM
 *
 * Get a free scheduler command and initial it by gem buffer object.
 * After all, add this command to pending list.
 *
 * Return: 0 on success, -errno on failure
 */
static int
fill_cmd_by_buffer(struct drm_device *dev, void *buffer, unsigned int cq_idx)
{
	struct sched_cmd *cmd = get_free_sched_cmd();
	struct drm_zocl_dev *zdev = dev->dev_private;
	int ret;

	SCHED_DEBUG("-> fill_cmd_by_buffer\n");
	cmd->ddev = dev;
	cmd->sched = zdev->exec->scheduler;
	cmd->buffer = buffer;
	cmd->packet = buffer;
	cmd->cq_slot_idx = cq_idx;
	cmd->free_buffer = zocl_cmd_buffer_free;

	ret = add_cmd(cmd);

	SCHED_DEBUG("<- fill_cmd_by_buffer\n");
	return ret;
}

/**
 * cq_check() - Check CQ status and submit new command to KDS
 *
 * @data: drm zocl device
 *
 * Iterate CQ BRAM for new command.
 *
 * Return: 0 on success, -errno on failure
 */
static int
cq_check(void *data)
{
	struct drm_zocl_dev *zdev = data;
	struct sched_exec_core *exec_core = zdev->exec;
	struct zocl_ert_dev *ert = zdev->ert;
	struct sched_packet *packet;
	void *buffer;
	unsigned int slot_idx;
	int ret;

	SCHED_DEBUG("-> cq_check");
	while (!kthread_should_stop() && !exec_core->cq_interrupt) {
		packet = ert->cq_ioremap;
		for (slot_idx = 0; slot_idx < exec_core->num_slots; slot_idx++) {
			if (packet->state == CMD_STATE_NEW) {
				packet->state = CMD_STATE_QUEUED;
				SCHED_DEBUG("packet header 0x%8x, packet addr 0x%p slot size %d", packet->header, packet, slot_size(zdev->ddev));
				buffer = kzalloc(slot_size(zdev->ddev), GFP_KERNEL);
				if (!buffer)
					return -ENOMEM;
				SCHED_DEBUG("packet size in words %d", get_packet_size(packet));
				/**
				 * In 2018.2, CQ BRAM is used. Access PL by AXI lite is expense,
				 * so copy packet to PS DDR. If host can directly submit command
				 * to PS DDR, we don't need this copy.
				 */
				memcpy_fromio(buffer, packet, get_packet_size(packet)*sizeof(uint32_t));
				if (fill_cmd_by_buffer(zdev->ddev, buffer, slot_idx)) {
					ret = -EINVAL;
					goto err;
				}
			}
			packet = get_next_packet(packet, slot_size(zdev->ddev));
		}
		schedule();
	}
	SCHED_DEBUG("<- cq_check");
	return 0;
err:
	kfree(buffer);
	return ret;
}

/**
 * sched_init_exec() - Initialize the command execution for device
 *
 * @drm: Device node to initialize
 *
 * Return: 0 on success, -errno otherwise
 */
int
sched_init_exec(struct drm_device *drm)
{
	struct sched_exec_core *exec_core;
	struct drm_zocl_dev *zdev = drm->dev_private;
	unsigned int i;

	SCHED_DEBUG("-> sched_init_exec\n");
	exec_core = devm_kzalloc(drm->dev, sizeof(*exec_core), GFP_KERNEL);
	if (!exec_core)
		return -ENOMEM;

	zdev->exec = exec_core;
	spin_lock_init(&exec_core->ctx_list_lock);
	INIT_LIST_HEAD(&exec_core->ctx_list);
	init_waitqueue_head(&exec_core->poll_wait_queue);

	exec_core->scheduler = &global_scheduler0;
	exec_core->base = zdev->regs;
	exec_core->num_slots = 16;
	exec_core->num_cus = 0;
	exec_core->cu_base_addr = 0;
	exec_core->cu_shift_offset = 0;
	exec_core->polling_mode = 1;
	exec_core->cq_interrupt = 0;
	exec_core->cu_isr = 0;
	exec_core->cu_dma = 0;
	exec_core->num_slot_masks = 1;
	exec_core->num_cu_masks = 0;
	exec_core->ops = &penguin_ops;

	for (i = 0; i < MAX_SLOTS; ++i)
		exec_core->submitted_cmds[i] = NULL;

	for (i = 0; i < MAX_U32_SLOT_MASKS; ++i)
		exec_core->slot_status[i] = 0;

	for (i = 0; i < MAX_U32_CU_MASKS; ++i)
		exec_core->cu_status[i] = 0;

	init_scheduler_thread();

	if (zdev->ert) {
		exec_core->hw_cq_check = kthread_run(cq_check, (void *)zdev, "zocl-ert-thread");
	}

	SCHED_DEBUG("<- sched_init_exec\n");
	return 0;
}

/**
 * sched_fini_exec() - Finalize the command execution for device
 *
 * @xdev: Device node to finalize
 *
 * Return: 0 on success, -errno otherwise
 */
int sched_fini_exec(struct drm_device *drm)
{
	SCHED_DEBUG("-> sched_fini_exec\n");
	kthread_stop(((struct drm_zocl_dev *)drm->dev_private)->exec->hw_cq_check);
	fini_scheduler_thread();
	SCHED_DEBUG("<- sched_fini_exec\n");

	return 0;
}

void zocl_track_ctx(struct drm_device *dev, struct sched_client_ctx *fpriv)
{
	unsigned long flags;
	struct drm_zocl_dev *zdev = dev->dev_private;

	spin_lock_irqsave(&zdev->exec->ctx_list_lock, flags);
	list_add_tail(&fpriv->link, &zdev->exec->ctx_list);
	spin_unlock_irqrestore(&zdev->exec->ctx_list_lock, flags);
}

void zocl_untrack_ctx(struct drm_device *dev, struct sched_client_ctx *fpriv)
{
	unsigned long flags;
	struct drm_zocl_dev *zdev = dev->dev_private;

	spin_lock_irqsave(&zdev->exec->ctx_list_lock, flags);
	list_del(&fpriv->link);
	spin_unlock_irqrestore(&zdev->exec->ctx_list_lock, flags);
}
