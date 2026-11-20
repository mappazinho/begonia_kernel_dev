// SPDX-License-Identifier: GPL-2.0-only
/*
 * ColorOS 16/17 userspace interface on top of the 4.14 (Realme 7/8, Android S)
 * SchedAssist core.
 *
 * CO17's libSchedAssistExtImpl.so, libart, libgui and oplus-framework expect the
 * newer Oplus interface:
 *   /proc/oplus_scheduler/sched_assist/ux_task      ioctl SET  _IOW(0x27, 1, {pid, tid, value})
 *   /proc/oplus_scheduler/sched_assist/ux_task_app  ioctl READ _IOR(0x27, 0, {pid, tid}) -> ux_state
 *   /proc/oplus_scheduler/sched_assist/im_flag(_app) text "p <tid> <flag>"
 *   ddl_task, audio/status, lb_enable, debug_enabled (accepted, stored)
 *   /proc/oplus_binder/ux_flag                      (accepted, ignored)
 * The ux value keeps the type bits in the low 24 bits and a priority (0..10)
 * in the top byte; the 4.14 core only looks at the low bits.
 */
#include <linux/ioctl.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/cred.h>
#include <linux/capability.h>
#include "sched_assist_common.h"

extern struct proc_dir_entry *d_sched_assist;

/*
 * Knobs that Realme defines in sysctl.c and the WALT-only slide-boost files
 * (not built here). CO17 userspace never writes them; they stay at the defaults.
 */
int sysctl_sched_assist_enabled = 1;
int sysctl_sched_assist_scene;
int sysctl_slide_boost_enabled;
int sysctl_boost_task_threshold = 51;	/* PELT util, of 1024 */
int sysctl_uxchain_v2;

#define SA_MAGIC		0x27
struct sa_ux_ctrl {
	int pid;
	int tid;
	int value;
};
struct sa_ux_read {
	int pid;
	int tid;
};
#define SA_IOC_READ_UX		_IOR(SA_MAGIC, 0, struct sa_ux_read)	/* 0x80082700 */
#define SA_IOC_SET_UX		_IOW(SA_MAGIC, 1, struct sa_ux_ctrl)	/* 0x400c2701 */

#define SA_PRIO_SHIFT		24
#define SA_PRIO_MASK		0xff000000
#define SA_TYPE_BITS		0x00ffffff
#define SA_PRIO_MAX		10

/* im_flag ids used by CO17 (only the ones we act on) */
#define IM_FLAG_SURFACEFLINGER	1
#define IM_FLAG_HWC		2
#define IM_FLAG_RENDERENGINE	3
#define IM_FLAG_CAMERA_HAL	5
#define IM_FLAG_3RD_RENDER	6
#define IM_FLAG_AUDIO		14

static int sa_lb_enable;
static int sa_debug_enabled;
static int sa_audio_status;

/* Callers outside system uids may only touch threads of their own process. */
static bool sa_may_ctrl(struct task_struct *t, bool app)
{
	if (!app && (uid_lt(current_uid(), KUIDT_INIT(10000)) || capable(CAP_SYS_NICE)))
		return true;
	return t->tgid == current->tgid;
}

static struct task_struct *sa_get_thread(int pid, int tid)
{
	struct task_struct *t;

	rcu_read_lock();
	t = find_task_by_vpid(tid);
	if (t && pid > 0 && t->tgid != pid)
		t = NULL;
	if (t)
		get_task_struct(t);
	rcu_read_unlock();
	return t;
}

/* Same semantics as /proc/<pid>/task/<tid>/ux_state on the 4.14 core, plus the priority byte. */
static void sa_apply_ux(struct task_struct *t, int value)
{
	int prio = ((unsigned int)value & SA_PRIO_MASK) >> SA_PRIO_SHIFT;
	int bits = value & SA_TYPE_BITS;
	int state;

	if (prio > SA_PRIO_MAX)
		prio = 0;

	task_lock(t);
	state = t->ux_state & SA_TYPE_BITS;
	if (bits == SA_OPT_CLEAR) {
		/* clear all ux types but animator */
		state &= ~(SA_TYPE_LISTPICK | SA_TYPE_HEAVY | SA_TYPE_LIGHT);
	} else if (bits & SA_OPT_SET) {
		state |= bits & ~SA_OPT_SET;
	} else if (prio && (state & bits) == bits) {
		/* read-modify-write that only changes the priority: keep the types */
	} else if (state & bits) {
		state &= ~bits;
	}
	t->ux_state = state | ((state ? prio : 0) << SA_PRIO_SHIFT);
	task_unlock(t);
	sched_assist_systrace_pid(t->tgid, t->ux_state, "ux_state %d", t->pid);
}

