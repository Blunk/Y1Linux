// SPDX-License-Identifier: GPL-2.0-only
/* Innioasis Y1 (MT6572) core parking: the Y2's coordinator (kernel/platform/system-idle.c) with Y1 inputs,
 * docs/planning/Y1-POWER-PLAN.md §17 D6a. The decision is the Y2's unchanged system-idle-policy.h: CPU1
 * parks after a sustained 30 s quiet window while the screen is dark and no audio stream runs; input, a
 * lit screen, an audio start or one second of saturation restores it, with the escalating hold.
 * Only coordinator-owned offlines are restored; a CPU taken offline by hand is left alone.
 * Inputs: the backlight's dark state (y2_backlight_dark), the Y1 AFE's stream lease
 * (y1_system_idle_lease, atomic context), input events, cpufreq transitions. Stock's deep idle needs
 * exactly one online CPU; this is the slow decision that gives it that, never made in the idle path. */
#include <linux/bitops.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/input.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <linux/tick.h>
#include <linux/workqueue.h>
#include "system-idle.h"
#include "system-idle-policy.h"
#define Y1_IDLE_CPUS 2
static DEFINE_MUTEX(idle_lock);
static struct workqueue_struct *idle_wq;
static bool enabled = true, paused, broken;
static unsigned parked_mask, park_count, restore_count, busy_percent;
static int last_error;
static u64 sample_idle[Y1_IDLE_CPUS], sample_wall[Y1_IDLE_CPUS];
static atomic_t demand = ATOMIC_INIT(0), frequency_raises = ATOMIC_INIT(0), leases = ATOMIC_INIT(0);
static struct y2_idle_state state;
static unsigned long last_sample;
static const struct kernel_param_ops enable_ops;
module_param_cb(enabled, &enable_ops, &enabled, 0600);
module_param(parked_mask, uint, 0400);
module_param(park_count, uint, 0400);
module_param(restore_count, uint, 0400);
module_param(busy_percent, uint, 0400);
module_param(last_error, int, 0400);
module_param(broken, bool, 0400);
static unsigned now_ms(void)
{
	return jiffies_to_msecs(jiffies);
}
static unsigned idle_cpus(void)
{
	return min_t(unsigned, nr_cpu_ids, Y1_IDLE_CPUS);
}
/* Restores only coordinator-owned CPUs. Manual offlines are never claimed. */
static int restore_locked(bool pressure)
{
	unsigned cpu;
	int ret, first = 0;
	for (cpu = 1; cpu < idle_cpus(); cpu++)
		if (parked_mask & BIT(cpu)) {
			ret = cpu_online(cpu) ? 0 : add_cpu(cpu);
			if (ret < 0) {
				if (!first)
					first = ret;
				broken = true;
			} else {
				parked_mask &= ~BIT(cpu);
				restore_count++;
			}
		}
	y2_idle_restored(&state, now_ms(), pressure);
	state.parked = hweight32(parked_mask);
	last_error = first;
	return first;
}
int y2_system_idle_restore(void)
{
	int ret;
	if (!idle_wq)
		return 0;
	atomic_set(&demand, 1);
	mutex_lock(&idle_lock);
	ret = restore_locked(false);
	y2_idle_wake(&state, now_ms());
	mutex_unlock(&idle_lock);
	return ret;
}
static void restore_work_fn(struct work_struct *work)
{
	y2_system_idle_restore();
}
static DECLARE_WORK(restore_work, restore_work_fn);
void y2_system_idle_activity(void)
{
	atomic_set(&demand, 1);
	if (idle_wq && READ_ONCE(parked_mask))
		queue_work(idle_wq, &restore_work);
}
/* The AFE's playback (bit 0) and FM/capture (bit 1) streams, from its trigger (atomic context). A start
 * is demand: a parked CPU comes back from a work item, without waiting for the next sample. */
