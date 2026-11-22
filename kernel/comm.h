#ifndef MEMKERNEL_COMM_H
#define MEMKERNEL_COMM_H

/*
 * Tamanho máximo de uma operação de R/W.
 * Limita o tempo de posse de mmap_read_lock e previne loops longos.
 * 64 MB é suficiente para qualquer leitura de jogo; ajuste se necessário.
 */
#define MEMK_MAX_RW_SIZE (64UL * 1024 * 1024)

/*
 * As structs são compartilhadas entre kernel e userspace (ARM64 AArch64).
 * Layout ARM64 AArch64 (sem __packed):
 *   CopyMemory: pid(4) + pad(4) + addr(8) + buffer(8) + size(8) = 32 bytes
 *   ModuleBase: pid(4) + pad(4) + name*(8) + base(8) = 24 bytes
 * O userspace deve usar o mesmo layout — não usar __packed nem #pragma pack.
 */

struct CopyMemory {
    pid_t      pid;
    uintptr_t  addr;
    void      *buffer;
    size_t     size;
};

struct ModuleBase {
    pid_t      pid;
    char      *name;
    uintptr_t  base;
};

enum Operations {
    OP_READ_MEM    = 0x801,
    OP_WRITE_MEM   = 0x802,
    OP_MODULE_BASE = 0x803,
};

#endif
