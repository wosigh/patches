/*
 * Copyright (C) 2005-2009 Junjiro Okajima
 *
 * This program, aufs is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

/*
 * module global variables and operations
 *
 * $Id: module.c,v 1.38 2009/01/26 06:24:45 sfjro Exp $
 */

#include <linux/module.h>
#include "aufs.h"

/* ---------------------------------------------------------------------- */

/*
 * aufs caches
 */
struct kmem_cache *au_cachep[AuCache_Last];
static int __init create_cache(void)
{
	au_cachep[AuCache_DINFO] = AuCache(au_dinfo);
	if (au_cachep[AuCache_DINFO])
		au_cachep[AuCache_ICNTNR] = AuCache(aufs_icntnr);
	if (au_cachep[AuCache_ICNTNR])
		au_cachep[AuCache_FINFO] = AuCache(au_finfo);
	//au_cachep[AuCache_FINFO] = NULL;
	if (au_cachep[AuCache_FINFO])
		au_cachep[AuCache_VDIR] = AuCache(au_vdir);
	if (au_cachep[AuCache_VDIR])
		au_cachep[AuCache_DEHSTR] = AuCache(au_vdir_dehstr);
	if (au_cachep[AuCache_DEHSTR])
		return 0;

	return -ENOMEM;
}

static void destroy_cache(void)
{
	int i;
	for (i = 0; i < AuCache_Last; i++)
		if (au_cachep[i]) {
			kmem_cache_destroy(au_cachep[i]);
			au_cachep[i] = NULL;
		}
}

/* ---------------------------------------------------------------------- */

char au_esc_chars[0x20 + 3]; /* 0x01-0x20, backslash, del, and NULL */
int au_dir_roflags;

/*
 * functions for module interface.
 */
MODULE_LICENSE("GPL");
/* MODULE_LICENSE("GPL v2"); */
MODULE_AUTHOR("Junjiro Okajima");
MODULE_DESCRIPTION(AUFS_NAME " -- Another unionfs");
MODULE_VERSION(AUFS_VERSION);

/* it should be 'byte', but param_set_byte() prints it by "%c" */
short aufs_nwkq = AUFS_NWKQ_DEF;
MODULE_PARM_DESC(nwkq, "the number of workqueue thread, " AUFS_WKQ_NAME);
module_param_named(nwkq, aufs_nwkq, short, S_IRUGO);

int sysaufs_brs;
MODULE_PARM_DESC(brs, "use <sysfs>/fs/aufs/sbi_*/brN");
module_param_named(brs, sysaufs_brs, int, S_IRUGO);

/* ---------------------------------------------------------------------- */

static int __init aufs_init(void)
{
	int err, i;
	char *p;

	au_debug_init();
#ifdef CONFIG_AUFS_INO_T_64
	BUILD_BUG_ON(sizeof(ino_t) != sizeof(long long));
#else
	BUILD_BUG_ON(sizeof(ino_t) != sizeof(int));
#endif

	p = au_esc_chars;
	for (i = 1; i <= ' '; i++)
		*p++ = i;
	*p++ = '\\';
	*p++ = '\x7f';
	*p = 0;

	au_dir_roflags = au_file_roflags(O_DIRECTORY | O_LARGEFILE);

	err = -EINVAL;
	if (unlikely(aufs_nwkq <= 0))
		goto out;

	err = sysaufs_init();
	if (unlikely(err))
		goto out;
	err = au_wkq_init();
	if (unlikely(err))
		goto out_sysaufs;
	err = au_inotify_init();
	if (unlikely(err))
		goto out_wkq;
	err = au_sysrq_init();
	if (unlikely(err))
		goto out_inotify;

	err = create_cache();
	if (unlikely(err))
		goto out_sysrq;

	err = register_filesystem(&aufs_fs_type);
	if (unlikely(err))
		goto out_cache;
	pr_info(AUFS_NAME " " AUFS_VERSION "\n");
	return 0; /* success */

 out_cache:
	destroy_cache();
 out_sysrq:
	au_sysrq_fin();
 out_inotify:
	au_inotify_fin();
 out_wkq:
	au_wkq_fin();
 out_sysaufs:
	sysaufs_fin();
 out:
	AuTraceErr(err);
	return err;
}

static void __exit aufs_exit(void)
{
	unregister_filesystem(&aufs_fs_type);
	destroy_cache();

	au_sysrq_fin();
	au_inotify_fin();
	au_wkq_fin();
	sysaufs_fin();
}

module_init(aufs_init);
module_exit(aufs_exit);

/* ---------------------------------------------------------------------- */

/* fake Kconfig */
#if 1

#if AUFS_BRANCH_MAX > 511 && PAGE_SIZE > 4096
#warning pagesize is larger than 4kb, \
	CONFIG_AUFS_BRANCH_MAX_511 or smaller is recommended.
#endif

#ifdef CONFIG_AUFS_SYSAUFS
#ifndef CONFIG_SYSFS
#error CONFIG_AUFS_SYSAUFS requires CONFIG_SYSFS.
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 18)
#error CONFIG_AUFS_SYSAUFS requires linux-2.6.18 and later.
#endif
#endif /* CONFIG_AUFS_SYSAUFS */

