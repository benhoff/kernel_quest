// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/kfifo.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/string.h>

#include "monster_game.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("you");
MODULE_DESCRIPTION("Kernel Caretakers — cooperative kernel critter simulation");

static unsigned int tick_ms = 250;
static struct delayed_work tick_work;
static bool tick_work_ready;

static unsigned int clamp_tick_ms(unsigned int v)
{
	unsigned int min = jiffies_to_msecs(1);
	unsigned int max = 60 * 1000;

	if (v == 0)
		return 0;
	if (v < min)
		return min;
	if (v > max)
		return max;
	return v;
}

static int set_tick_ms(const char *val, const struct kernel_param *kp)
{
	unsigned int v = tick_ms;
	int ret = kstrtouint(val, 0, &v);

	if (ret)
		return ret;

	v = clamp_tick_ms(v);
	*(unsigned int *)kp->arg = v;

	if (!READ_ONCE(tick_work_ready))
		return 0;

	if (v == 0) {
		cancel_delayed_work_sync(&tick_work);
		pr_info("monster: ticks paused (tick_ms=0)\n");
	} else {
		unsigned long delay = msecs_to_jiffies(v);

		if (!delay)
			delay = 1;
		mod_delayed_work(system_wq, &tick_work, delay);
		pr_info("monster: tick interval set to %u ms (min=%u ms)\n",
			v, max(1u, jiffies_to_msecs(1)));
	}
	return 0;
}

static const struct kernel_param_ops tick_ops = {
	.set = set_tick_ms,
	.get = param_get_uint,
};

module_param_cb(tick_ms, &tick_ops, &tick_ms, 0644);
MODULE_PARM_DESC(tick_ms, "Game tick interval in ms (0=paused; clamped to [1 jiffy, 60s])");

static void monster_cleanup_session(struct monster_session *s)
{
	kfifo_free(&s->out);
	kfree(s);
}

static void monster_tick_work(struct work_struct *work)
{
	unsigned int next;
	bool crashed = monster_game_tick();

	next = READ_ONCE(tick_ms);
	if (crashed || !next)
		return;

	{
		unsigned long delay = msecs_to_jiffies(next);
		if (!delay)
			delay = 1;
		schedule_delayed_work(&tick_work, delay);
	}
}

static int monster_open(struct inode *ino, struct file *filp)
{
	struct monster_session *s;
	int ret;

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	INIT_LIST_HEAD(&s->list);
	mutex_init(&s->out_lock);
	init_waitqueue_head(&s->wq);

	ret = kfifo_alloc(&s->out, MONSTER_FIFO_SZ, GFP_KERNEL);
	if (ret) {
		kfree(s);
		return ret;
	}

	s->filp = filp;
	filp->private_data = s;

	ret = monster_game_session_start(s);
	if (ret) {
		kfifo_free(&s->out);
		kfree(s);
		return ret;
	}

	return 0;
}

static int monster_release(struct inode *ino, struct file *filp)
{
	struct monster_session *s = filp->private_data;

	monster_game_session_stop(s);
	kfifo_free(&s->out);
	kfree(s);
	return 0;
}

static ssize_t monster_read(struct file *filp, char __user *ubuf, size_t len,
			    loff_t *ppos)
{
	struct monster_session *s = filp->private_data;
	unsigned int copied = 0;
	int ret;

	if (!len)
		return 0;

	ret = wait_event_interruptible(s->wq, kfifo_len(&s->out) > 0);
	if (ret)
		return ret;

	mutex_lock(&s->out_lock);
	ret = kfifo_to_user(&s->out, ubuf, len, &copied);
	mutex_unlock(&s->out_lock);

	return ret ? ret : copied;
}

static ssize_t monster_write(struct file *filp, const char __user *ubuf, size_t len,
			     loff_t *ppos)
{
	struct monster_session *s = filp->private_data;
	size_t n = min(len, sizeof(s->inbuf) - 1 - s->inlen);
	unsigned int events = MONSTER_GAME_EVENT_NONE;

	if (copy_from_user(s->inbuf + s->inlen, ubuf, n))
		return -EFAULT;

	s->inlen += n;
	s->inbuf[s->inlen] = 0;

	{
		char *start = s->inbuf;
		char *nl;

		while ((nl = strpbrk(start, "\n"))) {
			*nl = 0;
			events |= monster_game_handle_line(s, start);
			start = nl + 1;
		}

		n = s->inbuf + s->inlen - start;
		if (n && start != s->inbuf)
			memmove(s->inbuf, start, n);
		s->inlen = n;
	}

	if (events & MONSTER_GAME_EVENT_RESET) {
		unsigned int next = READ_ONCE(tick_ms);
		if (next) {
			unsigned long delay;

			cancel_delayed_work_sync(&tick_work);
			delay = msecs_to_jiffies(next);
			if (!delay)
				delay = 1;
			schedule_delayed_work(&tick_work, delay);
		}
	}

	return len;
}

static __poll_t monster_poll(struct file *filp, struct poll_table_struct *wait)
{
	struct monster_session *s = filp->private_data;
	__poll_t mask = 0;

	poll_wait(filp, &s->wq, wait);
	if (kfifo_len(&s->out) > 0)
		mask |= EPOLLIN | EPOLLRDNORM;
	return mask;
}

static const struct file_operations monster_fops = {
	.owner   = THIS_MODULE,
	.open    = monster_open,
	.release = monster_release,
	.read    = monster_read,
	.write   = monster_write,
	.poll    = monster_poll,
};

static struct miscdevice monster_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "monster",
	.fops  = &monster_fops,
	.mode  = 0666,
};

static int __init monster_init(void)
{
	int ret;

	ret = monster_game_init();
	if (ret)
		return ret;

	INIT_DELAYED_WORK(&tick_work, monster_tick_work);
	WRITE_ONCE(tick_work_ready, true);

	tick_ms = clamp_tick_ms(tick_ms);
	if (tick_ms) {
		unsigned long delay = msecs_to_jiffies(tick_ms);
		if (!delay)
			delay = 1;
		schedule_delayed_work(&tick_work, delay);
	}

	ret = misc_register(&monster_miscdev);
	if (ret) {
		WRITE_ONCE(tick_work_ready, false);
		cancel_delayed_work_sync(&tick_work);
		monster_game_shutdown_sessions(monster_cleanup_session);
		monster_game_exit();
		return ret;
	}

	pr_info("monster: loaded at /dev/monster (tick_ms=%u)\n", tick_ms);
	return 0;
}

static void __exit monster_exit(void)
{
	WRITE_ONCE(tick_work_ready, false);
	cancel_delayed_work_sync(&tick_work);

	monster_game_shutdown_sessions(monster_cleanup_session);
	misc_deregister(&monster_miscdev);
	monster_game_exit();

	pr_info("monster: unloaded\n");
}

module_init(monster_init);
module_exit(monster_exit);
