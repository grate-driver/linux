/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_THERMAL_H
#define _ASM_X86_THERMAL_H

/* Interrupt Handler for package thermal thresholds */
extern int (*platform_thermal_package_notify)(__u64 msr_val);

/* Interrupt Handler for core thermal thresholds */
extern int (*platform_thermal_notify)(__u64 msr_val);

/* Callback support of rate control, return true, if
 * callback has rate control */
extern bool (*platform_thermal_package_rate_control)(void);

#ifdef CONFIG_X86_THERMAL_VECTOR
void intel_init_thermal(struct cpuinfo_x86 *c);
#else
static inline void intel_init_thermal(struct cpuinfo_x86 *c) { }
#endif

#endif /* _ASM_X86_THERMAL_H */
