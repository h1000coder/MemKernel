#include "memory.h"
#include "resolve.h"
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/highmem.h>

#include <asm/page.h>
#include <asm/pgtable.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
#include <linux/mmap_lock.h>
#define MM_READ_LOCK(mm)   mmap_read_lock(mm)
#define MM_READ_UNLOCK(mm) mmap_read_unlock(mm)
#else
#include <linux/rwsem.h>
#define MM_READ_LOCK(mm)   down_read(&(mm)->mmap_sem)
#define MM_READ_UNLOCK(mm) up_read(&(mm)->mmap_sem)
#endif

/*
 * kmap_local_page foi introduzido no 5.14 e é o preferido nos kernels GKI
 * mais novos (não pode dormir na região mapeada, mais eficiente em SMP).
 * Em 5.10–5.13 usamos kmap que ainda é totalmente suportado.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
# define MEMK_MAP(pg)    kmap_local_page(pg)
# define MEMK_UNMAP(pg)  kunmap_local(kaddr)   /* kunmap_local recebe o ptr */
  /* kunmap_local precisa do endereço retornado por kmap_local_page, não da page */
# undef  MEMK_UNMAP
# define MEMK_UNMAP_ADDR(addr) kunmap_local(addr)
#else
# define MEMK_MAP(pg)          kmap(pg)
# define MEMK_UNMAP_ADDR(addr) kunmap(pg)
#endif

/* ================================================================
 * Tradução de endereço virtual → físico via page table walk.
 * ================================================================ */

static phys_addr_t translate_linear_address(struct mm_struct *mm, uintptr_t va)
{
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;
    pmd_t *pmd;

    pgd = pgd_offset(mm, va);
    if (pgd_none(*pgd) || pgd_bad(*pgd))
        return 0;

    p4d = p4d_offset(pgd, va);
    if (p4d_none(*p4d) || p4d_bad(*p4d))
        return 0;

    pud = pud_offset(p4d, va);
    if (pud_none(*pud))
        return 0;

    /* Huge page PUD (1 GB) */
    if (pud_trans_huge(*pud)) {
        phys_addr_t base = (phys_addr_t)(pud_pfn(*pud) << PAGE_SHIFT);
        return base + (va & ~PUD_MASK);
    }
    if (pud_bad(*pud))
        return 0;

    pmd = pmd_offset(pud, va);
    if (pmd_none(*pmd))
        return 0;

    /* Huge page PMD (2 MB) */
    if (pmd_trans_huge(*pmd)) {
        phys_addr_t base = (phys_addr_t)(pmd_pfn(*pmd) << PAGE_SHIFT);
        return base + (va & ~PMD_MASK);
    }
    if (pmd_bad(*pmd))
        return 0;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
    /*
     * 6.1+: pte_offset_map_lock() é a API correta para PTEs de processos.
     * Lemos o PFN enquanto seguramos o spinlock e soltamos imediatamente.
     */
    {
        spinlock_t *ptl;
        pte_t *pte = pte_offset_map_lock(mm, pmd, va, &ptl);
        phys_addr_t pa = 0;

        if (pte && !pte_none(*pte) && pte_present(*pte))
            pa = (phys_addr_t)(pte_pfn(*pte) << PAGE_SHIFT) + (va & (PAGE_SIZE - 1));

        pte_unmap_unlock(pte, ptl);
        return pa;
    }
#else
    {
        pte_t *pte = memk_pte_offset_kernel(pmd, va);
        if (!pte)
            pte = pte_offset_kernel(pmd, va);
        if (!pte || pte_none(*pte) || !pte_present(*pte))
            return 0;
        return (phys_addr_t)(pte_pfn(*pte) << PAGE_SHIFT) + (va & (PAGE_SIZE - 1));
    }
#endif
}

/* ================================================================
 * Leitura/escrita de uma página física.
 * ================================================================ */

static size_t rw_physical_page(phys_addr_t pa, void __user *ubuf,
                               size_t size, bool iswrite)
{
    unsigned long pfn    = pa >> PAGE_SHIFT;
    size_t        offset = pa & (PAGE_SIZE - 1);
    struct page  *pg;
    void         *kaddr;
    int           err;

    if (!pfn_valid(pfn))
        return 0;

    pg    = pfn_to_page(pfn);
    kaddr = MEMK_MAP(pg);
    if (!kaddr)
        return 0;

    if (iswrite)
        err = copy_from_user(kaddr + offset, ubuf, size);
    else
        err = copy_to_user(ubuf, kaddr + offset, size);

    MEMK_UNMAP_ADDR(kaddr);
    return err ? 0 : size;
}

/* ================================================================
 * Interface pública
 * ================================================================ */

ssize_t readwrite_process_memory(pid_t pid, uintptr_t addr,
                                 void __user *buffer, size_t size, bool iswrite)
{
    struct task_struct *task;
    struct mm_struct   *mm;
    struct pid         *pid_struct;
    size_t              count = 0;

    if (size == 0 || !buffer)
        return -EINVAL;

    pid_struct = find_get_pid(pid);
    if (!pid_struct)
        return -ESRCH;

    task = get_pid_task(pid_struct, PIDTYPE_PID);
    put_pid(pid_struct);
    if (!task)
        return -ESRCH;

    mm = get_task_mm(task);
    memk_put_task_struct(task);
    if (!mm)
        return -ESRCH;

    MM_READ_LOCK(mm);
    while (count < size) {
        phys_addr_t  pa;
        size_t       chunk;
        size_t       written;
        uintptr_t    cur_addr = addr + count;
        void __user *cur_buf  = buffer + count;

        pa = translate_linear_address(mm, cur_addr);
        if (!pa)
            break;

        chunk = PAGE_SIZE - (cur_addr & (PAGE_SIZE - 1));
        if (chunk > size - count)
            chunk = size - count;

        written = rw_physical_page(pa, cur_buf, chunk, iswrite);
        if (!written)
            break;

        count += written;
    }
    MM_READ_UNLOCK(mm);
    mmput(mm);

    return count > 0 ? (ssize_t)count : -1;
}
