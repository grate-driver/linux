#ifndef __ASM_ARM_ACPI_PARKING_PROTOCOL_H
#define __ASM_ARM_ACPI_PARKING_PROTOCOL_H

// Assume that we are running on Surface RT

extern const struct smp_operations acpi_parking_protocol_ops;

static inline bool acpi_parking_protocol_available(void)
{
	return IS_ENABLED(CONFIG_ARM_ACPI_PARKING_PROTOCOL);
}

#endif /* __ASM_ARM_ACPI_PARKING_PROTOCOL_H */
