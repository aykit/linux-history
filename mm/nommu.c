/*
 *  linux/mm/nommu.c
 *
 *  Replacement code for mm functions to support CPU's that don't
 *  have any form of memory management unit (thus no virtual memory).
 *
 *  Copyright (c) 2004      David Howells <dhowells@redhat.com>
 *  Copyright (c) 2000-2003 David McCullough <davidm@snapgear.com>
 *  Copyright (c) 2000-2001 D Jeff Dionne <jeff@uClinux.org>
 *  Copyright (c) 2002      Greg Ungerer <gerg@snapgear.com>
 */

#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/swap.h>
#include <linux/file.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/ptrace.h>
#include <linux/blkdev.h>
#include <linux/backing-dev.h>
#include <linux/syscalls.h>

#include <asm/uaccess.h>
#include <asm/tlb.h>
#include <asm/tlbflush.h>

void *high_memory;
struct page *mem_map;
unsigned long max_mapnr;
unsigned long num_physpages;
unsigned long askedalloc, realalloc;
atomic_t vm_committed_space = ATOMIC_INIT(0);
int sysctl_overcommit_memory = OVERCOMMIT_GUESS; /* heuristic overcommit */
int sysctl_overcommit_ratio = 50; /* default is 50% */
int sysctl_max_map_count = DEFAULT_MAX_MAP_COUNT;

EXPORT_SYMBOL(sysctl_max_map_count);
EXPORT_SYMBOL(mem_map);

/* list of shareable VMAs */
LIST_HEAD(nommu_vma_list);
DECLARE_RWSEM(nommu_vma_sem);

void __init prio_tree_init(void)
{
}

/*
 * Handle all mappings that got truncated by a "truncate()"
 * system call.
 *
 * NOTE! We have to be ready to update the memory sharing
 * between the file and the memory map for a potential last
 * incomplete page.  Ugly, but necessary.
 */
int vmtruncate(struct inode *inode, loff_t offset)
{
	struct address_space *mapping = inode->i_mapping;
	unsigned long limit;

	if (inode->i_size < offset)
		goto do_expand;
	i_size_write(inode, offset);

	truncate_inode_pages(mapping, offset);
	goto out_truncate;

do_expand:
	limit = current->signal->rlim[RLIMIT_FSIZE].rlim_cur;
	if (limit != RLIM_INFINITY && offset > limit)
		goto out_sig;
	if (offset > inode->i_sb->s_maxbytes)
		goto out;
	i_size_write(inode, offset);

out_truncate:
	if (inode->i_op && inode->i_op->truncate)
		inode->i_op->truncate(inode);
	return 0;
out_sig:
	send_sig(SIGXFSZ, current, 0);
out:
	return -EFBIG;
}

EXPORT_SYMBOL(vmtruncate);

/*
 * Return the total memory allocated for this pointer, not
 * just what the caller asked for.
 *
 * Doesn't have to be accurate, i.e. may have races.
 */
unsigned int kobjsize(const void *objp)
{
	struct page *page;

	if (!objp || !((page = virt_to_page(objp))))
		return 0;

	if (PageSlab(page))
		return ksize(objp);

	BUG_ON(page->index < 0);
	BUG_ON(page->index >= MAX_ORDER);

	return (PAGE_SIZE << page->index);
}

/*
 * The nommu dodgy version :-)
 */
int get_user_pages(struct task_struct *tsk, struct mm_struct *mm,
	unsigned long start, int len, int write, int force,
	struct page **pages, struct vm_area_struct **vmas)
{
	int i;
	static struct vm_area_struct dummy_vma;

	for (i = 0; i < len; i++) {
		if (pages) {
			pages[i] = virt_to_page(start);
			if (pages[i])
				page_cache_get(pages[i]);
		}
		if (vmas)
			vmas[i] = &dummy_vma;
		start += PAGE_SIZE;
	}
	return(i);
}

rwlock_t vmlist_lock = RW_LOCK_UNLOCKED;
struct vm_struct *vmlist;

void vfree(void *addr)
{
	kfree(addr);
}

void *__vmalloc(unsigned long size, int gfp_mask, pgprot_t prot)
{
	/*
	 * kmalloc doesn't like __GFP_HIGHMEM for some reason
	 */
	return kmalloc(size, gfp_mask & ~__GFP_HIGHMEM);
}

struct page * vmalloc_to_page(void *addr)
{
	return virt_to_page(addr);
}

unsigned long vmalloc_to_pfn(void *addr)
{
	return page_to_pfn(virt_to_page(addr));
}


long vread(char *buf, char *addr, unsigned long count)
{
	memcpy(buf, addr, count);
	return count;
}

