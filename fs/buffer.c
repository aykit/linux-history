/*
 *  linux/fs/buffer.c
 *
 *  Copyright (C) 1991, 1992, 2002  Linus Torvalds
 */

/*
 * Start bdflush() with kernel_thread not syscall - Paul Gortmaker, 12/95
 *
 * Removed a lot of unnecessary code and simplified things now that
 * the buffer cache isn't our primary cache - Andrew Tridgell 12/96
 *
 * Speed up hash, lru, and free list operations.  Use gfp() for allocating
 * hash table, use SLAB cache for buffer heads. SMP threading.  -DaveM
 *
 * Added 32k buffer block sizes - these are required older ARM systems. - RMK
 *
 * async buffer flushing, 1999 Andrea Arcangeli <andrea@suse.de>
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/smp_lock.h>
#include <linux/blkdev.h>
#include <linux/file.h>
#include <linux/quotaops.h>
#include <linux/iobuf.h>
#include <linux/module.h>
#include <linux/writeback.h>
#include <linux/mempool.h>
#include <linux/hash.h>
#include <linux/suspend.h>
#include <linux/buffer_head.h>
#include <linux/bio.h>
#include <asm/bitops.h>

static void invalidate_bh_lrus(void);

#define BH_ENTRY(list) list_entry((list), struct buffer_head, b_assoc_buffers)

/*
 * Hashed waitqueue_head's for wait_on_buffer()
 */
#define BH_WAIT_TABLE_ORDER	7
static struct bh_wait_queue_head {
	wait_queue_head_t wqh;
} ____cacheline_aligned_in_smp bh_wait_queue_heads[1<<BH_WAIT_TABLE_ORDER];

/*
 * Debug/devel support stuff
 */

void __buffer_error(char *file, int line)
{
	static int enough;

	if (enough > 10)
		return;
	enough++;
	printk("buffer layer error at %s:%d\n", file, line);
	printk("Pass this trace through ksymoops for reporting\n");
	dump_stack();
}
EXPORT_SYMBOL(__buffer_error);

inline void
init_buffer(struct buffer_head *bh, bh_end_io_t *handler, void *private)
{
	bh->b_end_io = handler;
	bh->b_private = private;
}

/*
 * Return the address of the waitqueue_head to be used for this
 * buffer_head
 */
static wait_queue_head_t *bh_waitq_head(struct buffer_head *bh)
{
	return &bh_wait_queue_heads[hash_ptr(bh, BH_WAIT_TABLE_ORDER)].wqh;
}

/*
 * Wait on a buffer until someone does a wakeup on it.  Needs
 * lots of external locking.  ext3 uses this.  Fix it.
 */
void sleep_on_buffer(struct buffer_head *bh)
{
	wait_queue_head_t *wq = bh_waitq_head(bh);
	sleep_on(wq);
}
EXPORT_SYMBOL(sleep_on_buffer);

void wake_up_buffer(struct buffer_head *bh)
{
	wait_queue_head_t *wq = bh_waitq_head(bh);

	if (waitqueue_active(wq))
		wake_up_all(wq);
}
EXPORT_SYMBOL(wake_up_buffer);

void unlock_buffer(struct buffer_head *bh)
{
	/*
	 * unlock_buffer against a zero-count bh is a bug, if the page
	 * is not locked.  Because then nothing protects the buffer's
	 * waitqueue, which is used here. (Well.  Other locked buffers
	 * against the page will pin it.  But complain anyway).
	 */
	if (atomic_read(&bh->b_count) == 0 &&
			!PageLocked(bh->b_page) &&
			!PageWriteback(bh->b_page))
		buffer_error();

	clear_buffer_locked(bh);
	smp_mb__after_clear_bit();
	wake_up_buffer(bh);
}

/*
 * Block until a buffer comes unlocked.  This doesn't stop it
 * from becoming locked again - you have to lock it yourself
 * if you want to preserve its state.
 */
void __wait_on_buffer(struct buffer_head * bh)
{
	wait_queue_head_t *wqh = bh_waitq_head(bh);
	DEFINE_WAIT(wait);

	get_bh(bh);
	do {
		prepare_to_wait(wqh, &wait, TASK_UNINTERRUPTIBLE);
		blk_run_queues();
		if (buffer_locked(bh))
			schedule();
	} while (buffer_locked(bh));
	put_bh(bh);
	finish_wait(wqh, &wait);
}

static inline void
__set_page_buffers(struct page *page, struct buffer_head *head)
{
	if (page_has_buffers(page))
		buffer_error();
	page_cache_get(page);
	SetPagePrivate(page);
	page->private = (unsigned long)head;
}

static inline void
__clear_page_buffers(struct page *page)
{
	ClearPagePrivate(page);
	page->private = 0;
	page_cache_release(page);
}

static void buffer_io_error(struct buffer_head *bh)
{
	printk(KERN_ERR "Buffer I/O error on device %s, logical block %Lu\n",
			bdevname(bh->b_bdev),
			(unsigned long long)bh->b_blocknr);
}

/*
 * Default synchronous end-of-IO handler..  Just mark it up-to-date and
 * unlock the buffer. This is what ll_rw_block uses too.
 */
void end_buffer_io_sync(struct buffer_head *bh, int uptodate)
{
	if (uptodate) {
		set_buffer_uptodate(bh);
	} else {
		/*
		 * This happens, due to failed READA attempts.
		 * buffer_io_error(bh);
		 */
		clear_buffer_uptodate(bh);
	}
	unlock_buffer(bh);
	put_bh(bh);
}

/*
 * Write out and wait upon all the dirty data associated with a block
 * device via its mapping.  Does not take the superblock lock.
 */
int sync_blockdev(struct block_device *bdev)
{
	int ret = 0;

	if (bdev) {
		int err;

		ret = filemap_fdatawrite(bdev->bd_inode->i_mapping);
		err = filemap_fdatawait(bdev->bd_inode->i_mapping);
		if (!ret)
			ret = err;
	}
	return ret;
}
EXPORT_SYMBOL(sync_blockdev);

/*
 * Write out and wait upon all dirty data associated with this
 * superblock.  Filesystem data as well as the underlying block
 * device.  Takes the superblock lock.
 */
int fsync_super(struct super_block *sb)
{
	sync_inodes_sb(sb, 0);
	DQUOT_SYNC(sb);
	lock_super(sb);
	if (sb->s_dirt && sb->s_op && sb->s_op->write_super)
		sb->s_op->write_super(sb);
	unlock_super(sb);
	sync_blockdev(sb->s_bdev);
	sync_inodes_sb(sb, 1);

	return sync_blockdev(sb->s_bdev);
}

/*
 * Write out and wait upon all dirty data associated with this
 * device.   Filesystem data as well as the underlying block
 * device.  Takes the superblock lock.
 */
int fsync_bdev(struct block_device *bdev)
{
	struct super_block *sb = get_super(bdev);
	if (sb) {
		int res = fsync_super(sb);
		drop_super(sb);
		return res;
	}
	return sync_blockdev(bdev);
}

/*
 * sync everything.  Start out by waking pdflush, because that writes back
 * all queues in parallel.
 */
asmlinkage long sys_sync(void)
{
	wakeup_bdflush(0);
	sync_inodes(0);	/* All mappings and inodes, including block devices */
	DQUOT_SYNC(NULL);
	sync_supers();	/* Write the superblocks */
	sync_inodes(1);	/* All the mappings and inodes, again. */
	return 0;
}

/*
 * Generic function to fsync a file.
 *
 * filp may be NULL if called via the msync of a vma.
 */
 
int file_fsync(struct file *filp, struct dentry *dentry, int datasync)
{
	struct inode * inode = dentry->d_inode;
	struct super_block * sb;
	int ret;

	/* sync the inode to buffers */
	write_inode_now(inode, 0);

	/* sync the superblock to buffers */
	sb = inode->i_sb;
	lock_super(sb);
	if (sb->s_op && sb->s_op->write_super)
		sb->s_op->write_super(sb);
	unlock_super(sb);

	/* .. finally sync the buffers to disk */
	ret = sync_blockdev(sb->s_bdev);
	return ret;
}

asmlinkage long sys_fsync(unsigned int fd)
{
	struct file * file;
	struct dentry * dentry;
	struct inode * inode;
	int ret, err;

	ret = -EBADF;
	file = fget(fd);
	if (!file)
		goto out;

	dentry = file->f_dentry;
	inode = dentry->d_inode;

	ret = -EINVAL;
	if (!file->f_op || !file->f_op->fsync) {
		/* Why?  We can still call filemap_fdatawrite */
		goto out_putf;
	}

	/* We need to protect against concurrent writers.. */
	down(&inode->i_sem);
	ret = filemap_fdatawrite(inode->i_mapping);
	err = file->f_op->fsync(file, dentry, 0);
	if (!ret)
		ret = err;
	err = filemap_fdatawait(inode->i_mapping);
	if (!ret)
		ret = err;
	up(&inode->i_sem);

out_putf:
	fput(file);
out:
	return ret;
}

asmlinkage long sys_fdatasync(unsigned int fd)
{
	struct file * file;
	struct dentry * dentry;
	struct inode * inode;
	int ret, err;

	ret = -EBADF;
	file = fget(fd);
	if (!file)
		goto out;

	dentry = file->f_dentry;
	inode = dentry->d_inode;

	ret = -EINVAL;
	if (!file->f_op || !file->f_op->fsync)
		goto out_putf;

	down(&inode->i_sem);
	ret = filemap_fdatawrite(inode->i_mapping);
	err = file->f_op->fsync(file, dentry, 1);
	if (!ret)
		ret = err;
	err = filemap_fdatawait(inode->i_mapping);
	if (!ret)
		ret = err;
	up(&inode->i_sem);

out_putf:
	fput(file);
out:
	return ret;
}

/*
 * Various filesystems appear to want __find_get_block to be non-blocking.
 * But it's the page lock which protects the buffers.  To get around this,
 * we get exclusion from try_to_free_buffers with the blockdev mapping's
 * private_lock.
 *
 * Hack idea: for the blockdev mapping, i_bufferlist_lock contention
 * may be quite high.  This code could TryLock the page, and if that
 * succeeds, there is no need to take private_lock. (But if
 * private_lock is contended then so is mapping->page_lock).
 */
