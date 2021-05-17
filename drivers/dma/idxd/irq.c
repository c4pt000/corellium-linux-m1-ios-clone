// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2019 Intel Corporation. All rights rsvd. */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/dmaengine.h>
#include <uapi/linux/idxd.h>
#include "../dmaengine.h"
#include "idxd.h"
#include "registers.h"

enum irq_work_type {
	IRQ_WORK_NORMAL = 0,
	IRQ_WORK_PROCESS_FAULT,
};

struct idxd_fault {
	struct work_struct work;
	u64 addr;
	struct idxd_device *idxd;
};

static int irq_process_work_list(struct idxd_irq_entry *irq_entry,
				 enum irq_work_type wtype,
				 int *processed, u64 data);
static int irq_process_pending_llist(struct idxd_irq_entry *irq_entry,
				     enum irq_work_type wtype,
				     int *processed, u64 data);

static void idxd_device_reinit(struct work_struct *work)
{
	struct idxd_device *idxd = container_of(work, struct idxd_device, work);
	struct device *dev = &idxd->pdev->dev;
	int rc, i;

	idxd_device_reset(idxd);
	rc = idxd_device_config(idxd);
	if (rc < 0)
		goto out;

	rc = idxd_device_enable(idxd);
	if (rc < 0)
		goto out;

	for (i = 0; i < idxd->max_wqs; i++) {
		struct idxd_wq *wq = &idxd->wqs[i];

		if (wq->state == IDXD_WQ_ENABLED) {
			rc = idxd_wq_enable(wq);
			if (rc < 0) {
				dev_warn(dev, "Unable to re-enable wq %s\n",
					 dev_name(&wq->conf_dev));
			}
		}
	}

	return;

 out:
	idxd_device_wqs_clear_state(idxd);
}

static void idxd_device_fault_work(struct work_struct *work)
{
	struct idxd_fault *fault = container_of(work, struct idxd_fault, work);
	struct idxd_irq_entry *ie;
	int i;
	int processed;
	int irqcnt = fault->idxd->num_wq_irqs + 1;

	for (i = 1; i < irqcnt; i++) {
		ie = &fault->idxd->irq_entries[i];
		irq_process_work_list(ie, IRQ_WORK_PROCESS_FAULT,
				      &processed, fault->addr);
		if (processed)
			break;

		irq_process_pending_llist(ie, IRQ_WORK_PROCESS_FAULT,
					  &processed, fault->addr);
		if (processed)
			break;
	}

	kfree(fault);
}

static int idxd_device_schedule_fault_process(struct idxd_device *idxd,
					      u64 fault_addr)
{
	struct idxd_fault *fault;

	fault = kmalloc(sizeof(*fault), GFP_ATOMIC);
	if (!fault)
		return -ENOMEM;

	fault->addr = fault_addr;
	fault->idxd = idxd;
	INIT_WORK(&fault->work, idxd_device_fault_work);
	queue_work(idxd->wq, &fault->work);
	return 0;
}

irqreturn_t idxd_irq_handler(int vec, void *data)
{
	struct idxd_irq_entry *irq_entry = data;
	struct idxd_device *idxd = irq_entry->idxd;

	idxd_mask_msix_vector(idxd, irq_entry->id);
	return IRQ_WAKE_THREAD;
}

