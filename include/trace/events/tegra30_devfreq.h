/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM tegra30_devfreq

#if !defined(_TRACE_TEGRA30_DEVFREQ_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_TEGRA30_DEVFREQ_H

#include <linux/io.h>
#include <linux/tracepoint.h>
#include <linux/types.h>

DECLARE_EVENT_CLASS(device_state,
	TP_PROTO(void __iomem *base, u32 offset, u32 boost, u32 cpufreq),
	TP_ARGS(base, offset, boost, cpufreq),
	TP_STRUCT__entry(
		__field(u32, offset)
		__field(u32, intr_status)
		__field(u32, ctrl)
		__field(u32, avg_count)
		__field(u32, avg_lower)
		__field(u32, avg_upper)
		__field(u32, count)
		__field(u32, lower)
		__field(u32, upper)
		__field(u32, boost_freq)
		__field(u32, cpu_freq)
	),
	TP_fast_assign(
		__entry->offset		= offset;
		__entry->intr_status	= readl_relaxed(base + offset + 0x24);
		__entry->ctrl		= readl_relaxed(base + offset + 0x0);
		__entry->avg_count	= readl_relaxed(base + offset + 0x20);
		__entry->avg_lower	= readl_relaxed(base + offset + 0x14);
		__entry->avg_upper	= readl_relaxed(base + offset + 0x10);
		__entry->count		= readl_relaxed(base + offset + 0x1c);
		__entry->lower		= readl_relaxed(base + offset + 0x8);
		__entry->upper		= readl_relaxed(base + offset + 0x4);
		__entry->boost_freq	= boost;
		__entry->cpu_freq	= cpufreq;
	),
	TP_printk("%03x: intr 0x%08x ctrl 0x%08x avg %010u %010u %010u cnt %010u %010u %010u boost %010u cpu %u",
		__entry->offset,
		__entry->intr_status,
		__entry->ctrl,
		__entry->avg_count,
		__entry->avg_lower,
		__entry->avg_upper,
		__entry->count,
		__entry->lower,
		__entry->upper,
		__entry->boost_freq,
		__entry->cpu_freq)
);

DEFINE_EVENT(device_state, device_isr_enter,
	TP_PROTO(void __iomem *base, u32 offset, u32 boost, u32 cpufreq),
	TP_ARGS(base, offset, boost, cpufreq));

DEFINE_EVENT(device_state, device_isr_exit,
	TP_PROTO(void __iomem *base, u32 offset, u32 boost, u32 cpufreq),
	TP_ARGS(base, offset, boost, cpufreq));

DEFINE_EVENT(device_state, device_target_update,
	TP_PROTO(void __iomem *base, u32 offset, u32 boost, u32 cpufreq),
	TP_ARGS(base, offset, boost, cpufreq));

TRACE_EVENT(device_lower_upper,
	TP_PROTO(u32 offset, u32 target, u32 lower, u32 upper),
	TP_ARGS(offset, target, lower, upper),
	TP_STRUCT__entry(
		__field(u32, offset)
		__field(u32, target)
		__field(u32, lower)
		__field(u32, upper)
	),
	TP_fast_assign(
		__entry->offset = offset;
		__entry->target = target;
		__entry->lower = lower;
		__entry->upper = upper;
	),
	TP_printk("%03x: freq %010u lower freq %010u upper freq %010u",
		__entry->offset,
		__entry->target,
		__entry->lower,
		__entry->upper)
);

TRACE_EVENT(device_target_freq,
	TP_PROTO(u32 offset, u32 target),
	TP_ARGS(offset, target),
	TP_STRUCT__entry(
		__field(u32, offset)
		__field(u32, target)
	),
	TP_fast_assign(
		__entry->offset = offset;
		__entry->target = target;
	),
	TP_printk("%03x: freq %010u", __entry->offset, __entry->target)
);
#endif /* _TRACE_TEGRA30_DEVFREQ_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