long vwrite(char *buf, char *addr, unsigned long count)
{
	/* Don't allow overflow */
	if ((unsigned long) addr + count < count)
		count = -(unsigned long) addr;

	memcpy(addr, buf, count);
	return(count);
}

/*
 *	vmalloc  -  allocate virtually continguos memory
 *
 *	@size:		allocation size
 *
 *	Allocate enough pages to cover @size from the page level
 *	allocator and map them into continguos kernel virtual space.
 *
 *	For tight cotrol over page level allocator and protection flags
 *	use __vmalloc() instead.
 */
void *vmalloc(unsigned long size)
{
       return __vmalloc(size, GFP_KERNEL | __GFP_HIGHMEM, PAGE_KERNEL);
}

/*
 *	vmalloc_32  -  allocate virtually continguos memory (32bit addressable)
 *
 *	@size:		allocation size
 *
 *	Allocate enough 32bit PA addressable pages to cover @size from the
 *	page level allocator and map them into continguos kernel virtual space.
 */
void *vmalloc_32(unsigned long size)
{
	return __vmalloc(size, GFP_KERNEL, PAGE_KERNEL);
}

void *vmap(struct page **pages, unsigned int count, unsigned long flags, pgprot_t prot)
{
	BUG();
	return NULL;
}

void vunmap(void *addr)
{
	BUG();
}

/*
 *  sys_brk() for the most part doesn't need the global kernel
 *  lock, except when an application is doing something nasty
 *  like trying to un-brk an area that has already been mapped
 *  to a regular file.  in this case, the unmapping will need
 *  to invoke file system routines that need the global lock.
 */
asmlinkage unsigned long sys_brk(unsigned long brk)
{
	struct mm_struct *mm = current->mm;

	if (brk < mm->start_brk || brk > mm->context.end_brk)
		return mm->brk;

	if (mm->brk == brk)
		return mm->brk;

	/*
	 * Always allow shrinking brk
	 */
	if (brk <= mm->brk) {
		mm->brk = brk;
		return brk;
	}

	/*
	 * Ok, looks good - let it rip.
	 */
	return mm->brk = brk;
}

/*
 * Combine the mmap "prot" and "flags" argument into one "vm_flags" used
 * internally. Essentially, translate the "PROT_xxx" and "MAP_xxx" bits
 * into "VM_xxx".
 */
static inline unsigned long calc_vm_flags(unsigned long prot, unsigned long flags)
{
#define _trans(x,bit1,bit2) \
((bit1==bit2)?(x&bit1):(x&bit1)?bit2:0)

	unsigned long prot_bits, flag_bits;
	prot_bits =
		_trans(prot, PROT_READ, VM_READ) |
		_trans(prot, PROT_WRITE, VM_WRITE) |
		_trans(prot, PROT_EXEC, VM_EXEC);
	flag_bits =
		_trans(flags, MAP_GROWSDOWN, VM_GROWSDOWN) |
		_trans(flags, MAP_DENYWRITE, VM_DENYWRITE) |
		_trans(flags, MAP_EXECUTABLE, VM_EXECUTABLE);
	return prot_bits | flag_bits;
#undef _trans
}

#ifdef DEBUG
static void show_process_blocks(void)
{
	struct mm_tblock_struct *tblock;

	printk("Process blocks %d:", current->pid);

	for (tblock = &current->mm->context.tblock; tblock; tblock = tblock->next) {
		printk(" %p: %p", tblock, tblock->rblock);
		if (tblock->rblock)
			printk(" (%d @%p #%d)", kobjsize(tblock->rblock->kblock), tblock->rblock->kblock, tblock->rblock->refcount);
		printk(tblock->next ? " ->" : ".\n");
	}
}
#endif /* DEBUG */

