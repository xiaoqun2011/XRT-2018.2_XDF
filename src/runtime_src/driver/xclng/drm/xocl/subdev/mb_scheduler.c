/*
 * A GEM style device manager for PCIe based OpenCL accelerators.
 *
 * Copyright (C) 2017 Xilinx, Inc. All rights reserved.
 *
 * Authors:
 *    Soren Soe <soren.soe@xilinx.com>
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
#include <linux/eventfd.h>
#include <linux/kthread.h>
#include <ert.h>
#include "../xocl_drv.h"
#include "../userpf/common.h"

//#define SCHED_VERBOSE

#if defined(__GNUC__)
#define SCHED_UNUSED __attribute__((unused))
#endif

#define sched_error_on(exec,expr,msg)		                  \
({		                                                          \
	unsigned int ret = 0;                                             \
	if ((expr)) {						          \
		xocl_err(&exec->pdev->dev, "Assertion failed %s %s",#expr,msg);\
		exec->scheduler->error=1;                                       \
		ret = 1; 					          \
	}                                                                 \
	(ret);                                                            \
})

#define sched_debug_packet(packet,size)				     \
({		                                                     \
	int i;							     \
	u32* data = (u32*)packet;                                    \
	for (i=0; i<size; ++i)			    	             \
		DRM_INFO("packet(0x%p) data[%d] = 0x%x\n",data,i,data[i]); \
})

#ifdef SCHED_VERBOSE
# define SCHED_DEBUG(msg) DRM_INFO(msg)
# define SCHED_DEBUGF(format,...) DRM_INFO(format, ##__VA_ARGS__)
# define SCHED_DEBUG_PACKET(packet,size) sched_debug_packet(packet,size)
#else
# define SCHED_DEBUG(msg)
# define SCHED_DEBUGF(format,...)
# define SCHED_DEBUG_PACKET(packet,size)
#endif

/* Scheduler call schedule() every MAX_SCHED_LOOP loop*/
#define MAX_SCHED_LOOP 8
static int    sched_loop_cnt;

/* Forward declaration */
struct exec_core;
struct sched_ops;
struct xocl_sched;

static bool queued_to_running(struct xocl_cmd *xcmd);

/**
 * struct exec_core: Core data structure for command execution on a device
 *
 * @ctx_list: Context list populated with device context
 * @poll_wait_queue: Wait queue for device polling
 * @scheduler: Command queue scheduler
 * @submitted_cmds: Tracking of command submitted for execution on this device
 * @num_slots: Number of command queue slots
 * @num_cus: Number of CUs in loaded program
 * @cu_shift_offset: CU idx to CU address shift value
 * @cu_base_addr: Base address of CU address space
 * @polling_mode: If set then poll for command completion
 * @cq_interrupt: If set then trigger interrupt to MB on new commands
 * @configured: Flag to indicate that the core data structure has been initialized
 * @slot_status: Bitmap to track status (busy(1)/free(0)) slots in command queue
 * @num_slot_masks: Number of slots status masks used (computed from @num_slots)
 * @cu_status: Bitmap to track status (busy(1)/free(0)) of CUs. Unused in ERT mode.
 * @num_cu_masks: Number of CU masks used (computed from @num_cus)
 * @sr0: If set, then status register [0..31] is pending with completed commands (ERT only).
 * @sr1: If set, then status register [32..63] is pending with completed commands (ERT only).
 * @sr2: If set, then status register [64..95] is pending with completed commands (ERT only).
 * @sr3: If set, then status register [96..127] is pending with completed commands (ERT only).
 * @ops: Scheduler operations vtable
 */
struct exec_core {
	struct platform_device    *pdev;

	void __iomem		  *base;
	u32			  intr_base;
	u32			  intr_num;

	wait_queue_head_t          poll_wait_queue;

	struct xocl_sched          *scheduler;

	struct xocl_cmd            *submitted_cmds[MAX_SLOTS];

        unsigned int               num_slots;
        unsigned int               num_cus;
        unsigned int               cu_shift_offset;
        u32                        cu_base_addr;
        unsigned int               polling_mode;
        unsigned int               cq_interrupt;
        unsigned int               configured;

	u32                        cu_addr_map[MAX_CUS];

        /* Bitmap tracks busy(1)/free(0) slots in cmd_slots*/
        u32                        slot_status[MAX_U32_SLOT_MASKS];
        unsigned int               num_slot_masks; /* ((num_slots-1)>>5)+1 */

        u32                        cu_status[MAX_U32_CU_MASKS];
        unsigned int               num_cu_masks; /* ((num_cus-1)>>5+1 */

	/* Status register pending complete.  Written by ISR, cleared
	   by scheduler */
	atomic_t                   sr0;
	atomic_t                   sr1;
	atomic_t                   sr2;
	atomic_t                   sr3;

	/* Operations for dynamic indirection dependt on MB or kernel scheduler */
	struct sched_ops	   *ops;
};

/**
 * exec_get_pdev() -
 */
inline struct platform_device *
exec_get_pdev(struct exec_core *exec)
{
	return exec->pdev;
}

/**
 * exec_get_xdev() -
 */
inline struct xocl_dev *
exec_get_xdev(struct exec_core *exec)
{
	return xocl_get_xdev(exec->pdev);
}

/**
 * struct xocl_sched: scheduler for xocl_cmd objects
 *
 * @scheduler_thread: thread associated with this scheduler
 * @use_count: use count for this scheduler
 * @wait_queue: conditional wait queue for scheduler thread
 * @error: set to 1 to indicate scheduler error
 * @stop: set to 1 to indicate scheduler should stop
 * @command_queue: list of command objects managed by scheduler
 * @intc: boolean flag set when there is a pending interrupt for command completion
 * @poll: number of running commands in polling mode
 */
struct xocl_sched
{
        struct task_struct        *scheduler_thread;
        unsigned int               use_count;

        wait_queue_head_t          wait_queue;
        unsigned int               error;
        unsigned int               stop;

        struct list_head           command_queue;
        atomic_t                   intc; /* pending interrupt shared with isr */
        unsigned int               poll; /* number of cmds to poll */
};

static struct xocl_sched global_scheduler0;

/**
 * Command data used by scheduler
 *
 * @list: command object moves from list to list
 * @bo: underlying drm buffer object
 * @exec: execution device associated with this command
 * @client: client (user process) context that created this command
 * @xs: command scheduler responsible for schedulint this command
 * @state: state of command object per scheduling
 * @id: unique id for an active command object
 * @cu_idx: index of CU executing this cmd object; used in penguin mode only
 * @slot_idx: command queue index of this command object
 * @wait_count: number of commands that must trigger this command before it can start
 * @chain_count: number of commands that this command must trigger when it completes
 * @chain: list of commands to trigger upon completion; maximum chain depth is 8
 * @deps: list of commands this object depends on, converted to chain when command is queued
 * @packet: mapped ert packet object from user space
 */
struct xocl_cmd
{
	struct list_head list;
	struct drm_xocl_bo *bo;
	struct exec_core *exec;
	struct client_ctx* client;
	struct xocl_sched *xs;
	enum ert_cmd_state state;
	unsigned long id;
	int cu_idx; /* running cu, initialized to -1 */
	int slot_idx;

	/* dependency handling */
	unsigned int chain_count;
	unsigned int wait_count;
	union {
		struct xocl_cmd *chain[8];
		struct drm_xocl_bo *deps[8];
	};

	/* The actual cmd object representation */
	struct ert_packet *packet;
};

/**
 * struct xocl_sched_ops: scheduler specific operations
 *
 * Scheduler can operate in MicroBlaze mode (mb/ert) or in penguin mode. This
 * struct differentiates specific operations.  The struct is per device node,
 * meaning that one device can operate in ert mode while another can operate in
 * penguin mode.
 */