struct buffer_head *
__find_get_block_slow(struct block_device *bdev, sector_t block, int unused)
{
	struct inode *bd_inode = bdev->bd_inode;
	struct address_space *bd_mapping = bd_inode->i_mapping;
	struct buffer_head *ret = NULL;
	unsigned long index;
	struct buffer_head *bh;
	struct buffer_head *head;
	struct page *page;

	index = block >> (PAGE_CACHE_SHIFT - bd_inode->i_blkbits);
	page = find_get_page(bd_mapping, index);
	if (!page)
		goto out;

	spin_lock(&bd_mapping->private_lock);
	if (!page_has_buffers(page))
		goto out_unlock;
	head = page_buffers(page);
	bh = head;
	do {
		if (bh->b_blocknr == block) {
			ret = bh;
			get_bh(bh);
			goto out_unlock;
		}
		bh = bh->b_this_page;
	} while (bh != head);
	buffer_error();
out_unlock:
	spin_unlock(&bd_mapping->private_lock);
	page_cache_release(page);
out:
	return ret;
}

/* If invalidate_buffers() will trash dirty buffers, it means some kind
   of fs corruption is going on. Trashing dirty data always imply losing
   information that was supposed to be just stored on the physical layer
   by the user.

   Thus invalidate_buffers in general usage is not allwowed to trash
   dirty buffers. For example ioctl(FLSBLKBUF) expects dirty data to
   be preserved.  These buffers are simply skipped.
  
   We also skip buffers which are still in use.  For example this can
   happen if a userspace program is reading the block device.

   NOTE: In the case where the user removed a removable-media-disk even if
   there's still dirty data not synced on disk (due a bug in the device driver
   or due an error of the user), by not destroying the dirty buffers we could
   generate corruption also on the next media inserted, thus a parameter is
   necessary to handle this case in the most safe way possible (trying
   to not corrupt also the new disk inserted with the data belonging to
   the old now corrupted disk). Also for the ramdisk the natural thing
   to do in order to release the ramdisk memory is to destroy dirty buffers.

   These are two special cases. Normal usage imply the device driver
   to issue a sync on the device (without waiting I/O completion) and
   then an invalidate_buffers call that doesn't trash dirty buffers.

   For handling cache coherency with the blkdev pagecache the 'update' case
   is been introduced. It is needed to re-read from disk any pinned
   buffer. NOTE: re-reading from disk is destructive so we can do it only
   when we assume nobody is changing the buffercache under our I/O and when
   we think the disk contains more recent information than the buffercache.
   The update == 1 pass marks the buffers we need to update, the update == 2
   pass does the actual I/O. */
void invalidate_bdev(struct block_device *bdev, int destroy_dirty_buffers)
{
	invalidate_bh_lrus();
	/*
	 * FIXME: what about destroy_dirty_buffers?
	 * We really want to use invalidate_inode_pages2() for
	 * that, but not until that's cleaned up.
	 */
	invalidate_inode_pages(bdev->bd_inode->i_mapping);
}

void __invalidate_buffers(kdev_t dev, int destroy_dirty_buffers)
{
	struct block_device *bdev = bdget(kdev_t_to_nr(dev));
	if (bdev) {
		invalidate_bdev(bdev, destroy_dirty_buffers);
		bdput(bdev);
	}
}

/*
 * Kick pdflush then try to free up some ZONE_NORMAL memory.
 */
static void free_more_memory(void)
{
	struct zone *zone;

	zone = contig_page_data.node_zonelists[GFP_NOFS&GFP_ZONEMASK].zones[0];
	wakeup_bdflush(1024);
	blk_run_queues();
	yield();
	try_to_free_pages(zone, GFP_NOFS, 0);
}

/*
 * I/O completion handler for block_read_full_page() - pages
 * which come unlocked at the end of I/O.
 */
static void end_buffer_async_read(struct buffer_head *bh, int uptodate)
{
	static spinlock_t page_uptodate_lock = SPIN_LOCK_UNLOCKED;
	unsigned long flags;
	struct buffer_head *tmp;
	struct page *page;
	int page_uptodate = 1;

	BUG_ON(!buffer_async_read(bh));

	page = bh->b_page;
	if (uptodate) {
		set_buffer_uptodate(bh);
	} else {
		clear_buffer_uptodate(bh);
		buffer_io_error(bh);
		SetPageError(page);
	}

	/*
	 * Be _very_ careful from here on. Bad things can happen if
	 * two buffer heads end IO at almost the same time and both
	 * decide that the page is now completely done.
	 */
	spin_lock_irqsave(&page_uptodate_lock, flags);
	clear_buffer_async_read(bh);
	unlock_buffer(bh);
	tmp = bh;
	do {
		if (!buffer_uptodate(tmp))
			page_uptodate = 0;
		if (buffer_async_read(tmp)) {
			BUG_ON(!buffer_locked(tmp));
			goto still_busy;
		}
		tmp = tmp->b_this_page;
	} while (tmp != bh);
	spin_unlock_irqrestore(&page_uptodate_lock, flags);

	/*
	 * If none of the buffers had errors and they are all
	 * uptodate then we can set the page uptodate.
	 */
	if (page_uptodate && !PageError(page))
		SetPageUptodate(page);
	unlock_page(page);
	return;

still_busy:
	spin_unlock_irqrestore(&page_uptodate_lock, flags);
	return;
}

/*
 * Completion handler for block_write_full_page() - pages which are unlocked
 * during I/O, and which have PageWriteback cleared upon I/O completion.
 */
static void end_buffer_async_write(struct buffer_head *bh, int uptodate)
{
	static spinlock_t page_uptodate_lock = SPIN_LOCK_UNLOCKED;
	unsigned long flags;
	struct buffer_head *tmp;
	struct page *page;

	BUG_ON(!buffer_async_write(bh));

	page = bh->b_page;
	if (uptodate) {
		set_buffer_uptodate(bh);
	} else {
		buffer_io_error(bh);
		clear_buffer_uptodate(bh);
		SetPageError(page);
	}

	spin_lock_irqsave(&page_uptodate_lock, flags);
	clear_buffer_async_write(bh);
	unlock_buffer(bh);
	tmp = bh->b_this_page;
	while (tmp != bh) {
		if (buffer_async_write(tmp)) {
			BUG_ON(!buffer_locked(tmp));
			goto still_busy;
		}
		tmp = tmp->b_this_page;
	}
	spin_unlock_irqrestore(&page_uptodate_lock, flags);
	end_page_writeback(page);
	return;

still_busy:
	spin_unlock_irqrestore(&page_uptodate_lock, flags);
	return;
}

/*
 * If a page's buffers are under async readin (end_buffer_async_read
 * completion) then there is a possibility that another thread of
 * control could lock one of the buffers after it has completed
 * but while some of the other buffers have not completed.  This
 * locked buffer would confuse end_buffer_async_read() into not unlocking
 * the page.  So the absence of BH_Async_Read tells end_buffer_async_read()
 * that this buffer is not under async I/O.
 *
 * The page comes unlocked when it has no locked buffer_async buffers
 * left.
 *
 * PageLocked prevents anyone starting new async I/O reads any of
 * the buffers.
 *
 * PageWriteback is used to prevent simultaneous writeout of the same
 * page.
 *
 * PageLocked prevents anyone from starting writeback of a page which is
 * under read I/O (PageWriteback is only ever set against a locked page).
 */
inline void mark_buffer_async_read(struct buffer_head *bh)
{
	bh->b_end_io = end_buffer_async_read;
	set_buffer_async_read(bh);
}
EXPORT_SYMBOL(mark_buffer_async_read);

inline void mark_buffer_async_write(struct buffer_head *bh)
{
	bh->b_end_io = end_buffer_async_write;
	set_buffer_async_write(bh);
}
EXPORT_SYMBOL(mark_buffer_async_write);


/*
 * fs/buffer.c contains helper functions for buffer-backed address space's
 * fsync functions.  A common requirement for buffer-based filesystems is
 * that certain data from the backing blockdev needs to be written out for
 * a successful fsync().  For example, ext2 indirect blocks need to be
 * written back and waited upon before fsync() returns.
 *
 * The functions mark_buffer_inode_dirty(), fsync_inode_buffers(),
 * inode_has_buffers() and invalidate_inode_buffers() are provided for the
 * management of a list of dependent buffers at ->i_mapping->private_list.
 *
 * Locking is a little subtle: try_to_free_buffers() will remove buffers
 * from their controlling inode's queue when they are being freed.  But
 * try_to_free_buffers() will be operating against the *blockdev* mapping
 * at the time, not against the S_ISREG file which depends on those buffers.
 * So the locking for private_list is via the private_lock in the address_space
 * which backs the buffers.  Which is different from the address_space 
 * against which the buffers are listed.  So for a particular address_space,
 * mapping->private_lock does *not* protect mapping->private_list!  In fact,
 * mapping->private_list will always be protected by the backing blockdev's
 * ->private_lock.
 *
 * Which introduces a requirement: all buffers on an address_space's
 * ->private_list must be from the same address_space: the blockdev's.
 *
 * address_spaces which do not place buffers at ->private_list via these
 * utility functions are free to use private_lock and private_list for
 * whatever they want.  The only requirement is that list_empty(private_list)
 * be true at clear_inode() time.
 *
 * FIXME: clear_inode should not call invalidate_inode_buffers().  The
 * filesystems should do that.  invalidate_inode_buffers() should just go
 * BUG_ON(!list_empty).
 *
 * FIXME: mark_buffer_dirty_inode() is a data-plane operation.  It should
 * take an address_space, not an inode.  And it should be called
 * mark_buffer_dirty_fsync() to clearly define why those buffers are being
 * queued up.
 *
 * FIXME: mark_buffer_dirty_inode() doesn't need to add the buffer to the
 * list if it is already on a list.  Because if the buffer is on a list,
 * it *must* already be on the right one.  If not, the filesystem is being
 * silly.  This will save a ton of locking.  But first we have to ensure
 * that buffers are taken *off* the old inode's list when they are freed
 * (presumably in truncate).  That requires careful auditing of all
 * filesystems (do it inside bforget()).  It could also be done by bringing
 * b_inode back.
 */

void buffer_insert_list(spinlock_t *lock,
		struct buffer_head *bh, struct list_head *list)
{
	spin_lock(lock);
	list_del(&bh->b_assoc_buffers);
	list_add(&bh->b_assoc_buffers, list);
	spin_unlock(lock);
}

/*
 * The buffer's backing address_space's private_lock must be held
 */
static inline void __remove_assoc_queue(struct buffer_head *bh)
{
	list_del_init(&bh->b_assoc_buffers);
}