unsigned long do_mmap_pgoff(struct file *file,
			    unsigned long addr,
			    unsigned long len,
			    unsigned long prot,
			    unsigned long flags,
			    unsigned long pgoff)
{
	struct mm_tblock_struct *tblock = NULL;
	struct vm_area_struct *vma = NULL, *pvma;
	struct list_head *p;
	unsigned int vm_flags;
	void *result;
	int ret, chrdev;

	/*
	 * Get the !CONFIG_MMU specific checks done first
	 */
	chrdev = 0;
	if (file)
		chrdev = S_ISCHR(file->f_dentry->d_inode->i_mode);

	if ((flags & MAP_SHARED) && (prot & PROT_WRITE) && file && !chrdev) {
		printk("MAP_SHARED not completely supported (cannot detect page dirtying)\n");
		return -EINVAL;
	}

	if (flags & MAP_FIXED || addr) {
		/* printk("can't do fixed-address/overlay mmap of RAM\n"); */
		return -EINVAL;
	}

	/*
	 * now all the standard checks
	 */
	if (file && (!file->f_op || !file->f_op->mmap))
		return -ENODEV;

	if (PAGE_ALIGN(len) == 0)
		return addr;

	if (len > TASK_SIZE)
		return -EINVAL;

	/* offset overflow? */
	if ((pgoff + (len >> PAGE_SHIFT)) < pgoff)
		return -EINVAL;

	/* we're going to need to record the mapping if it works */
	tblock = kmalloc(sizeof(struct mm_tblock_struct), GFP_KERNEL);
	if (!tblock)
		goto error_getting_tblock;
	memset(tblock, 0, sizeof(*tblock));

	/* Do simple checking here so the lower-level routines won't have
	 * to. we assume access permissions have been handled by the open
	 * of the memory object, so we don't do any here.
	 */
	vm_flags = calc_vm_flags(prot,flags) /* | mm->def_flags */
		| VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC;

	if (!chrdev) {
		/* share any file segment that's mapped read-only */
		if (((flags & MAP_PRIVATE) && !(prot & PROT_WRITE) && file) ||
		    ((flags & MAP_SHARED) && !(prot & PROT_WRITE) && file))
			vm_flags |= VM_SHARED | VM_MAYSHARE;

		/* refuse to let anyone share files with this process if it's being traced -
		 * otherwise breakpoints set in it may interfere with another untraced process
		 */
		if (!chrdev && current->ptrace & PT_PTRACED)
			vm_flags &= ~(VM_SHARED | VM_MAYSHARE);
	}
	else {
		/* permit sharing of character devices at any time */
		vm_flags |= VM_MAYSHARE;
		if (flags & MAP_SHARED)
			vm_flags |= VM_SHARED;
	}

	/* if we want to share, we need to search for VMAs created by another mmap() call that
	 * overlap with our proposed mapping
	 * - we can only share with an exact match on regular files
	 * - shared mappings on character devices are permitted to overlap inexactly as far as we
	 *   are concerned, but in that case, sharing is handled in the driver rather than here
	 */
	down_write(&nommu_vma_sem);
	if (!chrdev && vm_flags & VM_SHARED) {
		unsigned long pglen = (len + PAGE_SIZE - 1) >> PAGE_SHIFT;
		unsigned long vmpglen;

		list_for_each_entry(vma, &nommu_vma_list, vm_link) {
			if (!(vma->vm_flags & VM_SHARED))
				continue;

			if (vma->vm_file->f_dentry->d_inode != file->f_dentry->d_inode)
				continue;

			if (vma->vm_pgoff >= pgoff + pglen)
				continue;

			vmpglen = (vma->vm_end - vma->vm_start + PAGE_SIZE - 1) >> PAGE_SHIFT;
			if (pgoff >= vma->vm_pgoff + vmpglen)
				continue;

			if (vmpglen != pglen || vma->vm_pgoff != pgoff) {
				if (flags & MAP_SHARED)
					goto sharing_violation;
				continue;
			}

			/* we've found a VMA we can share */
			atomic_inc(&vma->vm_usage);

			tblock->vma = vma;
			result = (void *) vma->vm_start;
			goto shared;
		}
	}

	/* obtain the address to map to. we verify (or select) it and ensure
	 * that it represents a valid section of the address space
	 * - this is the hook for quasi-memory character devices
	 */
	if (file && file->f_op && file->f_op->get_unmapped_area)
		addr = file->f_op->get_unmapped_area(file, addr, len, pgoff, flags);

	if (IS_ERR((void *) addr)) {
		ret = addr;
		goto error;
	}

	/* we're going to need a VMA struct as well */
	vma = kmalloc(sizeof(struct vm_area_struct), GFP_KERNEL);
	if (!vma)
		goto error_getting_vma;

	INIT_LIST_HEAD(&vma->vm_link);
	atomic_set(&vma->vm_usage, 1);
	if (file)
		get_file(file);
	vma->vm_file	= file;
	vma->vm_flags	= vm_flags;
	vma->vm_start	= addr;
	vma->vm_end	= addr + len;
	vma->vm_pgoff	= pgoff;

	tblock->vma = vma;

	/*
	 * determine the object being mapped and call the appropriate
	 * specific mapper.
	 */
	if (file) {
		ret = -ENODEV;
		if (!file->f_op)
			goto error;

#ifdef MAGIC_ROM_PTR
		/* First, try simpler routine designed to give us a ROM pointer. */

		if (file->f_op->romptr && !(prot & PROT_WRITE)) {
			ret = file->f_op->romptr(file, vma);
#ifdef DEBUG
			printk("romptr mmap returned %d (st=%lx)\n",
			       ret, vma->vm_start);
#endif
			result = (void *) vma->vm_start;
			if (!ret)
				goto done;
			else if (ret != -ENOSYS)
				goto error;
		} else
#endif /* MAGIC_ROM_PTR */
		/* Then try full mmap routine, which might return a RAM pointer,
		   or do something truly complicated. */

		if (file->f_op->mmap) {
			ret = file->f_op->mmap(file, vma);

#ifdef DEBUG
			printk("f_op->mmap() returned %d (st=%lx)\n",
			       ret, vma->vm_start);
#endif
			result = (void *) vma->vm_start;
			if (!ret)
				goto done;
			else if (ret != -ENOSYS)
				goto error;
		} else {
			ret = -ENODEV; /* No mapping operations defined */
			goto error;
		}

		/* An ENOSYS error indicates that mmap isn't possible (as opposed to
		   tried but failed) so we'll fall through to the copy. */
	}

	/* allocate some memory to hold the mapping */
	ret = -ENOMEM;
	result = kmalloc(len, GFP_KERNEL);
	if (!result) {
		printk("Allocation of length %lu from process %d failed\n",
		       len, current->pid);
		show_free_areas();
		goto error;
	}

	vma->vm_start = (unsigned long) result;
	vma->vm_end = vma->vm_start + len;

#ifdef WARN_ON_SLACK
	if (len + WARN_ON_SLACK <= kobjsize(result))
		printk("Allocation of %lu bytes from process %d has %lu bytes of slack\n",
		       len, current->pid, kobjsize(result) - len);
#endif

	if (file) {
		mm_segment_t old_fs = get_fs();
		loff_t fpos;

		fpos = pgoff;
		fpos <<= PAGE_SHIFT;

		set_fs(KERNEL_DS);
		ret = file->f_op->read(file, (char *) result, len, &fpos);
		set_fs(old_fs);

		if (ret < 0)
			goto error2;
		if (ret < len)
			memset(result + ret, 0, len - ret);
	} else {
		memset(result, 0, len);
	}

	if (prot & PROT_EXEC)
		flush_icache_range((unsigned long) result, (unsigned long) result + len);

 done:
	realalloc += kobjsize(result);
	askedalloc += len;

	realalloc += kobjsize(vma);
	askedalloc += sizeof(*vma);

	current->mm->total_vm += len >> PAGE_SHIFT;

	list_for_each(p, &nommu_vma_list) {
		pvma = list_entry(p, struct vm_area_struct, vm_link);
		if (pvma->vm_start > vma->vm_start)
			break;
	}
	list_add_tail(&vma->vm_link, p);

 shared:
	realalloc += kobjsize(tblock);
	askedalloc += sizeof(*tblock);

	tblock->next = current->mm->context.tblock;
	current->mm->context.tblock = tblock;

	up_write(&nommu_vma_sem);

#ifdef DEBUG
	printk("do_mmap:\n");
	show_process_blocks();
#endif

	return (unsigned long) result;

 error2:
	kfree(result);
 error:
	up_write(&nommu_vma_sem);
	kfree(tblock);
	if (vma) {
		fput(vma->vm_file);
		kfree(vma);
	}
	return ret;

 sharing_violation:
	up_write(&nommu_vma_sem);
	printk("Attempt to share mismatched mappings\n");
	kfree(tblock);
	return -EINVAL;

 error_getting_vma:
	up_write(&nommu_vma_sem);
	kfree(tblock);
	printk("Allocation of tblock for %lu byte allocation from process %d failed\n",
	       len, current->pid);
	show_free_areas();
	return -ENOMEM;

 error_getting_tblock:
	printk("Allocation of tblock for %lu byte allocation from process %d failed\n",
	       len, current->pid);
	show_free_areas();
	return -ENOMEM;
}

