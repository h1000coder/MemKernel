#ifndef MEMKERNEL_COMM_H
#define MEMKERNEL_COMM_H

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
    OP_READ_MEM    = 0x666,
    OP_WRITE_MEM   = 0x777,
    OP_MODULE_BASE = 0x999,
};

#endif