static long sa_ux_ioctl(struct file *file, unsigned int cmd, unsigned long arg, bool app)
{
	void __user *uarg = (void __user *)arg;
	struct task_struct *t;
	long ret;

	if (_IOC_TYPE(cmd) != SA_MAGIC)
		return -EINVAL;

	switch (cmd) {
	case SA_IOC_SET_UX: {
		struct sa_ux_ctrl c;

		if (copy_from_user(&c, uarg, sizeof(c)))
			return -EFAULT;
		if (c.tid <= 0)
			return -EINVAL;
		t = sa_get_thread(c.pid, c.tid);
		if (!t)
			return -ESRCH;
		if (sa_may_ctrl(t, app)) {
			sa_apply_ux(t, c.value);
			ret = 0;
		} else {
			ret = -EPERM;
		}
		put_task_struct(t);
		return ret;
	}
	case SA_IOC_READ_UX: {
		struct sa_ux_read r;

		if (copy_from_user(&r, uarg, sizeof(r)))
			return -EFAULT;
		if (r.tid <= 0)
			return -EINVAL;
		t = sa_get_thread(r.pid, r.tid);
		if (!t)
			return -ESRCH;
		ret = READ_ONCE(t->ux_state) & ~(SA_TYPE_INHERIT | SA_TYPE_ONCE_UX);
		put_task_struct(t);
		return ret;
	}
	default:
		return -ENOTTY;
	}
}

static long sa_ux_task_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	return sa_ux_ioctl(f, cmd, arg, false);
}

static long sa_ux_task_app_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	return sa_ux_ioctl(f, cmd, arg, true);
}