int inode_has_buffers(struct inode *inode)
{
	return !list_empty(&inode->i_mapping->private_list);
}

/*
 * osync is designed to support O_SYNC io.  It waits synchronously for
 * all already-submitted IO to complete, but does not queue any new
 * writes to the disk.
 *
 * To do O_SYNC writes, just queue the buffer writes with ll_rw_block as
 * you dirty the buffers, and then use osync_inode_buffers to wait for
 * completion.  Any other dirty buffers which are not yet queued for
 * write will not be flushed to disk by the osync.
 */
static int osync_buffers_list(spinlock_t *lock, struct list_head *list)
{
	struct buffer_head *bh;
	struct list_head *p;
	int err = 0;

	spin_lock(lock);
repeat:
	list_for_each_prev(p, list) {
		bh = BH_ENTRY(p);
		if (buffer_locked(bh)) {
			get_bh(bh);
			spin_unlock(lock);
			wait_on_buffer(bh);
			if (!buffer_uptodate(bh))
				err = -EIO;
			brelse(bh);
			spin_lock(lock);
			goto repeat;
		}
	}
	spin_unlock(lock);
	return err;
}

/**
 * sync_mapping_buffers - write out and wait upon a mapping's "associated"
 *                        buffers
 * @buffer_mapping - the mapping which backs the buffers' data
 * @mapping - the mapping which wants those buffers written
 *
 * Starts I/O against the buffers at mapping->private_list, and waits upon
 * that I/O.
 *
 * Basically, this is a convenience function for fsync().  @buffer_mapping is
 * the blockdev which "owns" the buffers and @mapping is a file or directory
 * which needs those buffers to be written for a successful fsync().
 */
int sync_mapping_buffers(struct address_space *mapping)
{
	struct address_space *buffer_mapping = mapping->assoc_mapping;

	if (buffer_mapping == NULL || list_empty(&mapping->private_list))
		return 0;

	return fsync_buffers_list(&buffer_mapping->private_lock,
					&mapping->private_list);
}
EXPORT_SYMBOL(sync_mapping_buffers);

/*
 * Called when we've recently written block `bblock', and it is known that
 * `bblock' was for a buffer_boundary() buffer.  This means that the block at
 * `bblock + 1' is probably a dirty indirect block.  Hunt it down and, if it's
 * dirty, schedule it for IO.  So that indirects merge nicely with their data.
 */
void write_boundary_block(struct block_device *bdev,
			sector_t bblock, unsigned blocksize)
{
	struct buffer_head *bh = __find_get_block(bdev, bblock + 1, blocksize);
	if (bh) {
		if (buffer_dirty(bh))
			ll_rw_block(WRITE, 1, &bh);
		put_bh(bh);
	}
}

void mark_buffer_dirty_inode(struct buffer_head *bh, struct inode *inode)
{
	struct address_space *mapping = inode->i_mapping;
	struct address_space *buffer_mapping = bh->b_page->mapping;

	mark_buffer_dirty(bh);
	if (!mapping->assoc_mapping) {
		mapping->assoc_mapping = buffer_mapping;
	} else {
		if (mapping->assoc_mapping != buffer_mapping)
			BUG();
	}
	if (list_empty(&bh->b_assoc_buffers))
		buffer_insert_list(&buffer_mapping->private_lock,
				bh, &mapping->private_list);
}
EXPORT_SYMBOL(mark_buffer_dirty_inode);

/*
 * Write out and wait upon a list of buffers.
 *
 * We have conflicting pressures: we want to make sure that all
 * initially dirty buffers get waited on, but that any subsequently
 * dirtied buffers don't.  After all, we don't want fsync to last
 * forever if somebody is actively writing to the file.
 *
 * Do this in two main stages: first we copy dirty buffers to a
 * temporary inode list, queueing the writes as we go.  Then we clean
 * up, waiting for those writes to complete.
 * 
 * During this second stage, any subsequent updates to the file may end
 * up refiling the buffer on the original inode's dirty list again, so
 * there is a chance we will end up with a buffer queued for write but
 * not yet completed on that list.  So, as a final cleanup we go through
 * the osync code to catch these locked, dirty buffers without requeuing
 * any newly dirty buffers for write.
 */
int fsync_buffers_list(spinlock_t *lock, struct list_head *list)
{
	struct buffer_head *bh;
	struct list_head tmp;
	int err = 0, err2;

	INIT_LIST_HEAD(&tmp);

	spin_lock(lock);
	while (!list_empty(list)) {
		bh = BH_ENTRY(list->next);
		list_del_init(&bh->b_assoc_buffers);
		if (buffer_dirty(bh) || buffer_locked(bh)) {
			list_add(&bh->b_assoc_buffers, &tmp);
			if (buffer_dirty(bh)) {
				get_bh(bh);
				spin_unlock(lock);
				ll_rw_block(WRITE, 1, &bh);
				brelse(bh);
				spin_lock(lock);
			}
		}
	}

	while (!list_empty(&tmp)) {
		bh = BH_ENTRY(tmp.prev);
		__remove_assoc_queue(bh);
		get_bh(bh);
		spin_unlock(lock);
		wait_on_buffer(bh);
		if (!buffer_uptodate(bh))
			err = -EIO;
		brelse(bh);
		spin_lock(lock);
	}
	
	spin_unlock(lock);
	err2 = osync_buffers_list(lock, list);
	if (err)
		return err;
	else
		return err2;
}

/*
 * Invalidate any and all dirty buffers on a given inode.  We are
 * probably unmounting the fs, but that doesn't mean we have already
 * done a sync().  Just drop the buffers from the inode list.
 *
 * NOTE: we take the inode's blockdev's mapping's private_lock.  Which
 * assumes that all the buffers are against the blockdev.  Not true
 * for reiserfs.
 */
void invalidate_inode_buffers(struct inode *inode)
{
	if (inode_has_buffers(inode)) {
		struct address_space *mapping = inode->i_mapping;
		struct list_head *list = &mapping->private_list;
		struct address_space *buffer_mapping = mapping->assoc_mapping;

		spin_lock(&buffer_mapping->private_lock);
		while (!list_empty(list))
			__remove_assoc_queue(BH_ENTRY(list->next));
		spin_unlock(&buffer_mapping->private_lock);
	}
}

/*
 * Create the appropriate buffers when given a page for data area and
 * the size of each buffer.. Use the bh->b_this_page linked list to
 * follow the buffers created.  Return NULL if unable to create more
 * buffers.
 *
 * The retry flag is used to differentiate async IO (paging, swapping)
 * which may not fail from ordinary buffer allocations.
 */
static struct buffer_head *
create_buffers(struct page * page, unsigned long size, int retry)
{
	struct buffer_head *bh, *head;
	long offset;

try_again:
	head = NULL;
	offset = PAGE_SIZE;
	while ((offset -= size) >= 0) {
		int pf_flags = current->flags;

		current->flags |= PF_NOWARN;
		bh = alloc_buffer_head();
		current->flags = pf_flags;
		if (!bh)
			goto no_grow;

		bh->b_bdev = NULL;
		bh->b_this_page = head;
		bh->b_blocknr = -1;
		head = bh;

		bh->b_state = 0;
		atomic_set(&bh->b_count, 0);
		bh->b_size = size;

		/* Link the buffer to its page */
		set_bh_page(bh, page, offset);

		bh->b_end_io = NULL;
	}
	return head;
/*
 * In case anything failed, we just free everything we got.
 */
no_grow:
	if (head) {
		do {
			bh = head;
			head = head->b_this_page;
			free_buffer_head(bh);
		} while (head);
	}

	/*
	 * Return failure for non-async IO requests.  Async IO requests
	 * are not allowed to fail, so we have to wait until buffer heads
	 * become available.  But we don't want tasks sleeping with 
	 * partially complete buffers, so all were released above.
	 */
	if (!retry)
		return NULL;

	/* We're _really_ low on memory. Now we just
	 * wait for old buffer heads to become free due to
	 * finishing IO.  Since this is an async request and
	 * the reserve list is empty, we're sure there are 
	 * async buffer heads in use.
	 */
	blk_run_queues();

	free_more_memory();
	goto try_again;
}

static inline void
link_dev_buffers(struct page *page, struct buffer_head *head)
{
	struct buffer_head *bh, *tail;

	bh = head;
	do {
		tail = bh;
		bh = bh->b_this_page;
	} while (bh);
	tail->b_this_page = head;
	__set_page_buffers(page, head);
}

/*
 * Initialise the state of a blockdev page's buffers.
 */ 
static /*inline*/ void
init_page_buffers(struct page *page, struct block_device *bdev,
			int block, int size)
{
	struct buffer_head *head = page_buffers(page);
	struct buffer_head *bh = head;
	unsigned int b_state;

	b_state = 1 << BH_Mapped;
	if (PageUptodate(page))
		b_state |= 1 << BH_Uptodate;

	do {
		if (!(bh->b_state & (1 << BH_Mapped))) {
			init_buffer(bh, NULL, NULL);
			bh->b_bdev = bdev;
			bh->b_blocknr = block;
			bh->b_state = b_state;
		}
		block++;
		bh = bh->b_this_page;
	} while (bh != head);
}

/*
 * Create the page-cache page that contains the requested block.
 *
 * This is user purely for blockdev mappings.
 */
static /*inline*/ struct page *
grow_dev_page(struct block_device *bdev, unsigned long block,
			unsigned long index, int size)
{
	struct inode *inode = bdev->bd_inode;
	struct page *page;
	struct buffer_head *bh;

	page = find_or_create_page(inode->i_mapping, index, GFP_NOFS);
	if (!page)
		return NULL;

	if (!PageLocked(page))
		BUG();

	if (page_has_buffers(page)) {
		bh = page_buffers(page);
		if (bh->b_size == size)
			return page;
		if (!try_to_free_buffers(page))
			goto failed;
	}

	/*
	 * Allocate some buffers for this page
	 */
	bh = create_buffers(page, size, 0);
	if (!bh)
		goto failed;

	/*
	 * Link the page to the buffers and initialise them.  Take the
	 * lock to be atomic wrt __find_get_block(), which does not
	 * run under the page lock.
	 */
	spin_lock(&inode->i_mapping->private_lock);
	link_dev_buffers(page, bh);
	init_page_buffers(page, bdev, block, size);
	spin_unlock(&inode->i_mapping->private_lock);
	return page;

failed:
	buffer_error();
	unlock_page(page);
	page_cache_release(page);
	return NULL;
}