irqreturn_t idxd_misc_thread(int vec, void *data)
{
	struct idxd_irq_entry *irq_entry = data;
	struct idxd_device *idxd = irq_entry->idxd;
	struct device *dev = &idxd->pdev->dev;
	union gensts_reg gensts;
	u32 cause, val = 0;
	int i;
	bool err = false;

	cause = ioread32(idxd->reg_base + IDXD_INTCAUSE_OFFSET);
	iowrite32(cause, idxd->reg_base + IDXD_INTCAUSE_OFFSET);

	if (cause & IDXD_INTC_ERR) {
		spin_lock_bh(&idxd->dev_lock);
		for (i = 0; i < 4; i++)
			idxd->sw_err.bits[i] = ioread64(idxd->reg_base +
					IDXD_SWERR_OFFSET + i * sizeof(u64));
		iowrite64(IDXD_SWERR_ACK, idxd->reg_base + IDXD_SWERR_OFFSET);

		if (idxd->sw_err.valid && idxd->sw_err.wq_idx_valid) {
			int id = idxd->sw_err.wq_idx;
			struct idxd_wq *wq = &idxd->wqs[id];

			if (wq->type == IDXD_WQT_USER)
				wake_up_interruptible(&wq->idxd_cdev.err_queue);
		} else {
			int i;

			for (i = 0; i < idxd->max_wqs; i++) {
				struct idxd_wq *wq = &idxd->wqs[i];

				if (wq->type == IDXD_WQT_USER)
					wake_up_interruptible(&wq->idxd_cdev.err_queue);
			}
		}

		spin_unlock_bh(&idxd->dev_lock);
		val |= IDXD_INTC_ERR;

		for (i = 0; i < 4; i++)
			dev_warn(dev, "err[%d]: %#16.16llx\n",
				 i, idxd->sw_err.bits[i]);
		err = true;
	}

	if (cause & IDXD_INTC_CMD) {
		val |= IDXD_INTC_CMD;
		complete(idxd->cmd_done);
	}

	if (cause & IDXD_INTC_OCCUPY) {
		/* Driver does not utilize occupancy interrupt */
		val |= IDXD_INTC_OCCUPY;
	}

	if (cause & IDXD_INTC_PERFMON_OVFL) {
		/*
		 * Driver does not utilize perfmon counter overflow interrupt
		 * yet.
		 */
		val |= IDXD_INTC_PERFMON_OVFL;
	}

	val ^= cause;
	if (val)
		dev_warn_once(dev, "Unexpected interrupt cause bits set: %#x\n",
			      val);

	if (!err)
		goto out;

	/*
	 * This case should rarely happen and typically is due to software
	 * programming error by the driver.
	 */
	if (idxd->sw_err.valid &&
	    idxd->sw_err.desc_valid &&
	    idxd->sw_err.fault_addr)
		idxd_device_schedule_fault_process(idxd, idxd->sw_err.fault_addr);

	gensts.bits = ioread32(idxd->reg_base + IDXD_GENSTATS_OFFSET);
	if (gensts.state == IDXD_DEVICE_STATE_HALT) {
		idxd->state = IDXD_DEV_HALTED;
		if (gensts.reset_type == IDXD_DEVICE_RESET_SOFTWARE) {
			/*
			 * If we need a software reset, we will throw the work
			 * on a system workqueue in order to allow interrupts
			 * for the device command completions.
			 */
			INIT_WORK(&idxd->work, idxd_device_reinit);
			queue_work(idxd->wq, &idxd->work);
		} else {
			spin_lock_bh(&idxd->dev_lock);
			idxd_device_wqs_clear_state(idxd);
			dev_err(&idxd->pdev->dev,
				"idxd halted, need %s.\n",
				gensts.reset_type == IDXD_DEVICE_RESET_FLR ?
				"FLR" : "system reset");
			spin_unlock_bh(&idxd->dev_lock);
		}
	}

 out:
	idxd_unmask_msix_vector(idxd, irq_entry->id);
	return IRQ_HANDLED;
}

static bool process_fault(struct idxd_desc *desc, u64 fault_addr)
{
	/*
	 * Completion address can be bad as well. Check fault address match for descriptor
	 * and completion address.
	 */
	if ((u64)desc->hw == fault_addr ||
	    (u64)desc->completion == fault_addr) {
		idxd_dma_complete_txd(desc, IDXD_COMPLETE_DEV_FAIL);
		return true;
	}

	return false;
}

static bool complete_desc(struct idxd_desc *desc)
{
	if (desc->completion->status) {
		idxd_dma_complete_txd(desc, IDXD_COMPLETE_NORMAL);
		return true;
	}

	return false;
}