struct sched_ops
{
	bool (*submit) (struct xocl_cmd *xcmd);
	void (*query)  (struct xocl_cmd *xcmd);
};

static struct sched_ops mb_ops;
static struct sched_ops penguin_ops;

/**
 * opcode() - Command opcode
 *
 * @cmd: Command object
 * Return: Opcode per command packet
 */
inline u32
opcode(struct xocl_cmd* xcmd)
{
	return xcmd->packet->opcode;
}

/**
 * type() - Command type
 *
 * @cmd: Command object
 * Return: Type of command
 */
inline u32
type(struct xocl_cmd* xcmd)
{
	return xcmd->packet->type;
}

/**
 * payload_size() - Command payload size
 *
 * @xcmd: Command object
 * Return: Size in number of words of command packet payload
 */
inline u32
payload_size(struct xocl_cmd *xcmd)
{
	return xcmd->packet->count;
}

/**
 * packet_size() - Command packet size
 *
 * @xcmd: Command object
 * Return: Size in number of words of command packet
 */
inline u32
packet_size(struct xocl_cmd *xcmd)
{
	return payload_size(xcmd) + 1;
}

/**
 * cu_masks() - Number of command packet cu_masks
 *
 * @xcmd: Command object
 * Return: Total number of CU masks in command packet
 */
inline u32
cu_masks(struct xocl_cmd *xcmd)
{
	struct ert_start_kernel_cmd *sk;
	if (opcode(xcmd)!=ERT_START_KERNEL)
		return 0;
	sk = (struct ert_start_kernel_cmd *)xcmd->packet;
	return 1 + sk->extra_cu_masks;
}

/**
 * regmap_size() - Size of regmap is payload size (n) minus the number of cu_masks
 *
 * @xcmd: Command object
 * Return: Size of register map in number of words
 */
inline u32
regmap_size(struct xocl_cmd* xcmd)
{
	return payload_size(xcmd) - cu_masks(xcmd);
}

/**
 * cmd_get_xdev() -
 */
inline struct xocl_dev *
cmd_get_xdev(struct xocl_cmd *xcmd)
{
	return exec_get_xdev(xcmd->exec);
}

/**
 * set_cmd_int_state() - Set internal command state used by scheduler only
 *
 * @xcmd: command to change internal state on
 * @state: new command state per ert.h
 */
inline void
set_cmd_int_state(struct xocl_cmd* xcmd, enum ert_cmd_state state)
{
        SCHED_DEBUGF("-> set_cmd_int_state(%lu,%d)\n",xcmd->id,state);
        xcmd->state = state;
        SCHED_DEBUG("<- set_cmd_int_state\n");
}

/**
 * set_cmd_state() - Set both internal and external state of a command
 *
 * The state is reflected externally through the command packet
 * as well as being captured in internal state variable
 *
 * @xcmd: command object
 * @state: new state
 */
inline void
set_cmd_state(struct xocl_cmd* xcmd, enum ert_cmd_state state)
{
        SCHED_DEBUGF("->set_cmd_state(%lu,%d)\n",xcmd->id,state);
        xcmd->state = state;
        xcmd->packet->state = state;
        SCHED_DEBUG("<-set_cmd_state\n");
}

inline enum ert_cmd_state
update_cmd_state(struct xocl_cmd *xcmd)
{
	if (xcmd->state!=ERT_CMD_STATE_RUNNING && atomic_read(&xcmd->client->abort))
		set_cmd_state(xcmd,ERT_CMD_STATE_ABORT);
	return xcmd->state;
}

/**
 * List of free xocl_cmd objects.
 *
 * @free_cmds: populated with recycled xocl_cmd objects
 * @cmd_mutex: mutex lock for cmd_list
 *
 * Command objects are recycled for later use and only freed when kernel
 * module is unloaded.
 */
static LIST_HEAD(free_cmds);
static DEFINE_MUTEX(free_cmds_mutex);

/**
 * List of new pending xocl_cmd objects
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
 * get_free_xocl_cmd() - Get a free command object
 *
 * Get from free/recycled list or allocate a new command if necessary.
 *
 * Return: Free command object
 */
static struct xocl_cmd*
get_free_xocl_cmd(void)
{
	struct xocl_cmd* cmd;
	static unsigned long count=0;
	SCHED_DEBUG("-> get_free_xocl_cmd\n");
	mutex_lock(&free_cmds_mutex);
	cmd=list_first_entry_or_null(&free_cmds,struct xocl_cmd,list);
	if (cmd)
		list_del(&cmd->list);
        mutex_unlock(&free_cmds_mutex);
	if (!cmd)
		cmd = kmalloc(sizeof(struct xocl_cmd),GFP_KERNEL);
	if (!cmd)
		return ERR_PTR(-ENOMEM);
	cmd->id = count++;
	SCHED_DEBUGF("<- get_free_xocl_cmd %lu %p\n",cmd->id,cmd);
	return cmd;
}

/**
 * add_cmd() - Add a new command to pending list
 *
 * @exec: Targeted device
 * @bo: Buffer objects from user space from which new command is created
 * @numdeps: Number of dependencies for this command
 * @deps: List of @numdeps dependencies
 *
 * Scheduler copies pending commands to its internal command queue.
 *
 * Return: 0 on success, -errno on failure
 */
static int
add_cmd(struct exec_core *exec, struct client_ctx* client, struct drm_xocl_bo* bo, int numdeps, struct drm_xocl_bo **deps)
{
	struct platform_device *pdev=exec->pdev;
	struct xocl_dev *xdev = xocl_get_xdev(pdev);
	struct xocl_cmd *xcmd = get_free_xocl_cmd();
	SCHED_DEBUGF("-> add_cmd(%lu)\n",xcmd->id);
	xcmd->bo=bo;
	xcmd->exec=exec;
	xcmd->cu_idx=-1;
	xcmd->slot_idx=-1;
	xcmd->packet = (struct ert_packet*)bo->vmapping;
	xcmd->xs = exec->scheduler;

	xcmd->client=client;
	atomic_inc(&client->outstanding_execs);

	/* dependencies are copied here, the anticipated wait_count is number
	 * of specified dependencies.  The wait_count is adjusted when the
	 * command is queued in the scheduler based on whether or not a
	 * dependency is active (managed by scheduler) */
	memcpy(xcmd->deps,deps,numdeps*sizeof(struct drm_xocl_bo*));
	xcmd->wait_count = numdeps;
	xcmd->chain_count = 0;

	set_cmd_state(xcmd,ERT_CMD_STATE_NEW);
	mutex_lock(&pending_cmds_mutex);
	list_add_tail(&xcmd->list,&pending_cmds);
	mutex_unlock(&pending_cmds_mutex);

	/* wake scheduler */
	atomic_inc(&num_pending);
	atomic_inc(&xdev->outstanding_execs);
	atomic64_inc(&xdev->total_execs);
	wake_up_interruptible(&xcmd->xs->wait_queue);

	SCHED_DEBUGF("<- add_cmd opcode(%d) type(%d)\n",opcode(xcmd),type(xcmd));
	return 0;
}

/**
 * recycle_cmd() - recycle a command objects
 *
 * @xcmd: command object to recycle
 *
 * Command object is added to the freelist
 *
 * Return: 0
 */
static int
recycle_cmd(struct xocl_cmd* xcmd)
{
	SCHED_DEBUGF("recycle(%lu) %p\n",xcmd->id,xcmd);
	mutex_lock(&free_cmds_mutex);
	list_move_tail(&xcmd->list,&free_cmds);
	mutex_unlock(&free_cmds_mutex);
	return 0;
}

/**
 * delete_cmd_list() - reclaim memory for all allocated command objects
 */