/*
 * Create buffers for the specified block device block's page.  If
 * that page was dirty, the buffers are set dirty also.
 *
 * Except that's a bug.  Attaching dirty buffers to a dirty
 * blockdev's page can result in filesystem corruption, because
 * some of those buffers may be aliases of filesystem data.
 * grow_dev_page() will go BUG() if this happens.
 */
static inline int
grow_buffers(struct block_device *bdev, unsigned long block, int size)
{
	struct page *page;
	unsigned long index;
	int sizebits;

	/* Size must be multiple of hard sectorsize */
	if (size & (bdev_hardsect_size(bdev)-1))
		BUG();
	if (size < 512 || size > PAGE_SIZE)
		BUG();

	sizebits = -1;
	do {
		sizebits++;
	} while ((size << sizebits) < PAGE_SIZE);

	index = block >> sizebits;
	block = index << sizebits;

	/* Create a page with the proper size buffers.. */
	page = grow_dev_page(bdev, block, index, size);
	if (!page)
		return 0;
	unlock_page(page);
	page_cache_release(page);
	return 1;
}

/*
 * __getblk will locate (and, if necessary, create) the buffer_head
 * which corresponds to the passed block_device, block and size. The
 * returned buffer has its reference count incremented.
 *
 * __getblk() cannot fail - it just keeps trying.  If you pass it an
 * illegal block number, __getblk() will happily return a buffer_head
 * which represents the non-existent block.  Very weird.
 *
 * __getblk() will lock up the machine if grow_dev_page's try_to_free_buffers()
 * attempt is failing.  FIXME, perhaps?
 */
struct buffer_head *
__getblk_slow(struct block_device *bdev, sector_t block, int size)
{
	for (;;) {
		struct buffer_head * bh;

		bh = __find_get_block(bdev, block, size);
		if (bh) {
			touch_buffer(bh);
			return bh;
		}

		if (!grow_buffers(bdev, block, size))
			free_more_memory();
	}
}

/*
 * The relationship between dirty buffers and dirty pages:
 *
 * Whenever a page has any dirty buffers, the page's dirty bit is set, and
 * the page appears on its address_space.dirty_pages list.
 *
 * At all times, the dirtiness of the buffers represents the dirtiness of
 * subsections of the page.  If the page has buffers, the page dirty bit is
 * merely a hint about the true dirty state.
 *
 * When a page is set dirty in its entirety, all its buffers are marked dirty
 * (if the page has buffers).
 *
 * When a buffer is marked dirty, its page is dirtied, but the page's other
 * buffers are not.
 *
 * Also.  When blockdev buffers are explicitly read with bread(), they
 * individually become uptodate.  But their backing page remains not
 * uptodate - even if all of its buffers are uptodate.  A subsequent
 * block_read_full_page() against that page will discover all the uptodate
 * buffers, will set the page uptodate and will perform no I/O.
 */

/**
 * mark_buffer_dirty - mark a buffer_head as needing writeout
 *
 * mark_buffer_dirty() will set the dirty bit against the buffer,
 * then set its backing page dirty, then attach the page to its
 * address_space's dirty_pages list and then attach the address_space's
 * inode to its superblock's dirty inode list.
 *
 * mark_buffer_dirty() is atomic.  It takes bh->b_page->mapping->private_lock,
 * mapping->page_lock and the global inode_lock.
 */
void mark_buffer_dirty(struct buffer_head *bh)
{
	if (!buffer_uptodate(bh))
		buffer_error();
	if (!buffer_dirty(bh) && !test_set_buffer_dirty(bh))
		__set_page_dirty_nobuffers(bh->b_page);
}

/*
 * Decrement a buffer_head's reference count.  If all buffers against a page
 * have zero reference count, are clean and unlocked, and if the page is clean
 * and unlocked then try_to_free_buffers() may strip the buffers from the page
 * in preparation for freeing it (sometimes, rarely, buffers are removed from
 * a page but it ends up not being freed, and buffers may later be reattached).
 */
void __brelse(struct buffer_head * buf)
{
	if (atomic_read(&buf->b_count)) {
		put_bh(buf);
		return;
	}
	printk(KERN_ERR "VFS: brelse: Trying to free free buffer\n");
	buffer_error();		/* For the stack backtrace */
}

/*
 * bforget() is like brelse(), except it discards any
 * potentially dirty data.
 */
void __bforget(struct buffer_head *bh)
{
	clear_buffer_dirty(bh);
	if (!list_empty(&bh->b_assoc_buffers)) {
		struct address_space *buffer_mapping = bh->b_page->mapping;

		spin_lock(&buffer_mapping->private_lock);
		list_del_init(&bh->b_assoc_buffers);
		spin_unlock(&buffer_mapping->private_lock);
	}
	__brelse(bh);
}

/**
 *  __bread() - reads a specified block and returns the bh
 *  @block: number of block
 *  @size: size (in bytes) to read
 * 
 *  Reads a specified block, and returns buffer head that contains it.
 *  It returns NULL if the block was unreadable.
 */
struct buffer_head *
__bread_slow(struct block_device *bdev, sector_t block, int size)
{
	struct buffer_head *bh = __getblk(bdev, block, size);

	if (buffer_uptodate(bh))
		return bh;
	lock_buffer(bh);
	if (buffer_uptodate(bh)) {
		unlock_buffer(bh);
		return bh;
	} else {
		if (buffer_dirty(bh))
			buffer_error();
		get_bh(bh);
		bh->b_end_io = end_buffer_io_sync;
		submit_bh(READ, bh);
		wait_on_buffer(bh);
		if (buffer_uptodate(bh))
			return bh;
	}
	brelse(bh);
	return NULL;
}

/*
 * Per-cpu buffer LRU implementation.  To reduce the cost of __find_get_block().
 * The bhs[] array is sorted - newest buffer is at bhs[0].  Buffers have their
 * refcount elevated by one when they're in an LRU.  A buffer can only appear
 * once in a particular CPU's LRU.  A single buffer can be present in multiple
 * CPU's LRUs at the same time.
 *
 * This is a transparent caching front-end to sb_bread(), sb_getblk() and
 * sb_find_get_block().
 *
 * The LRUs themselves only need locking against invalidate_bh_lrus.  We use
 * a local interrupt disable for that.
 */

#define BH_LRU_SIZE	8

static struct bh_lru {
	struct buffer_head *bhs[BH_LRU_SIZE];
} ____cacheline_aligned_in_smp bh_lrus[NR_CPUS];

#ifdef CONFIG_SMP
#define bh_lru_lock()	local_irq_disable()
#define bh_lru_unlock()	local_irq_enable()
#else
#define bh_lru_lock()	preempt_disable()
#define bh_lru_unlock()	preempt_enable()
#endif

static inline void check_irqs_on(void)
{
#ifdef irqs_disabled
	BUG_ON(irqs_disabled());
#endif
}

/*
 * The LRU management algorithm is dopey-but-simple.  Sorry.
 */
static void bh_lru_install(struct buffer_head *bh)
{
	struct buffer_head *evictee = NULL;
	struct bh_lru *lru;

	if (bh == NULL)
		return;

	check_irqs_on();
	bh_lru_lock();
	lru = &bh_lrus[smp_processor_id()];
	if (lru->bhs[0] != bh) {
		struct buffer_head *bhs[BH_LRU_SIZE];
		int in;
		int out = 0;

		get_bh(bh);
		bhs[out++] = bh;
		for (in = 0; in < BH_LRU_SIZE; in++) {
			struct buffer_head *bh2 = lru->bhs[in];

			if (bh2 == bh) {
				__brelse(bh2);
			} else {
				if (out >= BH_LRU_SIZE) {
					BUG_ON(evictee != NULL);
					evictee = bh2;
				} else {
					bhs[out++] = bh2;
				}
			}
		}
		while (out < BH_LRU_SIZE)
			bhs[out++] = NULL;
		memcpy(lru->bhs, bhs, sizeof(bhs));
	}
	bh_lru_unlock();

	if (evictee) {
		touch_buffer(evictee);
		__brelse(evictee);
	}
}

static inline struct buffer_head *
lookup_bh(struct block_device *bdev, sector_t block, int size)
{
	struct buffer_head *ret = NULL;
	struct bh_lru *lru;
	int i;

	check_irqs_on();
	bh_lru_lock();
	lru = &bh_lrus[smp_processor_id()];
	for (i = 0; i < BH_LRU_SIZE; i++) {
		struct buffer_head *bh = lru->bhs[i];

		if (bh && bh->b_bdev == bdev &&
				bh->b_blocknr == block && bh->b_size == size) {
			if (i) {
				while (i) {
					lru->bhs[i] = lru->bhs[i - 1];
					i--;
				}
				lru->bhs[0] = bh;
			}
			get_bh(bh);
			ret = bh;
			break;
		}
	}
	bh_lru_unlock();
	return ret;
}

struct buffer_head *
__find_get_block(struct block_device *bdev, sector_t block, int size)
{
	struct buffer_head *bh = lookup_bh(bdev, block, size);

	if (bh == NULL) {
		bh = __find_get_block_slow(bdev, block, size);
		bh_lru_install(bh);
	}
	return bh;
}
EXPORT_SYMBOL(__find_get_block);

struct buffer_head *
__getblk(struct block_device *bdev, sector_t block, int size)
{
	struct buffer_head *bh = __find_get_block(bdev, block, size);

	if (bh == NULL) {
		bh = __getblk_slow(bdev, block, size);
		bh_lru_install(bh);
	}
	return bh;
}
EXPORT_SYMBOL(__getblk);

struct buffer_head *
__bread(struct block_device *bdev, sector_t block, int size)
{
	struct buffer_head *bh = __getblk(bdev, block, size);

	if (bh) {
		if (buffer_uptodate(bh))
			return bh;
		__brelse(bh);
	}
	bh = __bread_slow(bdev, block, size);
	bh_lru_install(bh);
	return bh;
}
EXPORT_SYMBOL(__bread);

/*
 * invalidate_bh_lrus() is called rarely - at unmount.  Because it is only for
 * unmount it only needs to ensure that all buffers from the target device are
 * invalidated on return and it doesn't need to worry about new buffers from
 * that device being added - the unmount code has to prevent that.
 */
static void invalidate_bh_lru(void *arg)
{
	const int cpu = get_cpu();
	int i;

	for (i = 0; i < BH_LRU_SIZE; i++) {
		brelse(bh_lrus[cpu].bhs[i]);
		bh_lrus[cpu].bhs[i] = NULL;
	}
	put_cpu();
}
	
