// SPDX-License-Identifier: GPL-2.0
/*
 * Runtime symbol resolution for MemKernel.
 *
 * Estratégia:
 *   1. Registrar um kprobe em "kallsyms_lookup_name" para obter seu endereço
 *      (funciona em kernels >= 4.x com CONFIG_KPROBES=y, mesmo que o símbolo
 *      não seja exportado a partir do 5.7).
 *   2. Usar kallsyms_lookup_name() para resolver os símbolos problemáticos.
 *   3. Expor ponteiros globais consumidos via helpers inline em resolve.h.
 *
 * Símbolos resolvidos:
 *   - pte_offset_kernel   raramente exportado em vendor kernels
 *   - high_memory         variável global às vezes não exportada
 *   - __put_task_struct   CRC mismatch em vendor kernels (struct task_struct)
 *   - file_path           CRC mismatch em vendor kernels (struct file)
 */

#include "resolve.h"

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/version.h>

/* ------------------------------------------------------------------ */
/* Ponteiros exportados                                                 */
/* ------------------------------------------------------------------ */

fn_pte_offset_kernel_t  k_pte_offset_kernel  = NULL;
void                  **k_high_memory        = NULL;
fn___put_task_struct_t  k___put_task_struct  = NULL;
fn_file_path_t          k_file_path          = NULL;
fn_get_task_mm_t        k_get_task_mm        = NULL;
fn_mmput_t              k_mmput              = NULL;
fn_find_get_pid_t       k_find_get_pid       = NULL;
fn_get_pid_task_t       k_get_pid_task       = NULL;
fn_put_pid_t            k_put_pid            = NULL;

/* ------------------------------------------------------------------ */
/* Obter kallsyms_lookup_name via kprobe                               */
/* ------------------------------------------------------------------ */

typedef unsigned long (*kallsyms_lookup_name_fn)(const char *name);
static kallsyms_lookup_name_fn my_kallsyms_lookup_name = NULL;

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)
/* Exportado até 5.6; a partir do 5.7 não é mais EXPORT_SYMBOL */
extern unsigned long kallsyms_lookup_name(const char *name);
#endif

static int get_kallsyms_lookup_name(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
	/* Declarações no topo — C89 não permite declarar após código */
	struct kprobe kp;
	int ret;
#endif

	/* Já resolvido — não reregistrar o kprobe */
	if (my_kallsyms_lookup_name)
		return 0;

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)
	/* Antes do 5.7 o símbolo ainda é exportado — usamos direto. */
	my_kallsyms_lookup_name = (kallsyms_lookup_name_fn)kallsyms_lookup_name;
	return 0;
#else
	/*
	 * 5.7+: registramos um kprobe temporário para obter o endereço.
	 * Inicializamos aqui (não na declaração) pois a declaração ficou no topo.
	 */
	memset(&kp, 0, sizeof(kp));
	kp.symbol_name = "kallsyms_lookup_name";

	ret = register_kprobe(&kp);
	if (ret < 0) {
		pr_err("[memkernel] register_kprobe(kallsyms_lookup_name) failed: %d\n", ret);
		return ret;
	}

	my_kallsyms_lookup_name = (kallsyms_lookup_name_fn)kp.addr;
	unregister_kprobe(&kp);
	return 0;
#endif
}

/* ------------------------------------------------------------------ */
/* Resolução de cada símbolo                                           */
/* ------------------------------------------------------------------ */

static void resolve_pte_offset_kernel(void)
{
	unsigned long addr = my_kallsyms_lookup_name("pte_offset_kernel");
	if (addr) {
		k_pte_offset_kernel = (fn_pte_offset_kernel_t)addr;
		pr_info("[memkernel] pte_offset_kernel resolved @ %lx\n", addr);
	} else {
		/*
		 * Em muitos kernels pte_offset_kernel é uma macro/inline e não
		 * aparece no kallsyms. Nesses casos o linker já a resolveu em
		 * compile-time; memory.c usa pte_offset_kernel diretamente como
		 * macro quando k_pte_offset_kernel for NULL.
		 */
		pr_info("[memkernel] pte_offset_kernel not in kallsyms — using compile-time macro\n");
	}
}

static void resolve_high_memory(void)
{
	unsigned long addr = my_kallsyms_lookup_name("high_memory");
	if (addr) {
		k_high_memory = (void **)addr;
		pr_info("[memkernel] high_memory resolved @ %lx\n", addr);
	} else {
		pr_warn("[memkernel] high_memory not found — skipping phys range check\n");
	}
}

/*
 * __put_task_struct: normalmente exportado, mas em vendor kernels que
 * modificam task_struct o CRC diverge → "disagrees about version" err -22.
 * Resolvendo via kallsyms o símbolo não entra na seção __versions do ELF,
 * eliminando a checagem de CRC por completo.
 */
static void resolve___put_task_struct(void)
{
	unsigned long addr = my_kallsyms_lookup_name("__put_task_struct");
	if (addr) {
		k___put_task_struct = (fn___put_task_struct_t)addr;
		pr_info("[memkernel] __put_task_struct resolved @ %lx\n", addr);
	} else {
		pr_warn("[memkernel] __put_task_struct not found in kallsyms\n");
	}
}

/*
 * file_path: mesmo problema de CRC quando o vendor modifica struct file.
 */
static void resolve_file_path(void)
{
	unsigned long addr = my_kallsyms_lookup_name("file_path");
	if (addr) {
		k_file_path = (fn_file_path_t)addr;
		pr_info("[memkernel] file_path resolved @ %lx\n", addr);
	} else {
		pr_warn("[memkernel] file_path not found in kallsyms\n");
	}
}

/*
 * get_task_mm / mmput / find_get_pid / get_pid_task / put_pid:
 * Em vendor kernels (Qualcomm, MediaTek) que modificam task_struct ou mm_struct,
 * o CRC desses símbolos diverge e o insmod falha com err -22.
 * Resolvendo via kallsyms o CRC não entra no .ko.
 */
#define RESOLVE_SYM(var, type, name)                                        \
	do {                                                                \
		unsigned long _a = my_kallsyms_lookup_name(name);           \
		if (_a) {                                                   \
			var = (type)_a;                                     \
			pr_info("[memkernel] " name " resolved @ %lx\n", _a); \
		} else {                                                    \
			pr_warn("[memkernel] " name " not in kallsyms — using export\n"); \
		}                                                           \
	} while (0)

static void resolve_mm_symbols(void)
{
	RESOLVE_SYM(k_get_task_mm,  fn_get_task_mm_t,   "get_task_mm");
	RESOLVE_SYM(k_mmput,        fn_mmput_t,          "mmput");
	RESOLVE_SYM(k_find_get_pid, fn_find_get_pid_t,   "find_get_pid");
	RESOLVE_SYM(k_get_pid_task, fn_get_pid_task_t,   "get_pid_task");
	RESOLVE_SYM(k_put_pid,      fn_put_pid_t,        "put_pid");
}

/* ------------------------------------------------------------------ */
/* Ponto de entrada                                                     */
/* ------------------------------------------------------------------ */

int resolve_symbols(void)
{
	int ret;

	ret = get_kallsyms_lookup_name();
	if (ret < 0) {
		pr_err("[memkernel] could not obtain kallsyms_lookup_name: %d\n", ret);
		return ret;
	}
	if (!my_kallsyms_lookup_name) {
		pr_err("[memkernel] kallsyms_lookup_name is NULL\n");
		return -EFAULT;
	}

	resolve_pte_offset_kernel();
	resolve_high_memory();
	resolve___put_task_struct();
	resolve_file_path();
	resolve_mm_symbols();

	return 0;
}
