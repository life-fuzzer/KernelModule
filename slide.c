/*************************************************************************
	> File Name: slide.c
	> Author: 
	> Mail: 
	> Created Time: 2016年01月16日 星期六 11时06分26秒
 ************************************************************************/
#include "slide.h"
#include <linux/mm.h>
#include <linux/hugetlb.h>
#include <linux/shm.h>
#include <linux/mman.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/security.h>
#include <linux/mempolicy.h>
#include <linux/personality.h>
#include <linux/syscalls.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/mmu_notifier.h>
#include <linux/migrate.h>
#include <linux/perf_event.h>
#include <linux/ksm.h>
#include <asm/uaccess.h>
#include <asm/pgtable.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>
/*
 * task_struct task->mm->mmap->vm_start,vm_end,vm_flags
 * mm_struct->mmap_rb read black tree
 * mm_struct->pgd -> pmd_t -> pte_t
 */

int init_task_vma(struct task_struct *task)
{
    int start_code = task->mm->start_code;
    int end_code = task->mm->end_code;
    int start_data = task->mm->start_data;
    int end_data = task->mm->end_data;
    int start_brk = task->mm->start_brk;
    //int brk = task->mm->brk;
    int start_stack = task->mm->start_stack;
    int i;
    unsigned long  vm_start,vm_end, vm_flags, prot;

    printk("code start addr: 0x%lu\t,end addr: 0x%lu\n",start_code,end_code);
    static struct vm_area_struct *vma;
    static struct vm_area_struct *p;
    struct vm_area_struct *q;
    vma = task->mm->mmap;
    p = vma;
    for (i=0;i<VMACACHE_SIZE;i++) {
        q = task->vmacache[i];
        printk("task caching 0x%lu->0x%lu\n",q->vm_start,q->vm_end);
    }
    while (p->vm_prev) {
        p = p->vm_prev;
    }
    while (p->vm_next) {
        printk("vm_start:0x%lu, vm_end:0x%lu\t",p->vm_start,p->vm_end);
        if (p->vm_flags & VM_READ) {
            printk("R");
        } else {
            printk("-");
        }
        if (p->vm_flags & VM_WRITE) {
            printk("W");
        } else { printk("-"); }
        if (p->vm_flags & VM_EXEC) {
            printk("X");
            vm_start = p->vm_start;
            vm_end = p->vm_end;
            vm_flags = p->vm_flags;
            vm_flags &= !(VM_EXEC);
            changeProt(vm_start, vm_end-vm_start,vm_flags,task);
        } else{ printk("-"); }
        printk("\t");
        if (p->vm_end < end_code && p->vm_start >start_code ) {
            printk("code page");
        } else if (p->vm_end < end_data && p->vm_start > start_data) {
            printk("data page");
        } else if (p->vm_start > start_brk) {
            printk("heap page");
        } else if (p->vm_start > start_stack) {
            printk("stack page");
        }
        printk("\n");
        p = p->vm_next;
    }
    printk("task %s total mapped vm %lu, total %lu vmas can exec\n",task->comm,task->mm->total_vm,task->mm->exec_vm);

    
    return 0;
}

int changeProt(unsigned long start, size_t len, unsigned long prot, struct task_struct *task)
{
    unsigned long vm_flags, nstart, end, tmp, reqprot;
	struct vm_area_struct *vma, *prev;
	int error = -EINVAL;
	const int grows = prot & (PROT_GROWSDOWN|PROT_GROWSUP);
	prot &= ~(PROT_GROWSDOWN|PROT_GROWSUP);
	if (grows == (PROT_GROWSDOWN|PROT_GROWSUP)) /* can't be both */
		return -EINVAL;

	if (start & ~PAGE_MASK)
		return -EINVAL;
	if (!len)
		return 0;
	len = PAGE_ALIGN(len);
	end = start + len;
	if (end <= start)
		return -ENOMEM;
	if (!arch_validate_prot(prot))
		return -EINVAL;

	reqprot = prot;
	/*
	 * Does the application expect PROT_READ to imply PROT_EXEC:
	 */
	if ((prot & PROT_READ) && (task->personality & READ_IMPLIES_EXEC))
		prot |= PROT_EXEC;

	vm_flags = calc_vm_prot_bits(prot);

	down_write(&task->mm->mmap_sem);

	vma = find_vma(task->mm, start);
	error = -ENOMEM;
	if (!vma)
		goto out;
	prev = vma->vm_prev;
	if (unlikely(grows & PROT_GROWSDOWN)) {
		if (vma->vm_start >= end)
			goto out;
		start = vma->vm_start;
		error = -EINVAL;
		if (!(vma->vm_flags & VM_GROWSDOWN))
			goto out;
	} else {
		if (vma->vm_start > start)
			goto out;
		if (unlikely(grows & PROT_GROWSUP)) {
			end = vma->vm_end;
			error = -EINVAL;
			if (!(vma->vm_flags & VM_GROWSUP))
				goto out;
		}
	}
	if (start > vma->vm_start)
		prev = vma;

	for (nstart = start ; ; ) {
		unsigned long newflags;

		/* Here we know that vma->vm_start <= nstart < vma->vm_end. */

		newflags = vm_flags;
		newflags |= (vma->vm_flags & ~(VM_READ | VM_WRITE | VM_EXEC));

		/* newflags >> 4 shift VM_MAY% in place of VM_% */
		if ((newflags & ~(newflags >> 4)) & (VM_READ | VM_WRITE | VM_EXEC)) {
			error = -EACCES;
			goto out;
		}

		error = security_file_mprotect(vma, reqprot, prot);
		if (error)
			goto out;

		tmp = vma->vm_end;
		if (tmp > end)
			tmp = end;
		error = mprotect_fixup(vma, &prev, nstart, tmp, newflags);
		if (error)
			goto out;
		nstart = tmp;

		if (nstart < prev->vm_end)
			nstart = prev->vm_end;
		if (nstart >= end)
			goto out;

		vma = prev->vm_next;
		if (!vma || vma->vm_start != nstart) {
			error = -ENOMEM;
			goto out;
		}
	}
out:
	up_write(&task->mm->mmap_sem);
	return error;
}