static void invalidate_bh_lrus(void)
{
	preempt_disable();
	invalidate_bh_lru(NULL);
	smp_call_function(invalidate_bh_lru, NULL, 1, 1);
	preempt_enable();
}



void set_bh_page(struct buffer_head *bh,
		struct page *page, unsigned long offset)
{
	bh->b_page = page;
	if (offset >= PAGE_SIZE)
		BUG();
	if (PageHighMem(page))
		/*
		 * This catches illegal uses and preserves the offset:
		 */
		bh->b_data = (char *)(0 + offset);
	else
		bh->b_data = page_address(page) + offset;
}
EXPORT_SYMBOL(set_bh_page);

/*
 * Called when truncating a buffer on a page completely.
 */
static /* inline */ void discard_buffer(struct buffer_head * bh)
{
	lock_buffer(bh);
	clear_buffer_dirty(bh);
	bh->b_bdev = NULL;
	clear_buffer_mapped(bh);
	clear_buffer_req(bh);
	clear_buffer_new(bh);
	unlock_buffer(bh);
}

/**
 * try_to_release_page() - release old fs-specific metadata on a page
 *
 * @page: the page which the kernel is trying to free
 * @gfp_mask: memory allocation flags (and I/O mode)
 *
 * The address_space is to try to release any data against the page
 * (presumably at page->private).  If the release was successful, return `1'.
 * Otherwise return zero.
 *
 * The @gfp_mask argument specifies whether I/O may be performed to release
 * this page (__GFP_IO), and whether the call may block (__GFP_WAIT).
 *
 * NOTE: @gfp_mask may go away, and this function may become non-blocking.
 */
int try_to_release_page(struct page *page, int gfp_mask)
{
	struct address_space * const mapping = page->mapping;

	if (!PageLocked(page))
		BUG();
	if (PageWriteback(page))
		return 0;
	
	if (mapping && mapping->a_ops->releasepage)
		return mapping->a_ops->releasepage(page, gfp_mask);
	return try_to_free_buffers(page);
}

/**
 * block_invalidatepage - invalidate part of all of a buffer-backed page
 *
 * @page: the page which is affected
 * @offset: the index of the truncation point
 *
 * block_invalidatepage() is called when all or part of the page has become
 * invalidatedby a truncate operation.
 *
 * block_invalidatepage() does not have to release all buffers, but it must
 * ensure that no dirty buffer is left outside @offset and that no I/O
 * is underway against any of the blocks which are outside the truncation
 * point.  Because the caller is about to free (and possibly reuse) those
 * blocks on-disk.
 */
int block_invalidatepage(struct page *page, unsigned long offset)
{
	struct buffer_head *head, *bh, *next;
	unsigned int curr_off = 0;
	int ret = 1;

	BUG_ON(!PageLocked(page));
	if (!page_has_buffers(page))
		goto out;

	head = page_buffers(page);
	bh = head;
	do {
		unsigned int next_off = curr_off + bh->b_size;
		next = bh->b_this_page;

		/*
		 * is this block fully invalidated?
		 */
		if (offset <= curr_off)
			discard_buffer(bh);
		curr_off = next_off;
		bh = next;
	} while (bh != head);

	/*
	 * We release buffers only if the entire page is being invalidated.
	 * The get_block cached value has been unconditionally invalidated,
	 * so real IO is not possible anymore.
	 */
	if (offset == 0)
		ret = try_to_release_page(page, 0);
out:
	return ret;
}
EXPORT_SYMBOL(block_invalidatepage);

/*
 * We attach and possibly dirty the buffers atomically wrt
 * __set_page_dirty_buffers() via private_lock.  try_to_free_buffers
 * is already excluded via the page lock.
 */
void create_empty_buffers(struct page *page,
			unsigned long blocksize, unsigned long b_state)
{
	struct buffer_head *bh, *head, *tail;

	head = create_buffers(page, blocksize, 1);
	bh = head;
	do {
		bh->b_state |= b_state;
		tail = bh;
		bh = bh->b_this_page;
	} while (bh);
	tail->b_this_page = head;

	spin_lock(&page->mapping->private_lock);
	if (PageUptodate(page) || PageDirty(page)) {
		bh = head;
		do {
			if (PageDirty(page))
				set_buffer_dirty(bh);
			if (PageUptodate(page))
				set_buffer_uptodate(bh);
			bh = bh->b_this_page;
		} while (bh != head);
	}
	__set_page_buffers(page, head);
	spin_unlock(&page->mapping->private_lock);
}
EXPORT_SYMBOL(create_empty_buffers);

/*
 * We are taking a block for data and we don't want any output from any
 * buffer-cache aliases starting from return from that function and
 * until the moment when something will explicitly mark the buffer
 * dirty (hopefully that will not happen until we will free that block ;-)
 * We don't even need to mark it not-uptodate - nobody can expect
 * anything from a newly allocated buffer anyway. We used to used
 * unmap_buffer() for such invalidation, but that was wrong. We definitely
 * don't want to mark the alias unmapped, for example - it would confuse
 * anyone who might pick it with bread() afterwards...
 *
 * Also..  Note that bforget() doesn't lock the buffer.  So there can
 * be writeout I/O going on against recently-freed buffers.  We don't
 * wait on that I/O in bforget() - it's more efficient to wait on the I/O
 * only if we really need to.  That happens here.
 */
void unmap_underlying_metadata(struct block_device *bdev, sector_t block)
{
	struct buffer_head *old_bh;

	old_bh = __find_get_block(bdev, block, 0);
	if (old_bh) {
#if 0	/* This happens.  Later. */
		if (buffer_dirty(old_bh))
			buffer_error();
#endif
		clear_buffer_dirty(old_bh);
		wait_on_buffer(old_bh);
		clear_buffer_req(old_bh);
		__brelse(old_bh);
	}
}
EXPORT_SYMBOL(unmap_underlying_metadata);

/*
 * NOTE! All mapped/uptodate combinations are valid:
 *
 *	Mapped	Uptodate	Meaning
 *
 *	No	No		"unknown" - must do get_block()
 *	No	Yes		"hole" - zero-filled
 *	Yes	No		"allocated" - allocated on disk, not read in
 *	Yes	Yes		"valid" - allocated and up-to-date in memory.
 *
 * "Dirty" is valid only with the last case (mapped+uptodate).
 */

/*
 * While block_write_full_page is writing back the dirty buffers under
 * the page lock, whoever dirtied the buffers may decide to clean them
 * again at any time.  We handle that by only looking at the buffer
 * state inside lock_buffer().
 *
 * If block_write_full_page() is called for regular writeback
 * (called_for_sync() is false) then it will return -EAGAIN for a locked
 * buffer.   This only can happen if someone has written the buffer directly,
 * with submit_bh().  At the address_space level PageWriteback prevents this
 * contention from occurring.
 */
static int __block_write_full_page(struct inode *inode,
			struct page *page, get_block_t *get_block)
{
	int err;
	int ret = 0;
	unsigned long block;
	unsigned long last_block;
	struct buffer_head *bh, *head;
	int nr_underway = 0;

	BUG_ON(!PageLocked(page));

	last_block = (inode->i_size - 1) >> inode->i_blkbits;

	if (!page_has_buffers(page)) {
		if (S_ISBLK(inode->i_mode))
			buffer_error();
		if (!PageUptodate(page))
			buffer_error();
		create_empty_buffers(page, 1 << inode->i_blkbits,
					(1 << BH_Dirty)|(1 << BH_Uptodate));
	}

	/*
	 * Be very careful.  We have no exclusion from __set_page_dirty_buffers
	 * here, and the (potentially unmapped) buffers may become dirty at
	 * any time.  If a buffer becomes dirty here after we've inspected it
	 * then we just miss that fact, and the page stays dirty.
	 *
	 * Buffers outside i_size may be dirtied by __set_page_dirty_buffers;
	 * handle that here by just cleaning them.
	 */

	block = page->index << (PAGE_CACHE_SHIFT - inode->i_blkbits);
	head = page_buffers(page);
	bh = head;

	/*
	 * Get all the dirty buffers mapped to disk addresses and
	 * handle any aliases from the underlying blockdev's mapping.
	 */
	do {
		if (block > last_block) {
			/*
			 * mapped buffers outside i_size will occur, because
			 * this page can be outside i_size when there is a
			 * truncate in progress.
			 *
			 * if (buffer_mapped(bh))
			 *	buffer_error();
			 */
			/*
			 * The buffer was zeroed by block_write_full_page()
			 */
			clear_buffer_dirty(bh);
			set_buffer_uptodate(bh);
		} else if (!buffer_mapped(bh) && buffer_dirty(bh)) {
			if (buffer_new(bh))
				buffer_error();
			err = get_block(inode, block, bh, 1);
			if (err)
				goto recover;
			if (buffer_new(bh)) {
				/* blockdev mappings never come here */
				clear_buffer_new(bh);
				unmap_underlying_metadata(bh->b_bdev,
							bh->b_blocknr);
			}
		}
		bh = bh->b_this_page;
		block++;
	} while (bh != head);

	do {
		get_bh(bh);
		if (buffer_mapped(bh) && buffer_dirty(bh)) {
			if (called_for_sync()) {
				lock_buffer(bh);
			} else {
				if (test_set_buffer_locked(bh)) {
					ret = -EAGAIN;
					continue;
				}
			}
			if (test_clear_buffer_dirty(bh)) {
				if (!buffer_uptodate(bh))
					buffer_error();
				mark_buffer_async_write(bh);
			} else {
				unlock_buffer(bh);
			}
		}
	} while ((bh = bh->b_this_page) != head);

	BUG_ON(PageWriteback(page));
	SetPageWriteback(page);		/* Keeps try_to_free_buffers() away */
	unlock_page(page);

	/*
	 * The page may come unlocked any time after the *first* submit_bh()
	 * call.  Be careful with its buffers.
	 */
	do {
		struct buffer_head *next = bh->b_this_page;
		if (buffer_async_write(bh)) {
			submit_bh(WRITE, bh);
			nr_underway++;
		}
		put_bh(bh);
		bh = next;
	} while (bh != head);

