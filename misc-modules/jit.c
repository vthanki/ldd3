/*
 * jit.c -- the just-in-time module
 *
 * Copyright (C) 2001,2003 Alessandro Rubini and Jonathan Corbet
 * Copyright (C) 2001,2003 O'Reilly & Associates
 *
 * The source code in this file can be freely used, adapted,
 * and redistributed in source or binary form, so long as an
 * acknowledgment appears in derived source files.  The citation
 * should list that the code comes from the book "Linux Device
 * Drivers" by Alessandro Rubini and Jonathan Corbet, published
 * by O'Reilly & Associates.   No warranty is attached;
 * we cannot take responsibility for errors or fitness for use.
 *
 * $Id: jit.c,v 1.16 2004/09/26 07:02:43 gregkh Exp $
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/time.h>
#include <linux/timer.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/debugfs.h>

#include <asm/hardirq.h>
/*
 * This module is a silly one: it only embeds short code fragments
 * that show how time delays can be handled in the kernel.
 */

int delay = HZ; /* the default delay, expressed in jiffies */

module_param(delay, int, 0);

MODULE_AUTHOR("Alessandro Rubini");
MODULE_LICENSE("Dual BSD/GPL");

/* use these as data pointers, to implement four files in one function */
enum jit_files {
	JIT_BUSY = 0x1,
	JIT_SCHED,
	JIT_QUEUE,
	JIT_SCHEDTO,
	JIT_TIMER,
	JIT_TASKLET,
	JIT_TASKLET_HI
};

/*
 * The timer example follows
 */

int tdelay = 10;
module_param(tdelay, int, 0);

/* This data structure used as "data" for the timer and tasklet functions */
struct jit_data {
	struct timer_list timer;
	struct tasklet_struct tlet;
	int hi; /* tasklet or tasklet_hi */
	wait_queue_head_t wait;
	unsigned long prevjiffies;
	void *priv;
	int loops;
};
#define JIT_ASYNC_LOOPS 5

void jit_timer_fn(unsigned long arg)
{
	struct jit_data *data = (struct jit_data *)arg;
	unsigned long j = jiffies;
	seq_printf((struct seq_file *)data->priv,
			"%9li  %3li     %i    %6i   %i   %s\n",
			 j, j - data->prevjiffies, in_interrupt() ? 1 : 0,
			 current->pid, smp_processor_id(), current->comm);

	if (--data->loops) {
		data->timer.expires += tdelay;
		data->prevjiffies = j;
		add_timer(&data->timer);
	} else {
		wake_up_interruptible(&data->wait);
	}
}

/* the /proc function: allocate everything to allow concurrency */
static int jit_timer(struct seq_file *s, void *unused)
{
	struct jit_data *data;
	unsigned long j = jiffies;

	data = kmalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	init_timer(&data->timer);
	init_waitqueue_head (&data->wait);

	/* write the first lines in the buffer */
	seq_printf(s, "   time   delta  inirq    pid   cpu command\n");
	seq_printf(s, "%9li  %3li     %i    %6i   %i   %s\n",
			j, 0L, in_interrupt() ? 1 : 0,
			current->pid, smp_processor_id(), current->comm);

	/* fill the data for our timer function */
	data->prevjiffies = j;
	data->priv = (void *)s;
	data->loops = JIT_ASYNC_LOOPS;

	/* register the timer */
	data->timer.data = (unsigned long)data;
	data->timer.function = jit_timer_fn;
	data->timer.expires = j + tdelay; /* parameter */
	add_timer(&data->timer);

	/* wait for the buffer to fill */
	wait_event_interruptible(data->wait, !data->loops);
	if (signal_pending(current))
		return -ERESTARTSYS;
	kfree(data);
	return 0;
}

static void jit_tasklet_fn(unsigned long arg)
{
	struct jit_data *data = (struct jit_data *)arg;
	unsigned long j = jiffies;
	seq_printf((struct seq_file *)data->priv,
			"%9li  %3li     %i    %6i   %i   %s\n",
			 j, j - data->prevjiffies, in_interrupt() ? 1 : 0,
			 current->pid, smp_processor_id(), current->comm);

	if (--data->loops) {
		data->prevjiffies = j;
		if (data->hi)
			tasklet_hi_schedule(&data->tlet);
		else
			tasklet_schedule(&data->tlet);
	} else {
		wake_up_interruptible(&data->wait);
	}
}

