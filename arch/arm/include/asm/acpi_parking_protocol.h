#ifndef __ASM_ARM_ACPI_PARKING_PROTOCOL_H
#define __ASM_ARM_ACPI_PARKING_PROTOCOL_H

#include <linux/of.h>

extern const struct smp_operations acpi_parking_protocol_ops;

static inline bool acpi_parking_protocol_available(void)
{
	return of_machine_is_compatible("microsoft,surface-rt-efi");
}

#endif /* __ASM_ARM_ACPI_PARKING_PROTOCOL_H */
