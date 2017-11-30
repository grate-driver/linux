#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <asm/pgtable.h>

static int ptdump_show(struct seq_file *m, void *v)
{
	ptdump_walk_pgd_level_debugfs(m, NULL, false);
	return 0;
}

static int ptdump_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, ptdump_show, NULL);
}

static const struct file_operations ptdump_fops = {
	.owner		= THIS_MODULE,
	.open		= ptdump_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int ptdump_show_curknl(struct seq_file *m, void *v)
{
	if (current->mm->pgd) {
		down_read(&current->mm->mmap_sem);
		ptdump_walk_pgd_level_debugfs(m, current->mm->pgd, false);
		up_read(&current->mm->mmap_sem);
	}
	return 0;
}

static int ptdump_open_curknl(struct inode *inode, struct file *filp)
{
	return single_open(filp, ptdump_show_curknl, NULL);
}

static const struct file_operations ptdump_curknl_fops = {
	.owner		= THIS_MODULE,
	.open		= ptdump_open_curknl,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

#ifdef CONFIG_KAISER
static int ptdump_show_curusr(struct seq_file *m, void *v)
{
	if (current->mm->pgd) {
		down_read(&current->mm->mmap_sem);
		ptdump_walk_pgd_level_debugfs(m, current->mm->pgd, true);
		up_read(&current->mm->mmap_sem);
	}
	return 0;
}

static int ptdump_open_curusr(struct inode *inode, struct file *filp)
{
	return single_open(filp, ptdump_show_curusr, NULL);
}

static const struct file_operations ptdump_curusr_fops = {
	.owner		= THIS_MODULE,
	.open		= ptdump_open_curusr,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};
#endif

static struct dentry *pe_knl, *pe_curknl, *pe_curusr;

static void pt_dump_debug_remove_files(void)
{
	debugfs_remove_recursive(pe_knl);
	debugfs_remove_recursive(pe_curknl);
	debugfs_remove_recursive(pe_curusr);
}

static int __init pt_dump_debug_init(void)
{
	pe_knl = debugfs_create_file("kernel_page_tables", S_IRUSR, NULL, NULL,
				     &ptdump_fops);
	if (!pe_knl)
		return -ENOMEM;

	pe_curknl = debugfs_create_file("current_page_tables_knl", S_IRUSR,
					NULL, NULL, &ptdump_curknl_fops);
	if (!pe_curknl)
		goto err;

#ifdef CONFIG_KAISER
	pe_curusr = debugfs_create_file("current_page_tables_usr", S_IRUSR,
					NULL, NULL, &ptdump_curusr_fops);
	if (!pe_curusr)
		goto err;
#endif
	return 0;
err:
	pt_dump_debug_remove_files();
	return -ENOMEM;
}

static void __exit pt_dump_debug_exit(void)
{
	pt_dump_debug_remove_files();
}

module_init(pt_dump_debug_init);
module_exit(pt_dump_debug_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Arjan van de Ven <arjan@linux.intel.com>");
MODULE_DESCRIPTION("Kernel debugging helper that dumps pagetables");
