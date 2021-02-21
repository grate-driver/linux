#ifndef __ASM_ARM_ACPI_PARKING_PROTOCOL_H
#define __ASM_ARM_ACPI_PARKING_PROTOCOL_H

#include <linux/of.h>
#include <linux/smp.h>

#ifdef CONFIG_SMP
extern const struct smp_operations acpi_parking_protocol_ops;
#endif

static inline bool acpi_parking_protocol_available(void)
{
	return IS_ENABLED(CONFIG_SMP) && of_machine_is_compatible("microsoft,surface-rt-efi");
}

#endif /* __ASM_ARM_ACPI_PARKING_PROTOCOL_H */
