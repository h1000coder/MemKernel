ifneq ($(KERNELRELEASE),)
    obj-$(CONFIG_MEMKERNEL) := liberdade.o
    liberdade-y := memory.o process.o entry.o resolve.o

    ccflags-y += -fno-stack-protector
    # Desabilita CFI por arquivo — compatível com build CFI+LTO do DDK
    ccflags-y += -fno-sanitize=cfi
else
default:
    ifeq ($(KDIR),)
        $(error KDIR is not set.)
    endif
    make -C $(KDIR) M=$(PWD) CONFIG_MEMKERNEL=m modules
clean:
    rm -f *.ko *.o *.mod.o *.mod.c *.symvers .*.cmd Module.symvers modules.order
endif