	err = 0;
done:
	if (nr_underway == 0) {
		/*
		 * The page was marked dirty, but the buffers were
		 * clean.  Someone wrote them back by hand with
		 * ll_rw_block/submit_bh.  A rare case.
		 */
		int uptodate = 1;
		do {
			if (!buffer_uptodate(bh)) {
				uptodate = 0;
				break;
			}
			bh = bh->b_this_page;
		} while (bh != head);
		if (uptodate)
			SetPageUptodate(page);
		end_page_writeback(page);
	}
	if (err == 0)
		return ret;
	return err;

recover:
	/*
	 * ENOSPC, or some other error.  We may already have added some
	 * blocks to the file, so we need to write these out to avoid
	 * exposing stale data.
	 * The page is currently locked and not marked for writeback
	 */
	ClearPageUptodate(page);
	bh = head;
	/* Recovery: lock and submit the mapped buffers */
	do {
		get_bh(bh);
		if (buffer_mapped(bh) && buffer_dirty(bh)) {
			lock_buffer(bh);
			mark_buffer_async_write(bh);
		} else {
			/*
			 * The buffer may have been set dirty during
			 * attachment to a dirty page.
			 */
			clear_buffer_dirty(bh);
		}
	} while ((bh = bh->b_this_page) != head);
	SetPageError(page);
	BUG_ON(PageWriteback(page));
	SetPageWriteback(page);
	unlock_page(page);
	do {
		struct buffer_head *next = bh->b_this_page;
		if (buffer_async_write(bh)) {
			clear_buffer_dirty(bh);
			submit_bh(WRITE, bh);
			nr_underway++;
		}
		put_bh(bh);
		bh = next;
	} while (bh != head);
	goto done;
}

static int __block_prepare_write(struct inode *inode, struct page *page,
		unsigned from, unsigned to, get_block_t *get_block)
{
	unsigned block_start, block_end;
	unsigned long block;
	int err = 0;
	unsigned blocksize, bbits;
	struct buffer_head *bh, *head, *wait[2], **wait_bh=wait;

	BUG_ON(!PageLocked(page));
	BUG_ON(from > PAGE_CACHE_SIZE);
	BUG_ON(to > PAGE_CACHE_SIZE);
	BUG_ON(from > to);

	blocksize = 1 << inode->i_blkbits;
	if (!page_has_buffers(page))
		create_empty_buffers(page, blocksize, 0);
	head = page_buffers(page);

	bbits = inode->i_blkbits;
	block = page->index << (PAGE_CACHE_SHIFT - bbits);

	for(bh = head, block_start = 0; bh != head || !block_start;
	    block++, block_start=block_end, bh = bh->b_this_page) {
		block_end = block_start + blocksize;
		if (block_end <= from || block_start >= to) {
			if (PageUptodate(page)) {
				if (!buffer_uptodate(bh))
					set_buffer_uptodate(bh);
			}
			continue;
		}
		if (buffer_new(bh))
			clear_buffer_new(bh);
		if (!buffer_mapped(bh)) {
			err = get_block(inode, block, bh, 1);
			if (err)
				goto out;
			if (buffer_new(bh)) {
				clear_buffer_new(bh);
				unmap_underlying_metadata(bh->b_bdev,
							bh->b_blocknr);
				if (PageUptodate(page)) {
					if (!buffer_mapped(bh))
						buffer_error();
					set_buffer_uptodate(bh);
					continue;
				}
				if (block_end > to || block_start < from) {
					void *kaddr;

					kaddr = kmap_atomic(page, KM_USER0);
					if (block_end > to)
						memset(kaddr+to, 0,
							block_end-to);
					if (block_start < from)
						memset(kaddr+block_start,
							0, from-block_start);
					flush_dcache_page(page);
					kunmap_atomic(kaddr, KM_USER0);
				}
				continue;
			}
		}
		if (PageUptodate(page)) {
			if (!buffer_uptodate(bh))
				set_buffer_uptodate(bh);
			continue; 
		}
		if (!buffer_uptodate(bh) &&
		     (block_start < from || block_end > to)) {
			ll_rw_block(READ, 1, &bh);
			*wait_bh++=bh;
		}
	}
	/*
	 * If we issued read requests - let them complete.
	 */
	while(wait_bh > wait) {
		wait_on_buffer(*--wait_bh);
		if (!buffer_uptodate(*wait_bh))
			return -EIO;
	}
	return 0;
out:
	/*
	 * Zero out any newly allocated blocks to avoid exposing stale
	 * data.  If BH_New is set, we know that the block was newly
	 * allocated in the above loop.
	 */
	bh = head;
	block_start = 0;
	do {
		block_end = block_start+blocksize;
		if (block_end <= from)
			goto next_bh;
		if (block_start >= to)
			break;
		if (buffer_new(bh)) {
			void *kaddr;

			clear_buffer_new(bh);
			if (buffer_uptodate(bh))
				buffer_error();
			kaddr = kmap_atomic(page, KM_USER0);
			memset(kaddr+block_start, 0, bh->b_size);
			kunmap_atomic(kaddr, KM_USER0);
			set_buffer_uptodate(bh);
			mark_buffer_dirty(bh);
		}
next_bh:
		block_start = block_end;
		bh = bh->b_this_page;
	} while (bh != head);
	return err;
}

static int __block_commit_write(struct inode *inode, struct page *page,
		unsigned from, unsigned to)
{
	unsigned block_start, block_end;
	int partial = 0;
	unsigned blocksize;
	struct buffer_head *bh, *head;

	blocksize = 1 << inode->i_blkbits;

	for(bh = head = page_buffers(page), block_start = 0;
	    bh != head || !block_start;
	    block_start=block_end, bh = bh->b_this_page) {
		block_end = block_start + blocksize;
		if (block_end <= from || block_start >= to) {
			if (!buffer_uptodate(bh))
				partial = 1;
		} else {
			set_buffer_uptodate(bh);
			mark_buffer_dirty(bh);
		}
	}

	/*
	 * If this is a partial write which happened to make all buffers
	 * uptodate then we can optimize away a bogus readpage() for
	 * the next read(). Here we 'discover' whether the page went
	 * uptodate as a result of this (potentially partial) write.
	 */
	if (!partial)
		SetPageUptodate(page);
	return 0;
}

/*
 * Generic "read page" function for block devices that have the normal
 * get_block functionality. This is most of the block device filesystems.
 * Reads the page asynchronously --- the unlock_buffer() and
 * set/clear_buffer_uptodate() functions propagate buffer state into the
 * page struct once IO has completed.
 */
int block_read_full_page(struct page *page, get_block_t *get_block)
{
	struct inode *inode = page->mapping->host;
	unsigned long iblock, lblock;
	struct buffer_head *bh, *head, *arr[MAX_BUF_PER_PAGE];
	unsigned int blocksize, blocks;
	int nr, i;

	if (!PageLocked(page))
		PAGE_BUG(page);
	if (PageUptodate(page))
		buffer_error();
	blocksize = 1 << inode->i_blkbits;
	if (!page_has_buffers(page))
		create_empty_buffers(page, blocksize, 0);
	head = page_buffers(page);

	blocks = PAGE_CACHE_SIZE >> inode->i_blkbits;
	iblock = page->index << (PAGE_CACHE_SHIFT - inode->i_blkbits);
	lblock = (inode->i_size+blocksize-1) >> inode->i_blkbits;
	bh = head;
	nr = 0;
	i = 0;

	do {
		if (buffer_uptodate(bh))
			continue;

		if (!buffer_mapped(bh)) {
			if (iblock < lblock) {
				if (get_block(inode, iblock, bh, 0))
					SetPageError(page);
			}
			if (!buffer_mapped(bh)) {
				void *kaddr = kmap_atomic(page, KM_USER0);
				memset(kaddr + i * blocksize, 0, blocksize);
				flush_dcache_page(page);
				kunmap_atomic(kaddr, KM_USER0);
				set_buffer_uptodate(bh);
				continue;
			}
			/*
			 * get_block() might have updated the buffer
			 * synchronously
			 */
			if (buffer_uptodate(bh))
				continue;
		}
		arr[nr++] = bh;
	} while (i++, iblock++, (bh = bh->b_this_page) != head);

	if (!nr) {
		/*
		 * All buffers are uptodate - we can set the page uptodate
		 * as well. But not if get_block() returned an error.
		 */
		if (!PageError(page))
			SetPageUptodate(page);
		unlock_page(page);
		return 0;
	}

	/* Stage two: lock the buffers */
	for (i = 0; i < nr; i++) {
		bh = arr[i];
		lock_buffer(bh);
		mark_buffer_async_read(bh);
	}

	/*
	 * Stage 3: start the IO.  Check for uptodateness
	 * inside the buffer lock in case another process reading
	 * the underlying blockdev brought it uptodate (the sct fix).
	 */
	for (i = 0; i < nr; i++) {
		bh = arr[i];
		if (buffer_uptodate(bh))
			end_buffer_async_read(bh, 1);
		else
			submit_bh(READ, bh);
	}
	return 0;
}

/* utility function for filesystems that need to do work on expanding
 * truncates.  Uses prepare/commit_write to allow the filesystem to
 * deal with the hole.  
 */
int generic_cont_expand(struct inode *inode, loff_t size)
{
	struct address_space *mapping = inode->i_mapping;
	struct page *page;
	unsigned long index, offset, limit;
	int err;

	err = -EFBIG;
        limit = current->rlim[RLIMIT_FSIZE].rlim_cur;
	if (limit != RLIM_INFINITY && size > (loff_t)limit) {
		send_sig(SIGXFSZ, current, 0);
		goto out;
	}
	if (size > inode->i_sb->s_maxbytes)
		goto out;

	offset = (size & (PAGE_CACHE_SIZE-1)); /* Within page */

	/* ugh.  in prepare/commit_write, if from==to==start of block, we 
	** skip the prepare.  make sure we never send an offset for the start
	** of a block
	*/
	if ((offset & (inode->i_sb->s_blocksize - 1)) == 0) {
		offset++;
	}
	index = size >> PAGE_CACHE_SHIFT;
	err = -ENOMEM;
	page = grab_cache_page(mapping, index);
	if (!page)
		goto out;
	err = mapping->a_ops->prepare_write(NULL, page, offset, offset);
	if (!err) {
		err = mapping->a_ops->commit_write(NULL, page, offset, offset);
	}
	unlock_page(page);
	page_cache_release(page);
	if (err > 0)
		err = 0;
out:
	return err;
}

/*
 * For moronic filesystems that do not allow holes in file.
 * We may have to extend the file.
 */