void y1_system_idle_lease(unsigned bit, bool on)
{
	if (on) {
		atomic_or(bit, &leases);
		y2_system_idle_activity();
	} else {
		atomic_andnot(bit, &leases);
	}
}
/* Deep idle's entry condition (deep-idle.c, D6c): the screen dark and no AFE stream. Read in the idle path. */
bool y1_system_idle_quiet(void)
{
	return y2_backlight_dark() && !atomic_read(&leases);
}
/* Busy per-mille of the online capacity since the previous sample. */
static unsigned sample_busy(void)
{
	unsigned cpu;
	u64 total = 0, idle = 0, wall, value, delta;
	for_each_online_cpu(cpu) {
		if (cpu >= Y1_IDLE_CPUS)
			continue;
		value = get_cpu_idle_time_us(cpu, &wall);
		if (value == -1ULL)
			return 1000;
		if (sample_wall[cpu] && wall > sample_wall[cpu]) {
			delta = wall - sample_wall[cpu];
			total += delta;
			idle += min(value - sample_idle[cpu], delta);
		}
		sample_idle[cpu] = value;
		sample_wall[cpu] = wall;
	}
	return total ? div64_u64((total - idle) * 1000, total) : 1000;
}
static void sample_work_fn(struct work_struct *work);
/* Deferrable: a fully idle CPU is not woken merely to observe idleness; any
 * real load wakes a CPU and runs the sample with a time-weighted interval. */
static DECLARE_DEFERRABLE_WORK(sample_work, sample_work_fn);
static void sample_work_fn(struct work_struct *work)
{
	struct y2_idle_sample in;
	enum y2_idle_reason why;
	unsigned cpu, busy;
	mutex_lock(&idle_lock);
	busy = sample_busy();
	busy_percent = busy / 10;
	in = (struct y2_idle_sample){
		.now_ms = now_ms(),
		.dt_ms = last_sample ? jiffies_to_msecs(jiffies - last_sample) : 250,
		.busy_permille = busy,
		.online = min(num_online_cpus(), idle_cpus()),
		.khz = cpufreq_quick_get(0),
		.dark = y2_backlight_dark(),
		.lease = atomic_read(&leases) != 0,
		.timer = 1, /* D4: the arch timer with the GPT broadcast; no Y2 local-timer handoff */
		.allowed = enabled && !broken && !paused,
		.demand = atomic_xchg(&demand, 0),
	};
	last_sample = jiffies;
	if (in.demand)
		y2_idle_wake(&state, in.now_ms);
	switch (y2_idle_decide(&state, &in, &why)) {
	case Y2_IDLE_RESTORE:
		restore_locked(why == Y2_IDLE_BURST);
		break;
	case Y2_IDLE_PARK:
		/* One CPU per step, highest first. Recheck producer demand before the
		 * slow offline; a race during remove_cpu restores immediately after. */
		for (cpu = idle_cpus() - 1; cpu > 0; cpu--)
			if (cpu_online(cpu)) {
				if (atomic_read(&demand) || atomic_read(&leases))
					break;
				last_error = remove_cpu(cpu);
				if (last_error < 0) {
					broken = true;
					restore_locked(false);
					break;
				}
				parked_mask |= BIT(cpu);
				park_count++;
				y2_idle_parked(&state, in.now_ms);
				if (atomic_read(&demand) || atomic_read(&leases))
					restore_locked(false);
				break;
			}
		break;
	case Y2_IDLE_WAIT:
		break;
	}
	mutex_unlock(&idle_lock);
	queue_delayed_work(idle_wq, &sample_work, msecs_to_jiffies(250));
}
/* Why the coordinator is (not) parking: sustained averages, window state and
 * every quiet-window reset by reason, for physical source attribution. */
static int state_get(char *buffer, const struct kernel_param *kp)
{
	unsigned i, now;
	int n;
	mutex_lock(&idle_lock);
	now = now_ms();
	n = sysfs_emit(buffer, "quiet=%d quiet_ms=%u longest_quiet_ms=%u load_mc=%u high_freq_permille=%u burst_ms=%u hold_ms=%u hold_remaining_ms=%u parked=%u parked_mask=%#x leases=%#x last_reset=%s pressure_restores=%u frequency_raises=%u",
		state.quiet, state.quiet ? now - state.quiet_start_ms : 0,
		state.longest_quiet_ms, state.load_mc, state.high_permille,
		state.burst_ms, state.hold_ms,
		y2_idle_after(now, state.hold_until_ms) ? 0 : state.hold_until_ms - now,
		state.parked, parked_mask, atomic_read(&leases), y2_idle_reason_names[state.last_reset],
		state.pressure_restores, atomic_read(&frequency_raises));
	for (i = 1; i < Y2_IDLE_REASONS; i++)
		n += sysfs_emit_at(buffer, n, " reset_%s=%u", y2_idle_reason_names[i], state.resets[i]);
	n += sysfs_emit_at(buffer, n, " wake_reclassified=%u\n", state.wake_reclassified);
	mutex_unlock(&idle_lock);
	return n;
}
static const struct kernel_param_ops state_ops = { .get = state_get };
module_param_cb(state, &state_ops, NULL, 0400);
static int enabled_set(const char *value, const struct kernel_param *kp)
{
	int ret = param_set_bool(value, kp);
	if (!ret && !enabled)
		ret = y2_system_idle_restore();
	return ret;
}
static const struct kernel_param_ops enable_ops = { .set = enabled_set,
						    .get = param_get_bool };
