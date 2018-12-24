obj-$(CONFIG_FS_VERITY)	+= fsverity.o

fsverity-y := hash_algs.o ioctl.o setup.o verify.o

fsverity-$(CONFIG_FS_VERITY_BUILTIN_SIGNATURES) += signature.o