int cont_prepare_write(struct page *page, unsigned offset,
		unsigned to, get_block_t *get_block, loff_t *bytes)
{
	struct address_space *mapping = page->mapping;
	struct inode *inode = mapping->host;
	struct page *new_page;
	unsigned long pgpos;
	long status;
	unsigned zerofrom;
	unsigned blocksize = 1 << inode->i_blkbits;
	void *kaddr;

	while(page->index > (pgpos = *bytes>>PAGE_CACHE_SHIFT)) {
		status = -ENOMEM;
		new_page = grab_cache_page(mapping, pgpos);
		if (!new_page)
			goto out;
		/* we might sleep */
		if (*bytes>>PAGE_CACHE_SHIFT != pgpos) {
			unlock_page(new_page);
			page_cache_release(new_page);
			continue;
		}
		zerofrom = *bytes & ~PAGE_CACHE_MASK;
		if (zerofrom & (blocksize-1)) {
			*bytes |= (blocksize-1);
			(*bytes)++;
		}
		status = __block_prepare_write(inode, new_page, zerofrom,
						PAGE_CACHE_SIZE, get_block);
		if (status)
			goto out_unmap;
		kaddr = kmap_atomic(new_page, KM_USER0);
		memset(kaddr+zerofrom, 0, PAGE_CACHE_SIZE-zerofrom);
		flush_dcache_page(new_page);
		kunmap_atomic(kaddr, KM_USER0);
		__block_commit_write(inode, new_page,
				zerofrom, PAGE_CACHE_SIZE);
		unlock_page(new_page);
		page_cache_release(new_page);
	}

	if (page->index < pgpos) {
		/* completely inside the area */
		zerofrom = offset;
	} else {
		/* page covers the boundary, find the boundary offset */
		zerofrom = *bytes & ~PAGE_CACHE_MASK;

		/* if we will expand the thing last block will be filled */
		if (to > zerofrom && (zerofrom & (blocksize-1))) {
			*bytes |= (blocksize-1);
			(*bytes)++;
		}

		/* starting below the boundary? Nothing to zero out */
		if (offset <= zerofrom)
			zerofrom = offset;
	}
	status = __block_prepare_write(inode, page, zerofrom, to, get_block);
	if (status)
		goto out1;
	if (zerofrom < offset) {
		kaddr = kmap_atomic(page, KM_USER0);
		memset(kaddr+zerofrom, 0, offset-zerofrom);
		flush_dcache_page(page);
		kunmap_atomic(kaddr, KM_USER0);
		__block_commit_write(inode, page, zerofrom, offset);
	}
	return 0;
out1:
	ClearPageUptodate(page);
	return status;

out_unmap:
	ClearPageUptodate(new_page);
	unlock_page(new_page);
	page_cache_release(new_page);
out:
	return status;
}

int block_prepare_write(struct page *page, unsigned from, unsigned to,
			get_block_t *get_block)
{
	struct inode *inode = page->mapping->host;
	int err = __block_prepare_write(inode, page, from, to, get_block);
	if (err)
		ClearPageUptodate(page);
	return err;
}

int block_commit_write(struct page *page, unsigned from, unsigned to)
{
	struct inode *inode = page->mapping->host;
	__block_commit_write(inode,page,from,to);
	return 0;
}

int generic_commit_write(struct file *file, struct page *page,
		unsigned from, unsigned to)
{
	struct inode *inode = page->mapping->host;
	loff_t pos = ((loff_t)page->index << PAGE_CACHE_SHIFT) + to;
	__block_commit_write(inode,page,from,to);
	if (pos > inode->i_size) {
		inode->i_size = pos;
		mark_inode_dirty(inode);
	}
	return 0;
}

int block_truncate_page(struct address_space *mapping,
			loff_t from, get_block_t *get_block)
{
	unsigned long index = from >> PAGE_CACHE_SHIFT;
	unsigned offset = from & (PAGE_CACHE_SIZE-1);
	unsigned blocksize, iblock, length, pos;
	struct inode *inode = mapping->host;
	struct page *page;
	struct buffer_head *bh;
	void *kaddr;
	int err;

	blocksize = 1 << inode->i_blkbits;
	length = offset & (blocksize - 1);

	/* Block boundary? Nothing to do */
	if (!length)
		return 0;

	length = blocksize - length;
	iblock = index << (PAGE_CACHE_SHIFT - inode->i_blkbits);
	
	page = grab_cache_page(mapping, index);
	err = -ENOMEM;
	if (!page)
		goto out;

	if (!page_has_buffers(page))
		create_empty_buffers(page, blocksize, 0);

	/* Find the buffer that contains "offset" */
	bh = page_buffers(page);
	pos = blocksize;
	while (offset >= pos) {
		bh = bh->b_this_page;
		iblock++;
		pos += blocksize;
	}

	err = 0;
	if (!buffer_mapped(bh)) {
		err = get_block(inode, iblock, bh, 0);
		if (err)
			goto unlock;
		/* unmapped? It's a hole - nothing to do */
		if (!buffer_mapped(bh))
			goto unlock;
	}

	/* Ok, it's mapped. Make sure it's up-to-date */
	if (PageUptodate(page))
		set_buffer_uptodate(bh);

	if (!buffer_uptodate(bh)) {
		err = -EIO;
		ll_rw_block(READ, 1, &bh);
		wait_on_buffer(bh);
		/* Uhhuh. Read error. Complain and punt. */
		if (!buffer_uptodate(bh))
			goto unlock;
	}

	kaddr = kmap_atomic(page, KM_USER0);
	memset(kaddr + offset, 0, length);
	flush_dcache_page(page);
	kunmap_atomic(kaddr, KM_USER0);

	mark_buffer_dirty(bh);
	err = 0;

unlock:
	unlock_page(page);
	page_cache_release(page);
out:
	return err;
}

/*
 * The generic ->writepage function for buffer-backed address_spaces
 */
int block_write_full_page(struct page *page, get_block_t *get_block)
{
	struct inode * const inode = page->mapping->host;
	const unsigned long end_index = inode->i_size >> PAGE_CACHE_SHIFT;
	unsigned offset;
	void *kaddr;

	/* Is the page fully inside i_size? */
	if (page->index < end_index)
		return __block_write_full_page(inode, page, get_block);

	/* Is the page fully outside i_size? (truncate in progress) */
	offset = inode->i_size & (PAGE_CACHE_SIZE-1);
	if (page->index >= end_index+1 || !offset) {
		unlock_page(page);
		return -EIO;
	}

	/*
	 * The page straddles i_size.  It must be zeroed out on each and every
	 * writepage invokation because it may be mmapped.  "A file is mapped
	 * in multiples of the page size.  For a file that is not a multiple of
	 * the  page size, the remaining memory is zeroed when mapped, and
	 * writes to that region are not written out to the file."
	 */
	kaddr = kmap_atomic(page, KM_USER0);
	memset(kaddr + offset, 0, PAGE_CACHE_SIZE - offset);
	flush_dcache_page(page);
	kunmap_atomic(kaddr, KM_USER0);
	return __block_write_full_page(inode, page, get_block);
}

sector_t generic_block_bmap(struct address_space *mapping, sector_t block,
			    get_block_t *get_block)
{
	struct buffer_head tmp;
	struct inode *inode = mapping->host;
	tmp.b_state = 0;
	tmp.b_blocknr = 0;
	get_block(inode, block, &tmp, 0);
	return tmp.b_blocknr;
}

/*
 * Start I/O on a physical range of kernel memory, defined by a vector
 * of kiobuf structs (much like a user-space iovec list).
 *
 * The kiobuf must already be locked for IO.  IO is submitted
 * asynchronously: you need to check page->locked and page->uptodate.
 *
 * It is up to the caller to make sure that there are enough blocks
 * passed in to completely map the iobufs to disk.
 */
int brw_kiovec(int rw, int nr, struct kiobuf *iovec[],
	       struct block_device *bdev, sector_t b[], int size)
{
	int		transferred;
	int		i;
	int		err;
	struct kiobuf *	iobuf;

	if (!nr)
		return 0;
	
	/* 
	 * First, do some alignment and validity checks 
	 */
	for (i = 0; i < nr; i++) {
		iobuf = iovec[i];
		if ((iobuf->offset & (size-1)) || (iobuf->length & (size-1)))
			return -EINVAL;
		if (!iobuf->nr_pages)
			panic("brw_kiovec: iobuf not initialised");
	}

	/* 
	 * OK to walk down the iovec doing page IO on each page we find. 
	 */
	for (i = 0; i < nr; i++) {
		iobuf = iovec[i];
		iobuf->errno = 0;

		ll_rw_kio(rw, iobuf, bdev, b[i] * (size >> 9));
	}

	/*
	 * now they are all submitted, wait for completion
	 */
	transferred = 0;
	err = 0;
	for (i = 0; i < nr; i++) {
		iobuf = iovec[i];
		kiobuf_wait_for_io(iobuf);
		if (iobuf->errno && !err)
			err = iobuf->errno;
		if (!err)
			transferred += iobuf->length;
	}

	return err ? err : transferred;
}

static int end_bio_bh_io_sync(struct bio *bio, unsigned int bytes_done, int err)
{
	struct buffer_head *bh = bio->bi_private;

	if (bio->bi_size)
		return 1;

	bh->b_end_io(bh, test_bit(BIO_UPTODATE, &bio->bi_flags));
	bio_put(bio);
	return 0;
}

int submit_bh(int rw, struct buffer_head * bh)
{
	struct bio *bio;

	BUG_ON(!buffer_locked(bh));
	BUG_ON(!buffer_mapped(bh));
	BUG_ON(!bh->b_end_io);

	if ((rw == READ || rw == READA) && buffer_uptodate(bh))
		buffer_error();
	if (rw == WRITE && !buffer_uptodate(bh))
		buffer_error();
	if (rw == READ && buffer_dirty(bh))
		buffer_error();
				
	set_buffer_req(bh);

	/*
	 * from here on down, it's all bio -- do the initial mapping,
	 * submit_bio -> generic_make_request may further map this bio around
	 */
	bio = bio_alloc(GFP_NOIO, 1);

	bio->bi_sector = bh->b_blocknr * (bh->b_size >> 9);
	bio->bi_bdev = bh->b_bdev;
	bio->bi_io_vec[0].bv_page = bh->b_page;
	bio->bi_io_vec[0].bv_len = bh->b_size;
	bio->bi_io_vec[0].bv_offset = bh_offset(bh);

	bio->bi_vcnt = 1;
	bio->bi_idx = 0;
	bio->bi_size = bh->b_size;

	bio->bi_end_io = end_bio_bh_io_sync;
	bio->bi_private = bh;

	return submit_bio(rw, bio);
}