/* the /proc function: allocate everything to allow concurrency */
static int jit_tasklet(struct seq_file *s, int tasklet_hi)
{
	struct jit_data *data;
	unsigned long j = jiffies;

	data = kmalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	init_waitqueue_head (&data->wait);

	/* write the first lines in the buffer */
	seq_printf(s, "   time   delta  inirq    pid   cpu command\n");
	seq_printf(s, "%9li  %3li     %i    %6i   %i   %s\n",
			j, 0L, in_interrupt() ? 1 : 0,
			current->pid, smp_processor_id(), current->comm);

	/* fill the data for our tasklet function */
	data->prevjiffies = j;
	data->priv = (void *)s;
	data->loops = JIT_ASYNC_LOOPS;

	/* register the tasklet */
	tasklet_init(&data->tlet, jit_tasklet_fn, (unsigned long)data);
	data->hi = tasklet_hi;
	if (tasklet_hi)
		tasklet_hi_schedule(&data->tlet);
	else
		tasklet_schedule(&data->tlet);

	/* wait for the buffer to fill */
	wait_event_interruptible(data->wait, !data->loops);

	if (signal_pending(current))
		return -ERESTARTSYS;
	kfree(data);
	return 0;
}

static int jit_common_show(struct seq_file *s, void *unused)
{
	unsigned long j0, j1; /* jiffies */
	wait_queue_head_t wait;

	init_waitqueue_head (&wait);
	j0 = jiffies;
	j1 = j0 + delay;

	switch((long)s->private) {
		case JIT_BUSY:
			while (time_before(jiffies, j1))
				cpu_relax();
			break;
		case JIT_SCHED:
			while (time_before(jiffies, j1)) {
				schedule();
			}
			break;
		case JIT_QUEUE:
			wait_event_interruptible_timeout(wait, 0, delay);
			break;
		case JIT_SCHEDTO:
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout (delay);
			break;
		case JIT_TIMER:
			jit_timer(s, NULL);
			goto timer_exit;
		case JIT_TASKLET:
			jit_tasklet(s, 0);
			goto tasklet_exit;
		case JIT_TASKLET_HI:
			jit_tasklet(s, 1);
			goto tasklet_hi_exit;

	}
	j1 = jiffies; /* actual value after we delayed */

	seq_printf(s, "%9li %9li\n", j0, j1);

timer_exit:
tasklet_exit:
tasklet_hi_exit:
	return 0;
}

static int jit_show(struct seq_file *s, void *unused)
{
	struct timeval tv1;
	struct timespec tv2;
	unsigned long j1;
	u64 j2;

	if (s->private)
		return 	jit_common_show(s, NULL);

	/* get them four */
	j1 = jiffies;
	j2 = get_jiffies_64();
	do_gettimeofday(&tv1);
	tv2 = current_kernel_time();

	/* print */
	seq_printf(s,"0x%08lx 0x%016Lx %10i.%06i\n"
			"%40i.%09i\n",
			j1, j2,
			(int) tv1.tv_sec, (int) tv1.tv_usec,
			(int) tv2.tv_sec, (int) tv2.tv_nsec);

	return 0;
}


static int jit_open(struct inode *inode, struct file *file)
{
	return single_open(file, jit_show, PDE_DATA(inode));
}

static const struct file_operations jit_ops = {
	.owner = THIS_MODULE,
	.open = jit_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
};


int __init jit_init(void)
{
	proc_create_data("currentime", 0, NULL, &jit_ops, NULL);
	proc_create_data("jitbusy", 0, NULL, &jit_ops, (void *)JIT_BUSY);
	proc_create_data("jitsched", 0, NULL, &jit_ops, (void *)JIT_SCHED);
	proc_create_data("jitqueue", 0, NULL, &jit_ops, (void *)JIT_QUEUE);
	proc_create_data("jitschedto", 0, NULL, &jit_ops, (void *)JIT_SCHEDTO);
	proc_create_data("jitimer", 0, NULL, &jit_ops, (void *)JIT_TIMER);
	proc_create_data("jitasklet", 0, NULL, &jit_ops, (void *)JIT_TASKLET);
	proc_create_data("jitasklethi", 0, NULL, &jit_ops, (void *)JIT_TASKLET_HI);

	return 0; /* success */
}

void __exit jit_cleanup(void)
{
	remove_proc_entry("currentime", NULL);
	remove_proc_entry("jitbusy", NULL);
	remove_proc_entry("jitsched", NULL);
	remove_proc_entry("jitqueue", NULL);
	remove_proc_entry("jitschedto", NULL);
	remove_proc_entry("jitimer", NULL);
	remove_proc_entry("jitasklet", NULL);
	remove_proc_entry("jitasklethi", NULL);
}

module_init(jit_init);
module_exit(jit_cleanup);