static void put_vma(struct vm_area_struct *vma)
{
	if (vma) {
		down_write(&nommu_vma_sem);
		if (atomic_dec_and_test(&vma->vm_usage)) {
			list_del_init(&vma->vm_link);

			if (!(vma->vm_flags & VM_IO) && vma->vm_start) {
				realalloc -= kobjsize((void *) vma->vm_start);
				askedalloc -= vma->vm_end - vma->vm_start;
				if (vma->vm_file)
					fput(vma->vm_file);
				kfree((void *) vma->vm_start);
			}

			realalloc -= kobjsize(vma);
			askedalloc -= sizeof(*vma);
			kfree(vma);
		}
		up_write(&nommu_vma_sem);
	}
}

int do_munmap(struct mm_struct *mm, unsigned long addr, size_t len)
{
	struct mm_tblock_struct *tblock, **parent;

#ifdef MAGIC_ROM_PTR
	/* For efficiency's sake, if the pointer is obviously in ROM,
	   don't bother walking the lists to free it */
	if (is_in_rom(addr))
		return 0;
#endif

#ifdef DEBUG
	printk("do_munmap:\n");
#endif

	for (parent = &mm->context.tblock; *parent; parent = &(*parent)->next)
		if ((*parent)->vma->vm_start == addr)
			break;
	tblock = *parent;

	if (!tblock) {
		printk("munmap of non-mmaped memory by process %d (%s): %p\n",
		       current->pid, current->comm, (void *) addr);
		return -EINVAL;
	}

	put_vma(tblock->vma);

	*parent = tblock->next;
	realalloc -= kobjsize(tblock);
	askedalloc -= sizeof(*tblock);
	kfree(tblock);
	mm->total_vm -= len >> PAGE_SHIFT;

#ifdef DEBUG
	show_process_blocks();
#endif

	return 0;
}

