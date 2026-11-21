ifneq ($(KERNELRELEASE),)
    obj-$(CONFIG_MEMKERNEL) := memkernel.o
    memkernel-y := memory.o process.o entry.o resolve.o

    # Sem stack protector: __stack_chk_guard não exportado em vendor kernels
    ccflags-y += -fno-stack-protector
    # Sem LTO conflitos com inline de resolve.h
    ccflags-y += -fno-lto
    # Alinha ao GKI build (Clang em GKI 5.10+)
    ccflags-y += -Wno-unused-variable
else
default:
    ifeq ($(KDIR),)
        $(error KDIR is not set.)
    endif
    make -C $(KDIR) M=$(PWD) CONFIG_MEMKERNEL=m modules
clean:
    rm -f *.ko *.o *.mod.o *.mod.c *.symvers .*.cmd Module.symvers modules.order
endif
