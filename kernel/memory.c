#include "memory.h"
#include "resolve.h"
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/highmem.h>   /* kmap / kunmap */

#include <asm/page.h>
#include <asm/pgtable.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0))
#include <linux/mmap_lock.h>
#define MM_READ_LOCK(mm)   mmap_read_lock(mm)
#define MM_READ_UNLOCK(mm) mmap_read_unlock(mm)
#else
#include <linux/rwsem.h>
#define MM_READ_LOCK(mm)   down_read(&(mm)->mmap_sem)
#define MM_READ_UNLOCK(mm) up_read(&(mm)->mmap_sem)
#endif

/* ================================================================
 * Tradução de endereço virtual → físico via page table walk.
 *
 * Invisível para o processo-alvo: não usa access_process_vm nem
 * qualquer syscall path monitorável.
 *
 * Suporta:
 *   - 4KB pages (PTE-level)   — ARM64 padrão GKI
 *   - Huge pages PMD (2 MB)   — usadas por motores Unity/Unreal
 *   - Huge pages PUD (1 GB)   — raro, mas tratado
 *   - 4-level (P4D) e 3-level page tables
 * ================================================================ */

static phys_addr_t translate_linear_address(struct mm_struct *mm, uintptr_t va)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	pgd = pgd_offset(mm, va);
	if (pgd_none(*pgd) || pgd_bad(*pgd))
		return 0;

	/* P4D — pode ser folded (macro no-op) em ARM64 com 4-level paging */
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

	/* Huge page PMD (2 MB) — comum em jogos modernos */
	if (pmd_trans_huge(*pmd)) {
		phys_addr_t base = (phys_addr_t)(pmd_pfn(*pmd) << PAGE_SHIFT);
		return base + (va & ~PMD_MASK);
	}
	if (pmd_bad(*pmd))
		return 0;

	/*
	 * PTE-level: k_pte_offset_kernel resolvido em runtime via kallsyms.
	 * Se for NULL, pte_offset_kernel é uma macro do kernel headers e
	 * memory.c a chama diretamente pelo helper inline de resolve.h.
	 */
	pte = memk_pte_offset_kernel(pmd, va);
	if (!pte)
		pte = pte_offset_kernel(pmd, va);   /* macro compile-time fallback */
	if (!pte || pte_none(*pte) || !pte_present(*pte))
		return 0;

	return (phys_addr_t)(pte_pfn(*pte) << PAGE_SHIFT) + (va & (PAGE_SIZE - 1));
}

/* ================================================================
 * Leitura/escrita de uma página física via kmap.
 *
 * kmap(pfn_to_page()) é a forma correta de acessar páginas de DRAM
 * em ARM64/GKI. Nunca usa ioremap (destinado a MMIO, falha em DRAM
 * nos kernels modernos).
 * ================================================================ */

static size_t rw_physical_page(phys_addr_t pa, void __user *ubuf,
				size_t size, bool iswrite)
{
	unsigned long pfn    = pa >> PAGE_SHIFT;
	size_t        offset = pa & (PAGE_SIZE - 1);
	struct page  *pg;
	void         *kaddr;

	if (!pfn_valid(pfn))
		return 0;

	pg    = pfn_to_page(pfn);
	kaddr = kmap(pg);
	if (!kaddr)
		return 0;

	if (iswrite) {
		if (copy_from_user(kaddr + offset, ubuf, size)) {
			kunmap(pg);
			return 0;
		}
	} else {
		if (copy_to_user(ubuf, kaddr + offset, size)) {
			kunmap(pg);
			return 0;
		}
	}

	kunmap(pg);
	return size;
}

/* ================================================================
 * Interface pública
 * ================================================================ */

ssize_t readwrite_process_memory(pid_t pid, uintptr_t addr,
				 void *buffer, size_t size, bool iswrite)
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
		phys_addr_t pa;
		size_t      chunk;
		size_t      written;
		uintptr_t   cur_addr = addr + count;
		void __user *cur_buf = (void __user *)buffer + count;

		pa = translate_linear_address(mm, cur_addr);
		if (!pa)
			break;

		/* não ultrapassa boundary de página */
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