/* Release all mmaps. */
void exit_mmap(struct mm_struct * mm)
{
	struct mm_tblock_struct *tmp;

	if (mm) {
#ifdef DEBUG
		printk("Exit_mmap:\n");
#endif

		mm->total_vm = 0;

		while ((tmp = mm->context.tblock)) {
			mm->context.tblock = tmp->next;
			put_vma(tmp->vma);

			realalloc -= kobjsize(tmp);
			askedalloc -= sizeof(*tmp);
			kfree(tmp);
		}

#ifdef DEBUG
		show_process_blocks();
#endif
	}
}

asmlinkage long sys_munmap(unsigned long addr, size_t len)
{
	int ret;
	struct mm_struct *mm = current->mm;

	down_write(&mm->mmap_sem);
	ret = do_munmap(mm, addr, len);
	up_write(&mm->mmap_sem);
	return ret;
}

unsigned long do_brk(unsigned long addr, unsigned long len)
{
	return -ENOMEM;
}

/*
 * Expand (or shrink) an existing mapping, potentially moving it at the
 * same time (controlled by the MREMAP_MAYMOVE flag and available VM space)
 *
 * MREMAP_FIXED option added 5-Dec-1999 by Benjamin LaHaise
 * This option implies MREMAP_MAYMOVE.
 *
 * on uClinux, we only permit changing a mapping's size, and only as long as it stays within the
 * hole allocated by the kmalloc() call in do_mmap_pgoff() and the block is not shareable
 */
unsigned long do_mremap(unsigned long addr,
			unsigned long old_len, unsigned long new_len,
			unsigned long flags, unsigned long new_addr)
{
	struct mm_tblock_struct *tblock = NULL;

	/* insanity checks first */
	if (new_len == 0)
		return (unsigned long) -EINVAL;

	if (flags & MREMAP_FIXED && new_addr != addr)
		return (unsigned long) -EINVAL;

	for (tblock = current->mm->context.tblock; tblock; tblock = tblock->next)
		if (tblock->vma->vm_start == addr)
			goto found;

	return (unsigned long) -EINVAL;

 found:
	if (tblock->vma->vm_end != tblock->vma->vm_start + old_len)
		return (unsigned long) -EFAULT;

	if (tblock->vma->vm_flags & VM_MAYSHARE)
		return (unsigned long) -EPERM;

	if (new_len > kobjsize((void *) addr))
		return (unsigned long) -ENOMEM;

	/* all checks complete - do it */
	tblock->vma->vm_end = tblock->vma->vm_start + new_len;

	askedalloc -= old_len;
	askedalloc += new_len;

	return tblock->vma->vm_start;
}

struct vm_area_struct * find_vma(struct mm_struct * mm, unsigned long addr)
{
	return NULL;
}

struct page * follow_page(struct mm_struct *mm, unsigned long addr, int write)
{
	return NULL;
}

struct vm_area_struct *find_extend_vma(struct mm_struct *mm, unsigned long addr)
{
	return NULL;
}

int remap_pfn_range(struct vm_area_struct *vma, unsigned long from,
		unsigned long to, unsigned long size, pgprot_t prot)
{
	return -EPERM;
}

void swap_unplug_io_fn(struct backing_dev_info *bdi, struct page *page)
{
}

unsigned long arch_get_unmapped_area(struct file *file, unsigned long addr,
	unsigned long len, unsigned long pgoff, unsigned long flags)
{
	return -ENOMEM;
}

void arch_unmap_area(struct vm_area_struct *area)
{
}