static void
delete_cmd_list(void)
{
	struct xocl_cmd *xcmd;
	struct list_head *pos, *next;

	mutex_lock(&free_cmds_mutex);
	list_for_each_safe(pos, next, &free_cmds) {
		xcmd = list_entry(pos, struct xocl_cmd, list);
		list_del(pos);
		kfree(xcmd);
	}
	mutex_unlock(&free_cmds_mutex);
}

/**
 * cleanup_exec()
 */
static void
cleanup_exec(struct xocl_cmd *xcmd)
{
	struct xocl_dev *xdev = cmd_get_xdev(xcmd);

	drm_gem_object_unreference_unlocked(&xcmd->bo->base);
	recycle_cmd(xcmd);
	atomic_dec(&xdev->outstanding_execs);
	atomic_dec(&xcmd->client->outstanding_execs);
}

/**
 * reset_exec() - Reset the scheduler
 *
 * @exec: Execution core (device) to reset
 *
 * Clear stale command objects associated with execution core.
 * This can occur if the HW for some reason hangs.
 */
SCHED_UNUSED
static void
reset_exec(struct exec_core* exec)
{
	int i;
	struct list_head *pos, *next;

	/* clear stale command objects if any */
	list_for_each_safe(pos, next, &pending_cmds) {
		struct xocl_cmd *xcmd = list_entry(pos,struct xocl_cmd,list);
		if (xcmd->exec!=exec)
			continue;
		DRM_INFO("deleting stale pending cmd\n");
		cleanup_exec(xcmd);
	}
	list_for_each_safe(pos, next, &global_scheduler0.command_queue) {
		struct xocl_cmd *xcmd = list_entry(pos,struct xocl_cmd,list);
		if (xcmd->exec!=exec)
			continue;
		DRM_INFO("deleting stale scheduler cmd\n");
		cleanup_exec(xcmd);
	}

	/* reset exec state */
        for (i=0; i<MAX_SLOTS; ++i)
		exec->submitted_cmds[i] = NULL;

	exec->num_slots = 16;
	exec->num_cus = 0;
	exec->cu_shift_offset = 0;
	exec->cu_base_addr = 0;
	exec->polling_mode = 1;
	exec->cq_interrupt = 0;
	exec->configured = false;
	exec->ops = &penguin_ops;

	for (i=0; i<MAX_CUS; ++i)
		exec->cu_addr_map[i] = 0;

	for (i=0; i<MAX_U32_SLOT_MASKS; ++i)
		exec->slot_status[i] = 0;
	exec->num_slot_masks = 1;

	for (i=0; i<MAX_U32_CU_MASKS; ++i)
		exec->cu_status[i] = 0;
	exec->num_cu_masks = 0;

	atomic_set(&exec->sr0,0);
	atomic_set(&exec->sr1,0);
	atomic_set(&exec->sr2,0);
	atomic_set(&exec->sr3,0);
}

/**
 * reset_all() - Reset the scheduler
 *
 * Clear stale command objects if any.  This can occur if the HW for
 * some reason hangs.
 */
static void
reset_all(void)
{
	/* clear stale command objects if any */
	while (!list_empty(&pending_cmds)) {
		struct xocl_cmd *xcmd = list_first_entry(&pending_cmds,struct xocl_cmd,list);
		DRM_INFO("deleting stale pending cmd\n");
		cleanup_exec(xcmd);
	}
	while (!list_empty(&global_scheduler0.command_queue)) {
		struct xocl_cmd *xcmd = list_first_entry(&global_scheduler0.command_queue,struct xocl_cmd,list);
		DRM_INFO("deleting stale scheduler cmd\n");
		cleanup_exec(xcmd);
	}
}

/**
 * is_ert() - Check if running in embedded (ert) mode.
 *
 * Return: %true of ert mode, %false otherwise
 */