static int irq_process_pending_llist(struct idxd_irq_entry *irq_entry,
				     enum irq_work_type wtype,
				     int *processed, u64 data)
{
	struct idxd_desc *desc, *t;
	struct llist_node *head;
	int queued = 0;
	bool completed = false;
	unsigned long flags;

	*processed = 0;
	head = llist_del_all(&irq_entry->pending_llist);
	if (!head)
		goto out;

	llist_for_each_entry_safe(desc, t, head, llnode) {
		if (wtype == IRQ_WORK_NORMAL)
			completed = complete_desc(desc);
		else if (wtype == IRQ_WORK_PROCESS_FAULT)
			completed = process_fault(desc, data);

		if (completed) {
			idxd_free_desc(desc->wq, desc);
			(*processed)++;
			if (wtype == IRQ_WORK_PROCESS_FAULT)
				break;
		} else {
			spin_lock_irqsave(&irq_entry->list_lock, flags);
			list_add_tail(&desc->list,
				      &irq_entry->work_list);
			spin_unlock_irqrestore(&irq_entry->list_lock, flags);
			queued++;
		}
	}

 out:
	return queued;
}

static int irq_process_work_list(struct idxd_irq_entry *irq_entry,
				 enum irq_work_type wtype,
				 int *processed, u64 data)
{
	struct list_head *node, *next;
	int queued = 0;
	bool completed = false;
	unsigned long flags;

	*processed = 0;
	spin_lock_irqsave(&irq_entry->list_lock, flags);
	if (list_empty(&irq_entry->work_list))
		goto out;

	list_for_each_safe(node, next, &irq_entry->work_list) {
		struct idxd_desc *desc =
			container_of(node, struct idxd_desc, list);

		spin_unlock_irqrestore(&irq_entry->list_lock, flags);
		if (wtype == IRQ_WORK_NORMAL)
			completed = complete_desc(desc);
		else if (wtype == IRQ_WORK_PROCESS_FAULT)
			completed = process_fault(desc, data);

		if (completed) {
			spin_lock_irqsave(&irq_entry->list_lock, flags);
			list_del(&desc->list);
			spin_unlock_irqrestore(&irq_entry->list_lock, flags);
			idxd_free_desc(desc->wq, desc);
			(*processed)++;
			if (wtype == IRQ_WORK_PROCESS_FAULT)
				return queued;
		} else {
			queued++;
		}
		spin_lock_irqsave(&irq_entry->list_lock, flags);
	}

 out:
	spin_unlock_irqrestore(&irq_entry->list_lock, flags);
	return queued;
}

static int idxd_desc_process(struct idxd_irq_entry *irq_entry)
{
	int rc, processed, total = 0;

	/*
	 * There are two lists we are processing. The pending_llist is where
	 * submmiter adds all the submitted descriptor after sending it to
	 * the workqueue. It's a lockless singly linked list. The work_list
	 * is the common linux double linked list. We are in a scenario of
	 * multiple producers and a single consumer. The producers are all
	 * the kernel submitters of descriptors, and the consumer is the
	 * kernel irq handler thread for the msix vector when using threaded
	 * irq. To work with the restrictions of llist to remain lockless,
	 * we are doing the following steps:
	 * 1. Iterate through the work_list and process any completed
	 *    descriptor. Delete the completed entries during iteration.
	 * 2. llist_del_all() from the pending list.
	 * 3. Iterate through the llist that was deleted from the pending list
	 *    and process the completed entries.
	 * 4. If the entry is still waiting on hardware, list_add_tail() to
	 *    the work_list.
	 * 5. Repeat until no more descriptors.
	 */
	do {
		rc = irq_process_work_list(irq_entry, IRQ_WORK_NORMAL,
					   &processed, 0);
		total += processed;
		if (rc != 0)
			continue;

		rc = irq_process_pending_llist(irq_entry, IRQ_WORK_NORMAL,
					       &processed, 0);
		total += processed;
	} while (rc != 0);

	return total;
}

irqreturn_t idxd_wq_thread(int irq, void *data)
{
	struct idxd_irq_entry *irq_entry = data;
	int processed;

	processed = idxd_desc_process(irq_entry);
	idxd_unmask_msix_vector(irq_entry->idxd, irq_entry->id);

	if (processed == 0)
		return IRQ_NONE;

	return IRQ_HANDLED;
}