static int frequency_event(struct notifier_block *nb, unsigned long event,
			   void *data)
{
	struct cpufreq_freqs *f = data;
	/* A transient schedutil/iowait raise is not demand by itself; sustained
	 * saturation is measured by the sampler. Counted for attribution only. */
	if (event == CPUFREQ_PRECHANGE && f->new > Y2_SYSTEM_LOW_KHZ)
		atomic_inc(&frequency_raises);
	return NOTIFY_OK;
}
static struct notifier_block frequency_nb = { .notifier_call =
						      frequency_event };
/* Pausing under idle_lock also ends a park the sampler is in the middle of,
 * so no CPU is parked after this point. A CPU that will not come back is no
 * reason to refuse sleep: freezing the secondaries copes with offline CPUs. */
static int suspend_event(struct notifier_block *nb, unsigned long event,
			 void *data)
{
	int ret;
	if (event == PM_SUSPEND_PREPARE) {
		mutex_lock(&idle_lock);
		paused = true;
		ret = restore_locked(false);
		mutex_unlock(&idle_lock);
		if (ret)
			pr_warn("Y1IDLE: restore before suspend result=%d; suspending anyway\n", ret);
	} else if (event == PM_POST_SUSPEND) {
		mutex_lock(&idle_lock);
		paused = false;
		y2_idle_restored(&state, now_ms(), false);
		mutex_unlock(&idle_lock);
	}
	return NOTIFY_OK;
}
/* Above cpu_hotplug_pm_callback (0): at equal priority it ran first and
 * disabled hotplug, so add_cpu() for a CPU parked just before suspend got
 * -EBUSY and the refused notifier failed the suspend with -EPERM. */
static struct notifier_block suspend_nb = { .notifier_call = suspend_event,
					    .priority = 1 };
static void idle_input_event(struct input_handle *handle, unsigned type,
			     unsigned code, int value)
{
	if (type != EV_SYN)
		y2_system_idle_activity();
}
static int input_connect(struct input_handler *handler, struct input_dev *dev,
			 const struct input_device_id *id)
{
	struct input_handle *h = kzalloc(sizeof(*h), GFP_KERNEL);
	int ret;
	if (!h)
		return -ENOMEM;
	h->dev = dev;
	h->handler = handler;
	h->name = "y1-system-idle";
	ret = input_register_handle(h);
	if (ret) {
		kfree(h);
		return ret;
	}
	ret = input_open_device(h);
	if (ret) {
		input_unregister_handle(h);
		kfree(h);
	}
	return ret;
}
static void input_disconnect(struct input_handle *h)
{
	input_close_device(h);
	input_unregister_handle(h);
	kfree(h);
}
static const struct input_device_id input_ids[] = { { .driver_info = 1 }, {} };
static struct input_handler input_handler = { .event = idle_input_event,
					      .connect = input_connect,
					      .disconnect = input_disconnect,
					      .name = "y1-system-idle",
					      .id_table = input_ids };
static int __init system_idle_init(void)
{
	int ret;
	if (!of_machine_is_compatible("innioasis,y1"))
		return -ENODEV;
	idle_wq = alloc_workqueue("y1-idle-demand",
				  WQ_UNBOUND | WQ_HIGHPRI | WQ_FREEZABLE, 1);
	if (!idle_wq)
		return -ENOMEM;
	ret = cpufreq_register_notifier(&frequency_nb,
					CPUFREQ_TRANSITION_NOTIFIER);
	if (ret)
		goto fail;
	ret = register_pm_notifier(&suspend_nb);
	if (ret)
		goto frequency;
	ret = input_register_handler(&input_handler);
	if (ret)
		goto pm;
	y2_idle_init(&state, now_ms());
	queue_delayed_work(idle_wq, &sample_work, msecs_to_jiffies(250));
	return 0;
pm:
	unregister_pm_notifier(&suspend_nb);
frequency:
	cpufreq_unregister_notifier(&frequency_nb, CPUFREQ_TRANSITION_NOTIFIER);
fail:
	destroy_workqueue(idle_wq);
	idle_wq = NULL;
	return ret;
}
device_initcall(system_idle_init);
MODULE_LICENSE("GPL");