/**
 * ll_rw_block: low-level access to block devices (DEPRECATED)
 * @rw: whether to %READ or %WRITE or maybe %READA (readahead)
 * @nr: number of &struct buffer_heads in the array
 * @bhs: array of pointers to &struct buffer_head
 *
 * ll_rw_block() takes an array of pointers to &struct buffer_heads,
 * and requests an I/O operation on them, either a %READ or a %WRITE.
 * The third %READA option is described in the documentation for
 * generic_make_request() which ll_rw_block() calls.
 *
 * This function drops any buffer that it cannot get a lock on (with the
 * BH_Lock state bit), any buffer that appears to be clean when doing a
 * write request, and any buffer that appears to be up-to-date when doing
 * read request.  Further it marks as clean buffers that are processed for
 * writing (the buffer cache wont assume that they are actually clean until
 * the buffer gets unlocked).
 *
 * ll_rw_block sets b_end_io to simple completion handler that marks
 * the buffer up-to-date (if approriate), unlocks the buffer and wakes
 * any waiters. 
 *
 * All of the buffers must be for the same device, and must also be a
 * multiple of the current approved size for the device.
 */
void ll_rw_block(int rw, int nr, struct buffer_head * bhs[])
{
	unsigned int major;
	int correct_size;
	int i;

	if (!nr)
		return;

	major = major(to_kdev_t(bhs[0]->b_bdev->bd_dev));

	/* Determine correct block size for this device. */
	correct_size = bdev_hardsect_size(bhs[0]->b_bdev);

	/* Verify requested block sizes. */
	for (i = 0; i < nr; i++) {
		struct buffer_head *bh = bhs[i];
		if (bh->b_size & (correct_size - 1)) {
			printk(KERN_NOTICE "ll_rw_block: device %s: "
			       "only %d-char blocks implemented (%u)\n",
			       bdevname(bhs[0]->b_bdev),
			       correct_size, bh->b_size);
			goto sorry;
		}
	}

	if ((rw & WRITE) && bdev_read_only(bhs[0]->b_bdev)) {
		printk(KERN_NOTICE "Can't write to read-only device %s\n",
		       bdevname(bhs[0]->b_bdev));
		goto sorry;
	}

	for (i = 0; i < nr; i++) {
		struct buffer_head *bh = bhs[i];

		/* Only one thread can actually submit the I/O. */
		if (test_set_buffer_locked(bh))
			continue;

		/* We have the buffer lock */
		atomic_inc(&bh->b_count);
		bh->b_end_io = end_buffer_io_sync;

		switch(rw) {
		case WRITE:
			if (!test_clear_buffer_dirty(bh))
				/* Hmmph! Nothing to write */
				goto end_io;
			break;

		case READA:
		case READ:
			if (buffer_uptodate(bh))
				/* Hmmph! Already have it */
				goto end_io;
			break;
		default:
			BUG();
	end_io:
			bh->b_end_io(bh, buffer_uptodate(bh));
			continue;
		}

		submit_bh(rw, bh);
	}
	return;

sorry:
	/* Make sure we don't get infinite dirty retries.. */
	for (i = 0; i < nr; i++)
		clear_buffer_dirty(bhs[i]);
}

/*
 * Sanity checks for try_to_free_buffers.
 */
static void check_ttfb_buffer(struct page *page, struct buffer_head *bh)
{
	if (!buffer_uptodate(bh)) {
		if (PageUptodate(page) && page->mapping
			&& buffer_mapped(bh)	/* discard_buffer */
			&& S_ISBLK(page->mapping->host->i_mode))
		{
			buffer_error();
		}
	}
}

/*
 * try_to_free_buffers() checks if all the buffers on this particular page
 * are unused, and releases them if so.
 *
 * Exclusion against try_to_free_buffers may be obtained by either
 * locking the page or by holding its mapping's private_lock.
 *
 * If the page is dirty but all the buffers are clean then we need to
 * be sure to mark the page clean as well.  This is because the page
 * may be against a block device, and a later reattachment of buffers
 * to a dirty page will set *all* buffers dirty.  Which would corrupt
 * filesystem data on the same device.
 *
 * The same applies to regular filesystem pages: if all the buffers are
 * clean then we set the page clean and proceed.  To do that, we require
 * total exclusion from __set_page_dirty_buffers().  That is obtained with
 * private_lock.
 *
 * try_to_free_buffers() is non-blocking.
 */
static inline int buffer_busy(struct buffer_head *bh)
{
	return atomic_read(&bh->b_count) |
		(bh->b_state & ((1 << BH_Dirty) | (1 << BH_Lock)));
}

static inline int
drop_buffers(struct page *page, struct buffer_head **buffers_to_free)
{
	struct buffer_head *head = page_buffers(page);
	struct buffer_head *bh;
	int was_uptodate = 1;

	bh = head;
	do {
		check_ttfb_buffer(page, bh);
		if (buffer_busy(bh))
			goto failed;
		if (!buffer_uptodate(bh))
			was_uptodate = 0;
		bh = bh->b_this_page;
	} while (bh != head);

	if (!was_uptodate && PageUptodate(page))
		buffer_error();

	do {
		struct buffer_head *next = bh->b_this_page;

		if (!list_empty(&bh->b_assoc_buffers))
			__remove_assoc_queue(bh);
		bh = next;
	} while (bh != head);
	*buffers_to_free = head;
	__clear_page_buffers(page);
	return 1;
failed:
	return 0;
}

int try_to_free_buffers(struct page *page)
{
	struct address_space * const mapping = page->mapping;
	struct buffer_head *buffers_to_free = NULL;
	int ret = 0;

	BUG_ON(!PageLocked(page));
	if (PageWriteback(page))
		return 0;

	if (mapping == NULL) {		/* swapped-in anon page */
		ret = drop_buffers(page, &buffers_to_free);
		goto out;
	}

	spin_lock(&mapping->private_lock);
	ret = drop_buffers(page, &buffers_to_free);
	if (ret && !PageSwapCache(page)) {
		/*
		 * If the filesystem writes its buffers by hand (eg ext3)
		 * then we can have clean buffers against a dirty page.  We
		 * clean the page here; otherwise later reattachment of buffers
		 * could encounter a non-uptodate page, which is unresolvable.
		 * This only applies in the rare case where try_to_free_buffers
		 * succeeds but the page is not freed.
		 */
		clear_page_dirty(page);
	}
	spin_unlock(&mapping->private_lock);
out:
	if (buffers_to_free) {
		struct buffer_head *bh = buffers_to_free;

		do {
			struct buffer_head *next = bh->b_this_page;
			free_buffer_head(bh);
			bh = next;
		} while (bh != buffers_to_free);
	}
	return ret;
}
EXPORT_SYMBOL(try_to_free_buffers);

int block_sync_page(struct page *page)
{
	blk_run_queues();
	return 0;
}

/*
 * There are no bdflush tunables left.  But distributions are
 * still running obsolete flush daemons, so we terminate them here.
 */
asmlinkage long sys_bdflush(int func, long data)
{
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (func == 1)
		do_exit(0);
	return 0;
}

/*
 * Buffer-head allocation
 */
static kmem_cache_t *bh_cachep;
static mempool_t *bh_mempool;

/*
 * Once the number of bh's in the machine exceeds this level, we start
 * stripping them in writeback.
 */
static int max_buffer_heads;

int buffer_heads_over_limit;

struct bh_accounting {
	int nr;			/* Number of live bh's */
	int ratelimit;		/* Limit cacheline bouncing */
};

static DEFINE_PER_CPU(struct bh_accounting, bh_accounting) = {0, 0};

static void recalc_bh_state(void)
{
	int i;
	int tot = 0;

	if (__get_cpu_var(bh_accounting).ratelimit++ < 4096)
		return;
	__get_cpu_var(bh_accounting).ratelimit = 0;
	for (i = 0; i < NR_CPUS; i++)
		tot += per_cpu(bh_accounting, i).nr;
	buffer_heads_over_limit = (tot > max_buffer_heads);
}
	
struct buffer_head *alloc_buffer_head(void)
{
	struct buffer_head *ret = mempool_alloc(bh_mempool, GFP_NOFS);
	if (ret) {
		preempt_disable();
		__get_cpu_var(bh_accounting).nr++;
		recalc_bh_state();
		preempt_enable();
	}
	return ret;
}
EXPORT_SYMBOL(alloc_buffer_head);

void free_buffer_head(struct buffer_head *bh)
{
	BUG_ON(!list_empty(&bh->b_assoc_buffers));
	mempool_free(bh, bh_mempool);
	preempt_disable();
	__get_cpu_var(bh_accounting).nr--;
	recalc_bh_state();
	preempt_enable();
}
EXPORT_SYMBOL(free_buffer_head);

static void init_buffer_head(void *data, kmem_cache_t *cachep, unsigned long flags)
{
	if ((flags & (SLAB_CTOR_VERIFY|SLAB_CTOR_CONSTRUCTOR)) ==
			    SLAB_CTOR_CONSTRUCTOR) {
		struct buffer_head * bh = (struct buffer_head *)data;

		memset(bh, 0, sizeof(*bh));
		INIT_LIST_HEAD(&bh->b_assoc_buffers);
	}
}

static void *bh_mempool_alloc(int gfp_mask, void *pool_data)
{
	return kmem_cache_alloc(bh_cachep, gfp_mask);
}

static void bh_mempool_free(void *element, void *pool_data)
{
	return kmem_cache_free(bh_cachep, element);
}

#define NR_RESERVED (10*MAX_BUF_PER_PAGE)
#define MAX_UNUSED_BUFFERS NR_RESERVED+20

void __init buffer_init(void)
{
	int i;
	int nrpages;

	bh_cachep = kmem_cache_create("buffer_head",
			sizeof(struct buffer_head), 0,
			0, init_buffer_head, NULL);
	bh_mempool = mempool_create(MAX_UNUSED_BUFFERS, bh_mempool_alloc,
				bh_mempool_free, NULL);
	for (i = 0; i < ARRAY_SIZE(bh_wait_queue_heads); i++)
		init_waitqueue_head(&bh_wait_queue_heads[i].wqh);

	/*
	 * Limit the bh occupancy to 10% of ZONE_NORMAL
	 */
	nrpages = (nr_free_buffer_pages() * 10) / 100;
	max_buffer_heads = nrpages * (PAGE_SIZE / sizeof(struct buffer_head));
}