/* Text form, for testing from a shell: "<pid> <tid> <value>" or "p <tid> <value>". */
static ssize_t sa_ux_write(struct file *file, const char __user *buf, size_t count, bool app)
{
	char kbuf[64];
	int pid = 0, tid = 0, value = 0;
	struct task_struct *t;
	char c;

	if (count >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;
	kbuf[count] = '\0';
	if (sscanf(kbuf, "%d %d %d", &pid, &tid, &value) != 3) {
		pid = 0;
		if (sscanf(kbuf, "%c %d %d", &c, &tid, &value) != 3)
			return -EINVAL;
	}
	t = sa_get_thread(pid, tid);
	if (!t)
		return -ESRCH;
	if (!sa_may_ctrl(t, app)) {
		put_task_struct(t);
		return -EPERM;
	}
	sa_apply_ux(t, value);
	put_task_struct(t);
	return count;
}

static ssize_t sa_ux_task_write(struct file *f, const char __user *b, size_t n, loff_t *p)
{
	return sa_ux_write(f, b, n, false);
}

static ssize_t sa_ux_task_app_write(struct file *f, const char __user *b, size_t n, loff_t *p)
{
	return sa_ux_write(f, b, n, true);
}

static const struct file_operations sa_ux_task_fops = {
	.write		= sa_ux_task_write,
	.unlocked_ioctl	= sa_ux_task_ioctl,
	.compat_ioctl	= sa_ux_task_ioctl,
	.llseek		= noop_llseek,
};

static const struct file_operations sa_ux_task_app_fops = {
	.write		= sa_ux_task_app_write,
	.unlocked_ioctl	= sa_ux_task_app_ioctl,
	.compat_ioctl	= sa_ux_task_app_ioctl,
	.llseek		= noop_llseek,
};

/*
 * im_flag: identity of special threads. The 4.14 core has no im_flag, so the
 * render/composition/audio identities become light UX (the core only boosts CFS
 * threads; SCHED_FIFO ones are left alone) and flag 0 drops what we added.
 */
static bool im_flag_is_ux(int flag)
{
	switch (flag) {
	case IM_FLAG_SURFACEFLINGER:
	case IM_FLAG_HWC:
	case IM_FLAG_RENDERENGINE:
	case IM_FLAG_CAMERA_HAL:
	case IM_FLAG_3RD_RENDER:
	case IM_FLAG_AUDIO:
		return true;
	}
	return false;
}

static ssize_t sa_im_flag_write(struct file *file, const char __user *buf, size_t count, bool app)
{
	char kbuf[64];
	int tid = 0, flag = 0;
	struct task_struct *t;
	char c;

	if (count >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;
	kbuf[count] = '\0';
	if (sscanf(kbuf, "%c %d %d", &c, &tid, &flag) != 3 &&
	    sscanf(kbuf, "%d %d", &tid, &flag) != 2)
		return -EINVAL;
	t = sa_get_thread(0, tid);
	if (!t)
		return -ESRCH;
	if (sa_may_ctrl(t, app)) {
		task_lock(t);
		if (im_flag_is_ux(flag)) {
			t->im_flag = flag;
			t->ux_state |= SA_TYPE_LIGHT;
		} else if (flag == 0 && im_flag_is_ux(t->im_flag)) {
			t->im_flag = 0;
			t->ux_state &= ~SA_TYPE_LIGHT;
		} else {
			t->im_flag = flag;
		}
		task_unlock(t);
	}
	put_task_struct(t);
	return count;
}

static ssize_t sa_im_flag_sys_write(struct file *f, const char __user *b, size_t n, loff_t *p)
{
	return sa_im_flag_write(f, b, n, false);
}

static ssize_t sa_im_flag_app_write(struct file *f, const char __user *b, size_t n, loff_t *p)
{
	return sa_im_flag_write(f, b, n, true);
}

static const struct file_operations sa_im_flag_fops = {
	.write	= sa_im_flag_sys_write,
	.llseek	= noop_llseek,
};

static const struct file_operations sa_im_flag_app_fops = {
	.write	= sa_im_flag_app_write,
	.llseek	= noop_llseek,
};

/* Plain integer knobs that CO17 writes; stored so reads give back what was written. */
static int sa_int_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", *(int *)m->private);
	return 0;
}

static int sa_int_open(struct inode *inode, struct file *file)
{
	return single_open(file, sa_int_show, PDE_DATA(inode));
}

static ssize_t sa_int_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	struct seq_file *m = file->private_data;
	char kbuf[16];
	int val;

	if (count >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;
	kbuf[count] = '\0';
	if (kstrtoint(strstrip(kbuf), 0, &val))
		return -EINVAL;
	WRITE_ONCE(*(int *)m->private, val);
	return count;
}

static const struct file_operations sa_int_fops = {
	.open		= sa_int_open,
	.read		= seq_read,
	.write		= sa_int_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* ddl_task and binder ux_flag: accepted and ignored (no 4.14 equivalent). */
static ssize_t sa_sink_write(struct file *f, const char __user *b, size_t n, loff_t *p)
{
	return n;
}

static long sa_sink_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	return 0;
}

static const struct file_operations sa_sink_fops = {
	.write		= sa_sink_write,
	.unlocked_ioctl	= sa_sink_ioctl,
	.compat_ioctl	= sa_sink_ioctl,
	.llseek		= noop_llseek,
};

static int __init sa_co17_init(void)
{
	struct proc_dir_entry *audio, *binder;

	if (!d_sched_assist)
		return -ENOENT;

	proc_create("ux_task", 0666, d_sched_assist, &sa_ux_task_fops);
	proc_create("ux_task_app", 0666, d_sched_assist, &sa_ux_task_app_fops);
	proc_create("im_flag", 0666, d_sched_assist, &sa_im_flag_fops);
	proc_create("im_flag_app", 0666, d_sched_assist, &sa_im_flag_app_fops);
	proc_create("ddl_task", 0666, d_sched_assist, &sa_sink_fops);
	proc_create_data("lb_enable", 0664, d_sched_assist, &sa_int_fops, &sa_lb_enable);
	proc_create_data("debug_enabled", 0664, d_sched_assist, &sa_int_fops, &sa_debug_enabled);
	audio = proc_mkdir("audio", d_sched_assist);
	if (audio)
		proc_create_data("status", 0664, audio, &sa_int_fops, &sa_audio_status);

	binder = proc_mkdir("oplus_binder", NULL);
	if (binder)
		proc_create("ux_flag", 0666, binder, &sa_sink_fops);
	return 0;
}
late_initcall(sa_co17_init);