inline bool
is_ert(struct exec_core *exec)
{
	return exec->ops == &mb_ops;
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
 * ffz_or_neg_one() - First first zero bit in bit mask
 *
 * @mask: mask to check
 * Return: Position of first zero bit, or -1 if none
 */
inline int
ffz_or_neg_one(u32 mask)
{
	if (mask==XOCL_U32_MASK)
		return -1;
	return ffz(mask);
}


/**
 * slot_size() - slot size per device configuration
 *
 * Return: Command queue slot size
 */
inline unsigned int
slot_size(struct exec_core *exec)
{
	return ERT_CQ_SIZE / exec->num_slots;
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
 * cu_idx_from_mask() - Given CU idx within a mask return its global idx [0..127]
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
 * slot_idx_in_mask() - Index of command queue slot within the mask that contains it
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
 * slot_idx_from_mask_idx() - Given slot idx within a mask, return its global idx [0..127]
 *
 * @slot_idx: Index of slot with mask identified by mask_idx
 * @mask_idx: Mask index of the mask hat has slot with slot_idx
 * Return: Global slot_idx [0..127]
 */
inline unsigned int
slot_idx_from_mask_idx(unsigned int slot_idx,unsigned int mask_idx)
{
	return slot_idx + (mask_idx << 5);
}


/**
 * cu_idx_to_addr() - Convert CU idx into it relative bar address.
 *
 * @xdev: Device handle
 * @cu_idx: Global CU idx
 * Return: Address of CU relative to bar
 */
inline u32
cu_idx_to_addr(struct exec_core *exec,unsigned int cu_idx)
{
	return exec->cu_addr_map[cu_idx];
}

/**
 * cu_idx_to_bitmask() - Compute the cu bitmask for cu_idx
 *
 * Subtract 32 * lower bitmasks prior to bitmask repsenting
 * this index.  For example, f.x cu_idx=67
 *  1 << (67 - (67>>5)<<5) =
 *  1 << (67 - (2<<5)) =
 *  1 << (67 - 64) =
 *  1 << 3 =
 *  0b1000 for position 4 in third bitmask
 *
 * @xdev: Device handle
 * @cu_idx: Global index [0..127] of CU
 *
 * This function computes the bitmask for cu_idx in the mask that stores cu_idx
 *
 * Return: Bitmask with bit set for corresponding CU
 */
inline u32
cu_idx_to_bitmask(struct exec_core *exec, u32 cu_idx)
{
	return 1 << (cu_idx - (cu_mask_idx(cu_idx)<<5));
}


/**
 * configure() - Configure the scheduler from user space command
 *
 * Process the configure command sent from user space. Only one process can
 * configure the scheduler, so if scheduler is already configured and held by
 * another process, the function errors out.
 *
 * Return: 0 on success, 1 on failure
 */
static int
configure(struct xocl_cmd *xcmd)
{
	struct exec_core *exec=xcmd->exec;
	struct xocl_dev *xdev = exec_get_xdev(exec);
	bool ert = xocl_mb_sched_on(xdev);
	bool cdma = xocl_cdma_on(xdev);
	unsigned int dsa = xocl_dsa_version(xdev);
	struct ert_configure_cmd *cfg;
	int i;

	DRM_INFO("ert per feature rom = %d\n",ert);
	DRM_INFO("dsa per feature rom = %d\n",dsa);

	if (sched_error_on(exec,opcode(xcmd)!=ERT_CONFIGURE,"expected configure command"))
		return 1;

	/* Only allow configuration with one live ctx */
	if (exec->configured) {
		DRM_INFO("command scheduler is already configured for this device\n");
		return 1;
	}

	cfg = (struct ert_configure_cmd *)(xcmd->packet);

	if (cfg->count != 5 + cfg->num_cus) {
		DRM_INFO("invalid configure command, count=%d expected 5+num_cus(%d)\n",cfg->count,cfg->num_cus);
		return 1;
	}

	SCHED_DEBUG("configuring scheduler\n");
	exec->num_slots = ERT_CQ_SIZE / cfg->slot_size;
	exec->num_cus = cfg->num_cus;
	exec->cu_shift_offset = cfg->cu_shift;
	exec->cu_base_addr = cfg->cu_base_addr;
	exec->num_cu_masks = ((exec->num_cus-1)>>5) + 1;
	exec->num_slot_masks = ((exec->num_slots-1)>>5) + 1;

	for (i=0; i<exec->num_cus; ++i) {
		exec->cu_addr_map[i] = cfg->data[i];
		SCHED_DEBUGF("++ configure cu(%d) at 0x%x\n",i,exec->cu_addr_map[i]);
	}

	if (cdma) {
		exec->cu_addr_map[exec->num_cus] = 0x250000;
		SCHED_DEBUGF("++ configure cdma cu(%d) at 0x%x\n",exec->num_cus,exec->cu_addr_map[exec->num_cus]);
		exec->num_cus = ++cfg->num_cus;
	}

	if (ert && cfg->ert) {
		SCHED_DEBUG("++ configuring embedded scheduler mode\n");
		exec->ops = &mb_ops;
		exec->polling_mode = cfg->polling;
		exec->cq_interrupt = cfg->cq_int;
		cfg->dsa52 = (dsa>=52) ? 1 : 0;
		cfg->cdma = cdma ? 1 : 0;
	}
	else {
		SCHED_DEBUG("++ configuring penguin scheduler mode\n");
		exec->ops = &penguin_ops;
		exec->polling_mode = 1;
	}

	DRM_INFO("scheduler config ert(%d) slots(%d), cudma(%d), cuisr(%d), cdma(%d), cus(%d), cu_shift(%d), cu_base(0x%x), cu_masks(%d)\n"
		 ,is_ert(exec)
		 ,exec->num_slots
		 ,cfg->cu_dma ? 1 : 0
		 ,cfg->cu_isr ? 1 : 0
		 ,cfg->cdma ? 1 : 0
		 ,exec->num_cus
		 ,exec->cu_shift_offset
		 ,exec->cu_base_addr
		 ,exec->num_cu_masks);

	exec->configured=true;
	return 0;
}

/**
 * exec_write() - Execute a write command
 */
static int
exec_write(struct xocl_cmd *xcmd)
{
	struct ert_packet *cmd = xcmd->packet;
	unsigned int idx=0;
	SCHED_DEBUGF("-> exec_write(%lu)\n",xcmd->id);
	for (idx=0; idx<cmd->count-1; idx+=2) {
		u32 addr = cmd->data[idx];
		u32 val = cmd->data[idx+1];
		SCHED_DEBUGF("+ exec_write base[0x%x] = 0x%x\n",addr,val);
		iowrite32(val,xcmd->exec->base + addr);
	}
	SCHED_DEBUG("<- exec_write\n");
	return 0;
}

/**
 * acquire_slot_idx() - Acquire a slot index if available.  Update slot status to busy
 * so it cannot be reacquired.
 *
 * This function is called from scheduler thread
 *
 * Return: Command queue slot index, or -1 if none avaiable
 */
static int
acquire_slot_idx(struct exec_core *exec)
{
	unsigned int mask_idx=0, slot_idx=-1;
	u32 mask;
	SCHED_DEBUG("-> acquire_slot_idx\n");
	for (mask_idx=0; mask_idx<exec->num_slot_masks; ++mask_idx) {
		mask = exec->slot_status[mask_idx];
		slot_idx = ffz_or_neg_one(mask);
		if (slot_idx==-1 || slot_idx_from_mask_idx(slot_idx,mask_idx)>=exec->num_slots)
			continue;
		exec->slot_status[mask_idx] ^= (1<<slot_idx);
		SCHED_DEBUGF("<- acquire_slot_idx returns %d\n",slot_idx_from_mask_idx(slot_idx,mask_idx));
		return slot_idx_from_mask_idx(slot_idx,mask_idx);
	}
	SCHED_DEBUGF("<- acquire_slot_idx returns -1\n");
	return -1;
}

/**
 * release_slot_idx() - Release a slot index
 *
 * Update slot status mask for slot index.  Notify scheduler in case
 * release is via ISR
 *
 * @xdev: scheduler
 * @slot_idx: the slot index to release
 */
static void
release_slot_idx(struct exec_core *exec, unsigned int slot_idx)
{
	unsigned int mask_idx = slot_mask_idx(slot_idx);
	unsigned int pos = slot_idx_in_mask(slot_idx);
	SCHED_DEBUGF("<-> release_slot_idx slot_status[%d]=0x%x, pos=%d\n"
		     ,mask_idx,exec->slot_status[mask_idx],pos);
	exec->slot_status[mask_idx] ^= (1<<pos);
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
get_cu_idx(struct exec_core *exec, unsigned int cmd_idx)
{
	struct xocl_cmd *xcmd = exec->submitted_cmds[cmd_idx];
	if (sched_error_on(exec,!xcmd,"no submtted cmd"))
		return -1;
	return xcmd->cu_idx;
}

/**
 * cu_done() - Check status of CU
 *
 * @cu_idx: Index of cu to check
 *
 * This function is called in polling mode only.  The cu_idx
 * is guaranteed to have been started
 *
 * Return: %true if cu done, %false otherwise
 */
inline bool
cu_done(struct exec_core *exec, unsigned int cu_idx)
{
	u32 cu_addr = cu_idx_to_addr(exec,cu_idx);
	SCHED_DEBUGF("-> cu_done(%d) checks cu at address 0x%x\n",cu_idx,cu_addr);
	/* done is indicated by AP_DONE(2) alone or by AP_DONE(2) | AP_IDLE(4)
	 * but not by AP_IDLE itself.  Since 0x10 | (0x10 | 0x100) = 0x110
	 * checking for 0x10 is sufficient. */
	if (ioread32(exec->base + cu_addr) & 2) {
		unsigned int mask_idx = cu_mask_idx(cu_idx);
		unsigned int pos = cu_idx_in_mask(cu_idx);
		exec->cu_status[mask_idx] ^= 1<<pos;
		SCHED_DEBUG("<- cu_done returns 1\n");
		return true;
	}
	SCHED_DEBUG("<- cu_done returns 0\n");
	return false;
}

/**
 * chain_dependencies() - Chain this command to its dependencies
 *
 * @xcmd: Command to chain to its dependencies
 *
 * This function looks at all incoming explicit BO dependencies, checks if a
 * corresponding xocl_cmd object exists (is active) in which case that command
 * object must chain argument xcmd so that it (xcmd) can be triggered when
 * dependency completes.  The chained command has a wait count correponding to
 * the number of dependencies that are active.
 */
static int
chain_dependencies(struct xocl_cmd* xcmd)
{
	int didx;
	int dcount=xcmd->wait_count;
	SCHED_DEBUGF("-> chain_dependencies of xcmd(%lu)\n",xcmd->id);
	for (didx=0; didx<dcount; ++didx) {
		struct drm_xocl_bo *dbo = xcmd->deps[didx];
		struct xocl_cmd* chain_to = dbo->metadata.active;
		/* release reference created in ioctl call when dependency was looked up
		 * see comments in xocl_ioctl.c:xocl_execbuf_ioctl() */
		drm_gem_object_unreference_unlocked(&dbo->base);
		xcmd->deps[didx] = NULL;
		if (!chain_to) { /* command may have completed already */
			--xcmd->wait_count;
			continue;
		}
		if (chain_to->chain_count>=MAX_DEPS) {
			DRM_INFO("chain count exceeded");
			return 1;
		}
		SCHED_DEBUGF("+ xcmd(%lu)->chain[%d]=xcmd(%lu)",chain_to->id,chain_to->chain_count,xcmd->id);
		chain_to->chain[chain_to->chain_count++] = xcmd;
	}
	SCHED_DEBUG("<- chain_dependencies\n");
	return 0;
}

/**
 * trigger_chain() - Trigger the execution of any commands chained to argument command
 *
 * @xcmd: Completed command that must trigger its chained (waiting) commands
 *
 * The argument command has completed and must trigger the execution of all
 * chained commands whos wait_count is 0.
 */
static int
trigger_chain(struct xocl_cmd *xcmd)
{
	SCHED_DEBUGF("-> trigger_chain xcmd(%lu)\n",xcmd->id);
	while (xcmd->chain_count) {
		struct xocl_cmd *trigger = xcmd->chain[--xcmd->chain_count];
		SCHED_DEBUGF("+ cmd(%lu) triggers cmd(%lu) with wait_count(%d)\n",xcmd->id,trigger->id,trigger->wait_count);
		sched_error_on(trigger->exec,trigger->wait_count<=0,"expected positive wait count");
		/* start trigger if its wait_count becomes 0 */
		if (--trigger->wait_count==0)
			queued_to_running(trigger);
	}
	SCHED_DEBUG("<- trigger_chain\n");
	return 0;
}

/**
 * notify_host() - Notify user space that a command is complete.
 */
static void
notify_host(struct xocl_cmd *xcmd)
{
	struct list_head *ptr;
	struct client_ctx *entry;
	struct exec_core *exec = xcmd->exec;
	struct xocl_dev *xdev = exec_get_xdev(exec);

	SCHED_DEBUGF("-> notify_host xcmd(%lu)\n",xcmd->id);

	/* now for each client update the trigger counter in the context */
	mutex_lock(&xdev->ctx_list_lock);
	list_for_each(ptr, &xdev->ctx_list) {
		entry = list_entry(ptr, struct client_ctx, link);
		atomic_inc(&entry->trigger);
	}
	mutex_unlock(&xdev->ctx_list_lock);
	/* wake up all the clients */
	wake_up_interruptible(&exec->poll_wait_queue);
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
 * queue. The slot is released so new commands can be submitted.  The host
 * is notified that some command has completed.
 */
static void
mark_cmd_complete(struct xocl_cmd *xcmd)
{
	struct exec_core *exec=xcmd->exec;

	SCHED_DEBUGF("-> mark_cmd_complete xcmd(%lu) slot(%d)\n",xcmd->id,xcmd->slot_idx);
	exec->submitted_cmds[xcmd->slot_idx] = NULL;
	set_cmd_state(xcmd,ERT_CMD_STATE_COMPLETED);
	if (exec->polling_mode)
		--xcmd->xs->poll;
	release_slot_idx(exec,xcmd->slot_idx);
	notify_host(xcmd);

	// Deactivate command and trigger chain of waiting commands
	xcmd->bo->metadata.active=NULL;
	trigger_chain(xcmd);

	SCHED_DEBUGF("<- mark_cmd_complete\n");
}

/**
 * mark_mask_complete() - Move all commands in mask to complete state
 *
 * @mask: Bitmask with queried statuses of commands
 * @mask_idx: Index of the command mask. Used to offset the actual cmd slot index
 */
static void
mark_mask_complete(struct exec_core *exec, u32 mask, unsigned int mask_idx)
{
	int bit_idx=0,cmd_idx=0;
	SCHED_DEBUGF("-> mark_mask_complete(0x%x,%d)\n",mask,mask_idx);
	if (!mask)
		return;
	for (bit_idx=0, cmd_idx=mask_idx<<5; bit_idx<32; mask>>=1,++bit_idx,++cmd_idx)
		if (mask & 0x1)
			mark_cmd_complete(exec->submitted_cmds[cmd_idx]);
	SCHED_DEBUG("<- mark_mask_complete\n");
}

/**
 * queued_to_running() - Move a command from queued to running state if possible
 *
 * @xcmd: Command to start
 *
 * Upon success, the command is not necessarily running. In ert mode the
 * command will have been submitted to the embedded scheduler, whereas in
 * penguin mode the command has been started on a CU.
 *
 * Return: %true if command was submitted to device, %false otherwise
 */
static bool
queued_to_running(struct xocl_cmd *xcmd)
{
	bool retval = false;

	if (xcmd->wait_count)
		return false;

	SCHED_DEBUGF("-> queued_to_running(%lu) opcode(%d)\n",xcmd->id,opcode(xcmd));

	if (opcode(xcmd)==ERT_CONFIGURE && configure(xcmd)) {
		set_cmd_state(xcmd,ERT_CMD_STATE_ERROR);
		return false;
	}

	if (opcode(xcmd)==ERT_WRITE && exec_write(xcmd)) {
		set_cmd_state(xcmd,ERT_CMD_STATE_ERROR);
		return false;
	}

	if (xcmd->exec->ops->submit(xcmd)) {
		set_cmd_int_state(xcmd,ERT_CMD_STATE_RUNNING);
		if (xcmd->exec->polling_mode)
			++xcmd->xs->poll;
		xcmd->exec->submitted_cmds[xcmd->slot_idx] = xcmd;
		retval = true;
	}

	SCHED_DEBUGF("<- queued_to_running returns %d\n",retval);

	return retval;
}

/**
 * running_to_complete() - Check status of running commands
 *
 * @xcmd: Command is in running state
 *
 * If a command is found to be complete, it marked complete prior to return
 * from this function.
 */
static void
running_to_complete(struct xocl_cmd *xcmd)
{
	SCHED_DEBUGF("-> running_to_complete(%lu)\n",xcmd->id);
	xcmd->exec->ops->query(xcmd);
	SCHED_DEBUG("<- running_to_complete\n");
}

/**
 * complete_to_free() - Recycle a complete command objects
 *
 * @xcmd: Command is in complete state
 */
static void
complete_to_free(struct xocl_cmd *xcmd)
{
	SCHED_DEBUGF("-> complete_to_free(%lu)\n",xcmd->id);
	cleanup_exec(xcmd);
	SCHED_DEBUG("<- complete_to_free\n");
}

static void
error_to_free(struct xocl_cmd *xcmd)
{
	SCHED_DEBUGF("-> error_to_free(%lu)\n",xcmd->id);
	notify_host(xcmd);
	complete_to_free(xcmd);
	SCHED_DEBUG("<- error_to_free\n");
}

static void
abort_to_free(struct xocl_cmd *xcmd)
{
	SCHED_DEBUGF("-> abort_to_free(%lu)\n",xcmd->id);
	complete_to_free(xcmd);
	SCHED_DEBUG("<- abort_to_free\n");
}

#if 0
static void
abort_cmd(struct xocl_cmd *xcmd)
{
	struct xocl_cmd *abort_cmd = get_free_xocl_cmd();
	abort_cmd->bo=NULL;
	abort_cmd->exec=xcmd->exec;

	ert_abort_cmd abort;
	abort.state = 1;
	abort.idx = xcmd->slot_idx;

	iowrite32
	SCHED_DEBUG("-> abort_cmd\n");
	SCHED_DEBUG("<- abort_cmd\n");
}
#endif

/**
 * scheduler_queue_cmds() - Queue any pending commands
 *
 * The scheduler copies pending commands to its internal command queue where
 * is is now in queued state.
 */
static void
scheduler_queue_cmds(struct xocl_sched *xs)
{
	struct xocl_cmd *xcmd;
	struct list_head *pos, *next;

	SCHED_DEBUG("-> scheduler_queue_cmds\n");
	mutex_lock(&pending_cmds_mutex);
	list_for_each_safe(pos, next, &pending_cmds) {
		xcmd = list_entry(pos, struct xocl_cmd, list);
		if (xcmd->xs != xs)
			continue;
		SCHED_DEBUGF("+ queueing cmd(%lu)\n",xcmd->id);
		list_del(&xcmd->list);
		list_add_tail(&xcmd->list,&xs->command_queue);

		/* chain active dependencies if any to this command object */
		if (xcmd->wait_count && chain_dependencies(xcmd))
			set_cmd_state(xcmd,ERT_CMD_STATE_ERROR);
		else
			set_cmd_int_state(xcmd,ERT_CMD_STATE_QUEUED);

		/* this command is now active and can chain other commands */
		xcmd->bo->metadata.active=xcmd;
		atomic_dec(&num_pending);
	}
	mutex_unlock(&pending_cmds_mutex);
	SCHED_DEBUG("<- scheduler_queue_cmds\n");
}

/**
 * scheduler_iterator_cmds() - Iterate all commands in scheduler command queue
 */
static void
scheduler_iterate_cmds(struct xocl_sched *xs)
{
	struct list_head *pos, *next;

	SCHED_DEBUG("-> scheduler_iterate_cmds\n");
	list_for_each_safe(pos, next, &xs->command_queue) {
		struct xocl_cmd *xcmd = list_entry(pos, struct xocl_cmd, list);
		update_cmd_state(xcmd);

		SCHED_DEBUGF("+ processing cmd(%lu)\n",xcmd->id);

		/* check running first since queued maybe we waiting for cmd slot */
		if (xcmd->state == ERT_CMD_STATE_QUEUED)
			queued_to_running(xcmd);
		if (xcmd->state == ERT_CMD_STATE_RUNNING)
			running_to_complete(xcmd);
		if (xcmd->state == ERT_CMD_STATE_COMPLETED)
			complete_to_free(xcmd);
		if (xcmd->state == ERT_CMD_STATE_ERROR)
			error_to_free(xcmd);
		if (xcmd->state == ERT_CMD_STATE_ABORT)
			abort_to_free(xcmd);
	}
	SCHED_DEBUG("<- scheduler_iterate_cmds\n");
}

/**
 * scheduler_wait_condition() - Check status of scheduler wait condition
 *
 * Scheduler must wait (sleep) if
 *   1. there are no pending commands
 *   2. no pending interrupt from embedded scheduler
 *   3. no pending complete commands in polling mode
 *
 * Return: 1 if scheduler must wait, 0 othewise
 */
static int
scheduler_wait_condition(struct xocl_sched *xs)
{
	if (kthread_should_stop()) {
		xs->stop = 1;
		SCHED_DEBUG("scheduler wakes kthread_should_stop\n");
		return 0;
	}

	if (atomic_read(&num_pending)) {
		SCHED_DEBUG("scheduler wakes to copy new pending commands\n");
		return 0;
	}

	if (atomic_read(&xs->intc)) {
		SCHED_DEBUG("scheduler wakes on interrupt\n");
		atomic_set(&xs->intc,0);
		return 0;
	}

	if (xs->poll) {
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
scheduler_wait(struct xocl_sched *xs)
{
	wait_event_interruptible(xs->wait_queue,scheduler_wait_condition(xs)==0);
}

/**
 * scheduler_loop() - Run one loop of the scheduler
 */
static void
scheduler_loop(struct xocl_sched *xs)
{
	SCHED_DEBUG("scheduler_loop\n");

	scheduler_wait(xs);

	if (xs->error) {
		DRM_INFO("scheduler encountered unexpected error\n");
	}

	if (xs->stop)
		return;

	/* queue new pending commands */
	scheduler_queue_cmds(xs);

	/* iterate all commands */
	scheduler_iterate_cmds(xs);

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
scheduler(void* data)
{
	struct xocl_sched *xs = (struct xocl_sched *)data;
	while (!xs->stop)
		scheduler_loop(xs);
	DRM_INFO("%s:%d scheduler thread exits with value %d\n",__FILE__,__LINE__,xs->error);
	return xs->error;
}

/**
 * init_scheduler_thread() - Initialize scheduler thread if necessary
 *
 * Return: 0 on success, -errno otherwise
 */
static int
init_scheduler_thread(void)
{
	SCHED_DEBUGF("init_scheduler_thread use_count=%d\n",global_scheduler0.use_count);
	if (global_scheduler0.use_count++)
		return 0;

	sched_loop_cnt = 0;

	init_waitqueue_head(&global_scheduler0.wait_queue);
	global_scheduler0.error = 0;
	global_scheduler0.stop = 0;

	INIT_LIST_HEAD(&global_scheduler0.command_queue);
	atomic_set(&global_scheduler0.intc,0);
	global_scheduler0.poll=0;

	global_scheduler0.scheduler_thread = kthread_run(scheduler,(void*)&global_scheduler0,"xocl-scheduler-thread0");
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
	SCHED_DEBUGF("fini_scheduler_thread use_count=%d\n",global_scheduler0.use_count);
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
 * mb_query() - Check command status of argument command
 *
 * @xcmd: Command to check
 *
 * This function is for ERT mode.  In polling mode, check the command status
 * register containing the slot assigned to the command.  In interrupt mode
 * check the interrupting status register.  The function checks all commands in
 * the same command status register as argument command so more than one
 * command may be marked complete by this function.
 */
static void
mb_query(struct xocl_cmd *xcmd)
{
	struct exec_core *exec = xcmd->exec;
	unsigned int cmd_mask_idx = slot_mask_idx(xcmd->slot_idx);

	SCHED_DEBUGF("-> mb_query(%lu) slot_idx(%d), cmd_mask_idx(%d)\n",xcmd->id,xcmd->slot_idx,cmd_mask_idx);

	if (type(xcmd)==ERT_KDS_LOCAL) {
		mark_cmd_complete(xcmd);
		SCHED_DEBUG("<- mb_query local command\n");
		return;
	}

	if (exec->polling_mode
	    || (cmd_mask_idx==0 && atomic_xchg(&exec->sr0,0))
	    || (cmd_mask_idx==1 && atomic_xchg(&exec->sr1,0))
	    || (cmd_mask_idx==2 && atomic_xchg(&exec->sr2,0))
	    || (cmd_mask_idx==3 && atomic_xchg(&exec->sr3,0))) {
		u32 csr_addr = ERT_STATUS_REGISTER_ADDR + (cmd_mask_idx<<2);
		u32 mask = ioread32(xcmd->exec->base + csr_addr);
		SCHED_DEBUGF("++ mb_query csr_addr=0x%x mask=0x%x\n",csr_addr,mask);
		if (mask)
			mark_mask_complete(xcmd->exec,mask,cmd_mask_idx);
	}

	SCHED_DEBUGF("<- mb_query\n");
}

/**
 * penguin_query() - Check command status of argument command
 *
 * @xcmd: Command to check
 *
 * Function is called in penguin mode (no embedded scheduler).
 */
static void
penguin_query(struct xocl_cmd *xcmd)
{
	u32 opc = opcode(xcmd);

	SCHED_DEBUGF("-> penguin_queury() slot_idx=%d\n",xcmd->slot_idx);

	if (type(xcmd)==ERT_KDS_LOCAL
	    ||opc==ERT_CONFIGURE
	    ||(opc==ERT_START_CU && cu_done(xcmd->exec,get_cu_idx(xcmd->exec,xcmd->slot_idx))))
		mark_cmd_complete(xcmd);

	SCHED_DEBUG("<- penguin_queury\n");
}

/**
 * mb_submit() - Submit a command the embedded scheduler command queue
 *
 * @xcmd:  Command to submit
 * Return: %true if successfully submitted, %false otherwise
 */
static bool
mb_submit(struct xocl_cmd *xcmd)
{
	u32 slot_addr;

	SCHED_DEBUGF("-> mb_submit(%lu)\n",xcmd->id);

	xcmd->slot_idx = acquire_slot_idx(xcmd->exec);
	if (xcmd->slot_idx<0) {
		SCHED_DEBUG("<- mb_submit returns false\n");
		return false;
	}

	if (type(xcmd)==ERT_KDS_LOCAL) {
		SCHED_DEBUG("<- mb_submit returns true for local command\n");
		return true;
	}

	slot_addr = ERT_CQ_BASE_ADDR + xcmd->slot_idx*slot_size(xcmd->exec);
	SCHED_DEBUGF("++ mb_submit slot_idx=%d, slot_addr=0x%x\n",xcmd->slot_idx,slot_addr);

	SCHED_DEBUG_PACKET(xcmd->packet,packet_size(xcmd));

	/* write packet minus header */
	memcpy_toio(xcmd->exec->base + slot_addr + 4,xcmd->packet->data,(packet_size(xcmd)-1)*sizeof(u32));

	/* write header */
	iowrite32(xcmd->packet->header,xcmd->exec->base + slot_addr);

	/* trigger interrupt to embedded scheduler if feature is enabled */
	if (xcmd->exec->cq_interrupt) {
		u32 cq_int_addr = ERT_CQ_STATUS_REGISTER_ADDR + (slot_mask_idx(xcmd->slot_idx)<<2);
		u32 mask = 1<<slot_idx_in_mask(xcmd->slot_idx);
		SCHED_DEBUGF("++ mb_submit writes slot mask 0x%x to CQ_INT register at addr 0x%x\n",
			     mask,cq_int_addr);
		iowrite32(mask,xcmd->exec->base + cq_int_addr);
	}

	SCHED_DEBUG("<- mb_submit returns true\n");
	return true;
}

/**
 * get_free_cu() - get index of first available CU per command cu mask
 *
 * @xcmd: command containing CUs to check for availability
 *
 * This function is called kernel software scheduler mode only, in embedded
 * scheduler mode, the hardware scheduler handles the commands directly.
 *
 * Return: Index of free CU, -1 of no CU is available.
 */
static int
get_free_cu(struct xocl_cmd *xcmd)
{
	int mask_idx=0;
	int num_masks = cu_masks(xcmd);
	SCHED_DEBUG("-> get_free_cu\n");
	for (mask_idx=0; mask_idx<num_masks; ++mask_idx) {
		u32 cmd_mask = xcmd->packet->data[mask_idx]; /* skip header */
		u32 busy_mask = xcmd->exec->cu_status[mask_idx];
		int cu_idx = ffs_or_neg_one((cmd_mask | busy_mask) ^ busy_mask);
		if (cu_idx>=0) {
			xcmd->exec->cu_status[mask_idx] ^= 1<<cu_idx;
			SCHED_DEBUGF("<- get_free_cu returns %d\n",cu_idx_from_mask(cu_idx,mask_idx));
			return cu_idx_from_mask(cu_idx,mask_idx);
		}
	}
	SCHED_DEBUG("<- get_free_cu returns -1\n");
	return -1;
}

/**
 * configure_cu() - transfer command register map to specified CU and start the CU.
 *
 * @xcmd: command with register map to transfer to CU
 * @cu_idx: index of CU to configure
 *
 * This function is called in kernel software scheduler mode only.
 */
static void
configure_cu(struct xocl_cmd *xcmd, int cu_idx)
{
	u32 i;
	struct exec_core *exec = xcmd->exec;
	u32 cu_addr = cu_idx_to_addr(xcmd->exec,cu_idx);
	u32 size = regmap_size(xcmd);
	struct ert_start_kernel_cmd *ecmd = (struct ert_start_kernel_cmd *)xcmd->packet;

	SCHED_DEBUGF("-> configure_cu cu_idx=%d, cu_addr=0x%x, regmap_size=%d\n"
		     ,cu_idx,cu_addr,size);

	/* past header, past cumasks */
	SCHED_DEBUG_PACKET(ecmd+1+ecmd->extra_cu_masks+1,size);

	/* write register map, but skip first word (AP_START) */
	/* can't get memcpy_toio to work */
	/* memcpy_toio(user_bar + cu_addr + 4,ecmd->data + ecmd->extra_cu_masks + 1,(size-1)*4); */
	for (i=1; i<size; ++i)
		iowrite32(*(ecmd->data + ecmd->extra_cu_masks + i),exec->base + cu_addr + (i<<2));

	/* start CU at base + 0x0 */
	iowrite32(0x1,exec->base + cu_addr);

	SCHED_DEBUG("<- configure_cu\n");
}

/**
 * penguin_submit() - penguin submit of a command
 *
 * @xcmd: command to submit
 *
 * Special processing for configure command.  Configuration itself is
 * done/called by queued_to_running before calling penguin_submit.  In penguin
 * mode configuration need to ensure that the command is retired properly by
 * scheduler, so assign it a slot index and let normal flow continue.
 *
 * Return: %true on successful submit, %false otherwise
 */
static bool
penguin_submit(struct xocl_cmd *xcmd)
{
	SCHED_DEBUGF("-> penguin_submit(%lu) opcode(%d) type(%d)\n",xcmd->id,opcode(xcmd),type(xcmd));

	/* execution done by submit_cmds, ensure the cmd retired properly */
	if (opcode(xcmd)==ERT_CONFIGURE || type(xcmd)==ERT_KDS_LOCAL) {
		xcmd->slot_idx = acquire_slot_idx(xcmd->exec);
		SCHED_DEBUGF("<- penguin_submit slot(%d)\n",xcmd->slot_idx);
		return true;
	}

	if (opcode(xcmd)!=ERT_START_CU)
		return false;

	/* extract cu list */
	xcmd->cu_idx = get_free_cu(xcmd);
	if (xcmd->cu_idx<0)
		return false;

	xcmd->slot_idx = acquire_slot_idx(xcmd->exec);
	if (xcmd->slot_idx<0)
		return false;

	/* found free cu, transfer regmap and start it */
	configure_cu(xcmd,xcmd->cu_idx);

	SCHED_DEBUGF("<- penguin_submit cu_idx(%d) slot(%d)\n",xcmd->cu_idx,xcmd->slot_idx);

	return true;
}


/**
 * mb_ops: operations for ERT scheduling
 */
static struct sched_ops mb_ops = {
	.submit = mb_submit,
	.query = mb_query,
};

/**
 * penguin_ops: operations for kernel mode scheduling
 */
static struct sched_ops penguin_ops = {
	.submit = penguin_submit,
	.query = penguin_query,
};

static irqreturn_t exec_isr(int irq, void *arg)
{
	struct exec_core *exec = (struct exec_core *)arg;

	SCHED_DEBUGF("-> xocl_user_event %d\n",irq);
	if (is_ert(exec) && !exec->polling_mode) {

		if (irq==0)
			atomic_set(&exec->sr0,1);
		else if (irq==1)
			atomic_set(&exec->sr1,1);
		else if (irq==2)
			atomic_set(&exec->sr2,1);
		else if (irq==3)
			atomic_set(&exec->sr3,1);

		/* wake up all scheduler ... currently one only */
		atomic_set(&global_scheduler0.intc,1);
		wake_up_interruptible(&global_scheduler0.wait_queue);
	} else {
		xocl_err(&exec->pdev->dev, "Unhandled isr irq %d, is_ert %d, "
			"polling %d", irq, is_ert(exec), exec->polling_mode);
	}
	SCHED_DEBUGF("<- xocl_user_event\n");
	return IRQ_HANDLED;
}

/**
 * Entry point for exec buffer.
 *
 * Function adds exec buffer to the pending list of commands
 */
int
add_exec_buffer(struct platform_device *pdev, struct client_ctx *client, void *buf, int numdeps, struct drm_xocl_bo **deps)
{
	struct exec_core *exec = platform_get_drvdata(pdev);
	/* Add the command to pending list */
	return add_cmd(exec, client, buf, numdeps, deps);
}

static int create_client(struct platform_device *pdev, void **priv)
{
	struct client_ctx	*client;
	struct exec_core	*exec;
	struct xocl_dev *xdev = xocl_get_xdev(pdev);

	DRM_INFO("scheduler client created pid(%d)\n",pid_nr(task_tgid(current)));

	client = devm_kzalloc(&pdev->dev, sizeof (*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;

	client->pid = task_tgid(current);
	exec = platform_get_drvdata(pdev);

	mutex_init(&client->lock);

	atomic_set(&client->trigger, 0);
	atomic_set(&client->abort, 0);
	atomic_set(&client->outstanding_execs, 0);
	mutex_lock(&xdev->ctx_list_lock);
	client->xdev = xocl_get_xdev(pdev);
	list_add_tail(&client->link, &xdev->ctx_list);

	/* kds must be configured on first xdev context even if that context
	 * does not trigger an xclbin download */
	if (list_is_singular(&xdev->ctx_list))
		reset_exec(exec);

	mutex_unlock(&xdev->ctx_list_lock);

	*priv =  client;

	return 0;
}

static void destroy_client(struct platform_device *pdev, void **priv)
{
	struct client_ctx 	*client = (struct client_ctx *)(*priv);
	struct xocl_dev         *xdev = xocl_get_xdev(pdev);
	unsigned int            outstanding = atomic_read(&client->outstanding_execs);
	unsigned int            timeout_loops = 20;
	unsigned int            loops = 0;

	/* force scheduler to abort execs for this client */
	atomic_set(&client->abort,1);

	/* wait for outstanding execs to finish */
	while (outstanding) {
		unsigned int new;
		userpf_info(xdev,"waiting for %d outstanding execs to finish",outstanding);
		msleep(500);
		new = atomic_read(&client->outstanding_execs);
		loops = (new==outstanding ? (loops + 1) : 0);
		if (loops == timeout_loops) {
			userpf_err(xdev,"Giving up with %d outstanding execs, please reset device with 'xbsak reset -h'\n",outstanding);
			atomic_set(&xdev->needs_reset,1);
			break;
		}
		outstanding = new;
	}

	DRM_INFO("client exits pid(%d)\n",pid_nr(client->pid));

	mutex_lock(&xdev->ctx_list_lock);
	list_del(&client->link);
	mutex_unlock(&xdev->ctx_list_lock);

	mutex_destroy(&client->lock);
	devm_kfree(&pdev->dev, client);
	*priv = NULL;
}

static uint poll_client(struct platform_device *pdev, struct file *filp,
	poll_table *wait, void *priv)
{
	struct client_ctx	*client = (struct client_ctx *)priv;
	struct exec_core	*exec;
	int			counter;
	uint			ret = 0;

	exec = platform_get_drvdata(pdev);

	poll_wait(filp, &exec->poll_wait_queue, wait);

	/*
	 * Mutex lock protects from two threads from the same application
	 * calling poll concurrently using the same file handle
	 */
	mutex_lock(&client->lock);
	counter = atomic_read(&client->trigger);
	if (counter > 0) {
		/*
		 * Use atomic here since the trigger may be incremented by
		 * interrupt handler running concurrently.
		 */
		atomic_dec(&client->trigger);
		ret = POLLIN;
	}
	mutex_unlock(&client->lock);

	return ret;
}

/**
 * reset() - Reset device exec data structure
 *
 * @pdev: platform device to reset
 *
 * This function is currently called from mgmt icap on every AXI is
 * freeze/unfreeze.  It ensures that the device exec_core state is reset to
 * same state as was when scheduler was originally probed for the device.
 * The callback from icap, ensures that scheduler resets the exec core when
 * multiple processes are already attached to the device but AXI is reset.
 *
 * Even though the very first client created for this device also resets the
 * exec core, it is possible that further resets are necessary.  For example
 * in multi-process case, there can be 'n' processes that attach to the
 * device.  On first client attach the exec core is reset correctly, but now
 * assume that 'm' of these processes finishes completely before any remaining
 * (n-m) processes start using the scheduler.  In this case, the n-m clients have
 * already been created, but icap resets AXI because the xclbin has no
 * references.
 */
static int
reset(struct platform_device *pdev)
{
	struct exec_core *exec = platform_get_drvdata(pdev);
	reset_exec(exec);
	return 0;
}

static int
validate(struct platform_device *pdev, struct client_ctx *client, const struct drm_xocl_bo *cmd)
{
	// TODO: Add code to check if requested cmd is valid in the current context
	return 0;
}

struct xocl_mb_scheduler_funcs sche_ops = {
	.add_exec_buffer = add_exec_buffer,
	.create_client = create_client,
	.destroy_client = destroy_client,
	.poll_client = poll_client,
	.reset = reset,
	.validate = validate,
};

/**
 * Init scheduler
 */
static int mb_scheduler_probe(struct platform_device *pdev)
{
	struct exec_core *exec;
	struct resource *res;
	struct xocl_dev *xdev;
	unsigned int i;

	exec = devm_kzalloc(&pdev->dev, sizeof(*exec), GFP_KERNEL);
	if (!exec)
		return -ENOMEM;

	/* uses entire bar for now, because scheduler directly program
 	 * CUs.
	 */
	xdev = xocl_get_xdev(pdev);
	exec->base = xdev->base_addr;

	res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	exec->intr_base = res->start;
	exec->intr_num = res->end - res->start + 1;

	exec->pdev = pdev;
	init_waitqueue_head(&exec->poll_wait_queue);

	exec->scheduler = &global_scheduler0;

	for (i = 0; i < exec->intr_num; i++) {
		xocl_user_interrupt_reg(xdev, i + exec->intr_base,
			exec_isr, exec);
		xocl_user_interrupt_config(xdev, i + exec->intr_base, true);
	}

	init_scheduler_thread();
	reset_exec(exec);

	xocl_subdev_register(pdev, XOCL_SUBDEV_MB_SCHEDULER, &sche_ops);
	platform_set_drvdata(pdev, exec);

	DRM_INFO("command scheduler started\n");

	return 0;
}

/**
 * Fini scheduler
 */
static int mb_scheduler_remove(struct platform_device *pdev)
{
	struct xocl_dev *xdev;
	int i;
	struct exec_core *exec = platform_get_drvdata(pdev);

	SCHED_DEBUG("-> mb_scheduler_remove\n");
	fini_scheduler_thread();

	xdev = xocl_get_xdev(pdev);
	for (i = 0; i < exec->intr_num; i++) {
		xocl_user_interrupt_reg(xdev, i + exec->intr_base,
			NULL, NULL);
		xocl_user_interrupt_config(xdev, i + exec->intr_base, false);
	}
	/* ????? should be deref in drm drv*/
#if 0
	for (i=0; i<16; i++) {
		if (exec->user_msix_table[i])
			eventfd_ctx_put(exec->user_msix_table[i]);
	}
#endif

	devm_kfree(&pdev->dev, exec);
	platform_set_drvdata(pdev, NULL);

	SCHED_DEBUG("<- mb_scheduler_remove\n");
	DRM_INFO("command scheduler removed\n");
	return 0;
}

static struct platform_device_id mb_sche_id_table[] = {
	{ XOCL_MB_SCHEDULER, 0 },
	{ },
};

static struct platform_driver	mb_scheduler_driver = {
	.probe		= mb_scheduler_probe,
	.remove		= mb_scheduler_remove,
	.driver		= {
		.name = "xocl_mb_sche",
	},
	.id_table	= mb_sche_id_table,
};

int __init xocl_init_mb_scheduler(void)
{
	return platform_driver_register(&mb_scheduler_driver);
}

void xocl_fini_mb_scheduler(void)
{
	SCHED_DEBUG("-> xocl_fini_mb_scheduler\n");
	platform_driver_unregister(&mb_scheduler_driver);
	SCHED_DEBUG("<- xocl_fini_mb_scheduler\n");
}