#ifdef CONFIG_AUFS_HINOTIFY
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 18)
#error CONFIG_AUFS_HINOTIFY is supported in linux-2.6.18 and later.
#endif
#ifndef CONFIG_INOTIFY
#error enable CONFIG_INOTIFY to use CONFIG_AUFS_HINOTIFY.
#endif
#endif /* CONFIG_AUFS_HINOTIFY */

#ifdef CONFIG_AUFS_EXPORT
#if !defined(CONFIG_EXPORTFS) && !defined(CONFIG_EXPORTFS_MODULE)
#error CONFIG_AUFS_EXPORT requires CONFIG_EXPORTFS
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 18)
#error CONFIG_AUFS_EXPORT requires linux-2.6.18 and later.
#endif
#if defined(CONFIG_EXPORTFS_MODULE) && defined(CONFIG_AUFS)
#error need CONFIG_EXPORTFS = y to link aufs statically with CONFIG_AUFS_EXPORT
#endif
#endif /* CONFIG_AUFS_EXPORT */

#ifdef CONFIG_AUFS_SEC_PERM_PATCH
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 24)
#warning CONFIG_AUFS_SEC_PERM_PATCH does not support before linux-2.6.24.
#endif
#ifndef CONFIG_SECURITY
#warning AUFS_SEC_PERM_PATCH is unnecessary since SECURITY is disabled.
#endif
#ifdef CONFIG_AUFS
#warning AUFS_SEC_PERM_PATCH is unnecessary since AUFS is not a module.
#endif
#endif

#if defined(CONFIG_AUFS_SPLICE_PATCH) \
	&& LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 23)
#error CONFIG_AUFS_SPLICE_PATCH is supported linux-2.6.23 and later.
#endif

#ifdef CONFIG_AUFS_LHASH_PATCH
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 19)
#error CONFIG_AUFS_LHASH_PATCH does not support before linux-2.6.19.
#endif
#if !defined(CONFIG_NFS_FS) && !defined(CONFIG_NFS_FS_MODULE)
#warning CONFIG_AUFS_LHASH_PATCH is unnecessary since CONFIG_NFS_FS is disabled.
#endif
#endif

#ifdef CONFIG_AUFS_PUT_FILP_PATCH
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 19)
#warning CONFIG_AUFS_PUT_FILP_PATCH does not support before linux-2.6.19.
#endif
#ifndef CONFIG_NFS_V4
#warning AUFS_PUT_FILP_PATCH is unnecessary since NFS_V4 is disabled.
#endif
#ifdef CONFIG_AUFS
#warning AUFS_PUT_FILP_PATCH is unnecessary since AUFS is not a module.
#endif
#endif /* CONFIG_AUFS_PUT_FILP_PATCH */

#ifdef CONFIG_AUFS_FSYNC_SUPER_PATCH
#ifdef CONFIG_AUFS
#warning AUFS_FSYNC_SUPER_PATCH is unnecessary since AUFS is not a module.
#endif
#endif

#ifdef CONFIG_DENY_WRITE_ACCESS_PATCH
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 17)
#warning AUFS_DENY_WRITE_ACCESS_PATCH is supported in linux-2.6.17 and later.
#endif
#ifdef CONFIG_AUFS
#warning AUFS_DENY_WRITE_ACCESS_PATCH is unnecessary since AUFS is not a module.
#endif
#endif

#ifdef CONFIG_AUFS_KSIZE_PATCH
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 22)
#warning CONFIG_AUFS_KSIZE_PATCH is unnecessary for linux-2.6.22 and later.
#endif
#ifdef CONFIG_AUFS
#warning CONFIG_AUFS_KSIZE_PATCH is unnecessary since AUFS is not a module.
#endif
#endif

#ifdef CONFIG_AUFS_WORKAROUND_FUSE
#if !defined(CONFIG_FUSE_FS) && !defined(CONFIG_FUSE_FS_MODULE)
#warning CONFIG_AUFS_WORKAROUND_FUSE is enabled while FUSE is disabled.
#endif
#endif

#ifdef CONFIG_AUFS_DEBUG_LOCK
#ifndef CONFIG_AUFS_MAGIC_SYSRQ
#warning CONFIG_AUFS_DEBUG_LOCK is enabled but CONFIG_AUFS_MAGIC_SYSRQ.
#endif
#endif

#ifdef CONFIG_AUFS_COMPAT
#warning CONFIG_AUFS_COMPAT will be removed in the near future.
#endif

#ifdef CONFIG_AUFS_UNIONFS23_PATCH
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 23) \
	&& !defined(CONFIG_AUFS_SPLICE_PATCH)
#error mis-config. AUFS_UNIONFS23_PATCH is enabled but AUFS_SPLICE_PATCH.
#endif
#endif

#ifdef CONFIG_DEBUG_PROVE_LOCKING
#if MAX_LOCKDEP_SUBCLASSES < AuLsc_I_End
#warning lockdep will not work since aufs uses deeper locks.
#endif
#endif

#endif
