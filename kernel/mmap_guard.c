// SPDX-License-Identifier: GPL-2.0
/*
 * ====================================================================================
 * mmap_guard.c — kernel-side protection for one file mapped with mmap()
 * ====================================================================================
 * Sistemas Operativos, Parcial 3. Loadable module for Linux 5.10+ / 6.x.
 *
 * Once a page table entry exists, reads and writes through a mapping never enter the
 * kernel again, so the check has to happen when the mapping is created or widened.
 * The module puts kretprobes on the two LSM entry points every path goes through:
 *
 *   security_mmap_file(file, prot, flags)        mmap(2) creating the VMA
 *   security_file_mprotect(vma, reqprot, prot)   mprotect(2) widening an existing VMA
 *
 * For the guarded file (module parameter `path`) the policy is:
 *
 *   PROT_WRITE + MAP_SHARED   denied   writes would reach the page cache and the disk
 *   PROT_EXEC                 denied   the file is data, never code
 *   anything else             allowed  read-only maps, and private COW copies
 * A denied call returns -EACCES to the process. Every decision is written to the
 * kernel log and counted in /sys/module/mmap_guard/parameters/{allowed,denied}:
 *   sudo insmod mmap_guard.ko path=/tmp/moon_secret.txt
 */

#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/cred.h>
#include <linux/sched.h>
#include <linux/atomic.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Pablo Manjarres");
MODULE_DESCRIPTION("Deny writable shared and executable mmap() of one protected file");

/* ------------------------------------------------------------------------------------
 * Parameters: the file to protect, and two read-only counters
 * ------------------------------------------------------------------------------------ */
static char *path = "/tmp/moon_secret.txt";
module_param(path, charp, 0444);
MODULE_PARM_DESC(path, "absolute path of the file to protect");

static atomic_long_t n_allowed = ATOMIC_LONG_INIT(0);
static atomic_long_t n_denied  = ATOMIC_LONG_INIT(0);

static int counter_get(char *buf, const struct kernel_param *kp)
{
    return scnprintf(buf, PAGE_SIZE, "%ld\n", atomic_long_read(kp->arg));
}

static const struct kernel_param_ops counter_ops = { .get = counter_get };
module_param_cb(allowed, &counter_ops, &n_allowed, 0444);
module_param_cb(denied,  &counter_ops, &n_denied,  0444);

/* The inode is resolved once at load time. Holding the path keeps it alive, and
 * comparing one pointer is all the hot path costs for every other file. */
static struct path guarded_path;
static struct inode *guarded_inode;

/* ------------------------------------------------------------------------------------
 * Policy
 * ------------------------------------------------------------------------------------ */

/* Returns why a mapping is refused, or NULL when it may go ahead. */
static const char *verdict(unsigned long prot, bool shared)
{
    if (prot & PROT_EXEC)
        return "exec";
    if (shared && (prot & PROT_WRITE))
        return "shared-write";
    return NULL;
}

/* What the entry handler hands to the return handler of the same call. */
struct decision {
    bool deny;
};

/* Logs and counts one decision on the guarded file. Returns true to deny. */
static bool judge(const char *hook, unsigned long prot, bool shared)
{
    const char *reason = verdict(prot, shared);

    if (!reason) {
        atomic_long_inc(&n_allowed);
        pr_info_ratelimited("mmap_guard: ALLOW %s pid=%d uid=%u comm=%s prot=%#lx %s\n",
                            hook, task_pid_nr(current),
                            from_kuid(&init_user_ns, current_uid()), current->comm,
                            prot, shared ? "shared" : "private");
        return false;
    }

    atomic_long_inc(&n_denied);
    pr_warn_ratelimited("mmap_guard: DENY %s (%s) pid=%d uid=%u comm=%s prot=%#lx %s\n",
                        hook, reason, task_pid_nr(current),
                        from_kuid(&init_user_ns, current_uid()), current->comm,
                        prot, shared ? "shared" : "private");
    return true;
}

/* ------------------------------------------------------------------------------------
 * Probes. Entry handlers return non-zero to skip the return handler: that is the
 * fast path for every file that is not the guarded one.
 * ------------------------------------------------------------------------------------ */

/* int security_mmap_file(struct file *file, unsigned long prot, unsigned long flags) */
static int mmap_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    struct file *file = (struct file *)regs_get_kernel_argument(regs, 0);
    unsigned long prot  = regs_get_kernel_argument(regs, 1);
    unsigned long flags = regs_get_kernel_argument(regs, 2);
    struct decision *d = (struct decision *)ri->data;

    if (!file || file_inode(file) != guarded_inode)
        return 1;

    d->deny = judge("mmap", prot, (flags & MAP_TYPE) != MAP_PRIVATE);
    return d->deny ? 0 : 1;
}

/* int security_file_mprotect(struct vm_area_struct *vma, unsigned long reqprot,
 *                            unsigned long prot) */
static int mprotect_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    struct vm_area_struct *vma = (struct vm_area_struct *)regs_get_kernel_argument(regs, 0);
    unsigned long prot = regs_get_kernel_argument(regs, 2);
    struct decision *d = (struct decision *)ri->data;

    if (!vma || !vma->vm_file || file_inode(vma->vm_file) != guarded_inode)
        return 1;

    d->deny = judge("mprotect", prot, vma->vm_flags & VM_SHARED);
    return d->deny ? 0 : 1;
}

/* Turns a successful LSM answer into -EACCES. The caller sees it before any VMA is
 * created or changed, so nothing has to be undone. */
static int deny_return(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    struct decision *d = (struct decision *)ri->data;

    if (d->deny && regs_return_value(regs) == 0)
        regs_set_return_value(regs, -EACCES);
    return 0;
}

static struct kretprobe mmap_probe = {
    .kp.symbol_name = "security_mmap_file",
    .entry_handler  = mmap_entry,
    .handler        = deny_return,
    .data_size      = sizeof(struct decision),
    .maxactive      = 64,
};

static struct kretprobe mprotect_probe = {
    .kp.symbol_name = "security_file_mprotect",
    .entry_handler  = mprotect_entry,
    .handler        = deny_return,
    .data_size      = sizeof(struct decision),
    .maxactive      = 64,
};

static struct kretprobe *probes[] = { &mmap_probe, &mprotect_probe };

/* Resolves the guarded inode, then arms both probes. */
static int __init mmap_guard_init(void)
{
    int err = kern_path(path, LOOKUP_FOLLOW, &guarded_path);

    if (err) {
        pr_err("mmap_guard: cannot resolve '%s' (%d)\n", path, err);
        return err;
    }
    guarded_inode = d_inode(guarded_path.dentry);

    err = register_kretprobes(probes, ARRAY_SIZE(probes));
    if (err) {
        pr_err("mmap_guard: register_kretprobes failed (%d)\n", err);
        path_put(&guarded_path);
        return err;
    }

    pr_info("mmap_guard: guarding %s (inode %lu)\n", path, guarded_inode->i_ino);
    return 0;
}

static void __exit mmap_guard_exit(void)
{
    unregister_kretprobes(probes, ARRAY_SIZE(probes));
    path_put(&guarded_path);
    pr_info("mmap_guard: unloaded, allowed=%ld denied=%ld\n",
            atomic_long_read(&n_allowed), atomic_long_read(&n_denied));
}

module_init(mmap_guard_init);
module_exit(mmap_guard_exit);
