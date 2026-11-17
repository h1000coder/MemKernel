#ifndef MEMKERNEL_RESOLVE_H
#define MEMKERNEL_RESOLVE_H

#include <linux/types.h>
#include <linux/mm_types.h>
#include <linux/pgtable.h>
#include <asm/io.h>

/*
 * Runtime symbol resolution via kprobes + kallsyms_lookup_name.
 *
 * kallsyms_lookup_name() deixou de ser EXPORT_SYMBOL no 5.7+, mas ainda existe
 * no binário do kernel; usamos um kprobe no próprio símbolo para obter o
 * endereço e depois resolvemos os demais.
 *
 * Estratégia de R/W: acesso físico direto via page table walk + kmap.
 * Não usa access_process_vm (visível em traces do kernel) nem ioremap
 * (errado para DRAM em GKI 5.10+). O processo-alvo não tem nenhum
 * indício de que sua memória foi lida.
 *
 * Símbolos resolvidos em runtime:
 *   - pte_offset_kernel  (raramente exportado em vendor kernels)
 *   - high_memory        (variável global, às vezes não exportada)
 *   - __put_task_struct  (CRC mismatch em vendor kernels)
 *   - file_path          (CRC mismatch em vendor kernels)
 */

/* Tipos dos ponteiros de função */
typedef pte_t *(*fn_pte_offset_kernel_t)(pmd_t *pmd, unsigned long address);
typedef void  (*fn___put_task_struct_t)(struct task_struct *t);
typedef char *(*fn_file_path_t)(struct file *f, char *buf, int buflen);

/* Ponteiros resolvidos em tempo de execução */
extern fn_pte_offset_kernel_t  k_pte_offset_kernel;
extern void                  **k_high_memory;
extern fn___put_task_struct_t  k___put_task_struct;
extern fn_file_path_t          k_file_path;

/*
 * __nocfi: desativa a checagem de CFI (Clang Control Flow Integrity) para
 * funções que chamam ponteiros resolvidos em runtime via kallsyms.
 * Obrigatório em GKI 5.10+ compilado com Clang CFI (praticamente todos).
 * Em compiladores sem suporte ao atributo, expande para nada.
 */
#if defined(__clang__) && defined(CONFIG_CFI_CLANG)
# define MEMK_NOCFI __attribute__((__no_sanitize__("cfi")))
#else
# define MEMK_NOCFI
#endif

/*
 * Chame isso no module_init, antes de qualquer uso dos símbolos acima.
 * Retorna 0 em sucesso ou código de erro negativo.
 */
int resolve_symbols(void);

/*
 * Helpers inline que fazem fallback seguro quando um símbolo não foi
 * resolvido (evita NULL-deref e facilita a leitura no código que os usa).
 */
static inline pte_t *memk_pte_offset_kernel(pmd_t *pmd, unsigned long addr)
{
	if (k_pte_offset_kernel)
		return k_pte_offset_kernel(pmd, addr);
	/*
	 * Se pte_offset_kernel for uma macro no kernel atual (caso comum em
	 * kernels mais antigos), ela ainda está disponível como inline.
	 * Nesse caminho chegamos apenas se o símbolo externo não foi
	 * encontrado; retornar NULL fará translate_linear_address() abortar.
	 */
	return NULL;
}

static inline void *memk_high_memory(void)
{
	if (k_high_memory)
		return *k_high_memory;
	return (void *)ULONG_MAX; /* conservador: valida tudo como inválido */
}

/*
 * memk_put_task_struct — substitui put_task_struct() para remover a
 * dependência de CRC em __put_task_struct do ELF do módulo.
 *
 * put_task_struct() é um inline que faz:
 *   if (refcount_dec_and_test(&t->usage)) __put_task_struct(t);
 * Replicamos isso aqui; refcount_dec_and_test é puramente inline e não
 * gera símbolo externo.
 */
static MEMK_NOCFI inline void memk_put_task_struct(struct task_struct *t)
{
	if (!k___put_task_struct) {
		/* resolução falhou — chama o original e aceita o risco de CRC */
		put_task_struct(t);
		return;
	}
	if (refcount_dec_and_test(&t->usage))
		k___put_task_struct(t);
}

/*
 * memk_file_path — substitui file_path() para remover a dependência de CRC.
 * Fallback retorna string vazia (não casa com nenhum nome de módulo).
 */
static MEMK_NOCFI inline char *memk_file_path(struct file *f, char *buf, int buflen)
{
	if (k_file_path)
		return k_file_path(f, buf, buflen);
	if (buflen > 0)
		buf[0] = '\0';
	return buf;
}

#endif /* MEMKERNEL_RESOLVE_H */
