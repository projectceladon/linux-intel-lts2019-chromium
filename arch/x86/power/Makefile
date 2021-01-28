# SPDX-License-Identifier: GPL-2.0
OBJECT_FILES_NON_STANDARD_hibernate_asm_$(BITS).o := y

# __restore_processor_state() restores %gs after S3 resume and so should not
# itself be stack-protected
nostackp := $(call cc-option, -fno-stack-protector)
CFLAGS_cpu.o	:= $(nostackp)

# When LTO enabled, the stack-protector code fragment would be still built
# into object file. This would trigger TRIPLE_FAULT when resume from S3.
# So here disable LTO to make resume successful.
CFLAGS_cpu.o	+= -fno-lto -fvisibility=default -fno-sanitize=cfi

obj-$(CONFIG_PM_SLEEP)		+= cpu.o
obj-$(CONFIG_HIBERNATION)	+= hibernate_$(BITS).o hibernate_asm_$(BITS).o hibernate.o
