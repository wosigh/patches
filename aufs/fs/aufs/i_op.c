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
 * inode operations (except add/del/rename)
 *
 * $Id: i_op.c,v 1.79 2009/01/26 06:24:45 sfjro Exp $
 */

//#include <linux/fs.h>
//#include <linux/namei.h>
#include <linux/mm.h>
#include "aufs.h"

static int silly_lock(struct inode *inode, struct nameidata *nd)
{
	int locked = 0;
	struct super_block *sb = inode->i_sb;

	LKTRTrace("i%lu, nd %p\n", inode->i_ino, nd);

	if (!nd || !nd->dentry) {
		si_read_lock(sb, AuLock_FLUSH);
		ii_read_lock_child(inode);
	} else if (nd->dentry->d_inode != inode) {
		locked = 1;
		/* lock child first, then parent */
		si_read_lock(sb, AuLock_FLUSH);
		ii_read_lock_child(inode);
		di_read_lock_parent(nd->dentry, 0);
	} else {
		locked = 2;
		aufs_read_lock(nd->dentry, AuLock_FLUSH | AuLock_IR);
	}
	return locked;
}

static void silly_unlock(int locked, struct inode *inode, struct nameidata *nd)
{
	struct super_block *sb = inode->i_sb;

	LKTRTrace("locked %d, i%lu, nd %p\n", locked, inode->i_ino, nd);

	switch (locked) {
	case 0:
		ii_read_unlock(inode);
		si_read_unlock(sb);
		break;
	case 1:
		di_read_unlock(nd->dentry, 0);
		ii_read_unlock(inode);
		si_read_unlock(sb);
		break;
	case 2:
		aufs_read_unlock(nd->dentry, AuLock_FLUSH | AuLock_IR);
		break;
	default:
		BUG();
	}
}

static int h_permission(struct inode *h_inode, int mask,
			struct nameidata *fake_nd, int brperm, int dlgt)
{
	int err, submask;
	const int write_mask = (mask & (MAY_WRITE | MAY_APPEND));

	LKTRTrace("ino %lu, mask 0x%x, brperm 0x%x\n",
		  h_inode->i_ino, mask, brperm);

	err = -EACCES;
	if ((write_mask && IS_IMMUTABLE(h_inode))
	    || ((mask & MAY_EXEC) && S_ISREG(h_inode->i_mode)
		&& fake_nd && fake_nd->mnt
		&& (fake_nd->mnt->mnt_flags & MNT_NOEXEC)))
		goto out;

	/*
	 * - skip hidden fs test in the case of write to ro branch.
	 * - nfs dir permission write check is optimized, but a policy for
	 *   link/rename requires a real check.
	 */
	submask = mask & ~MAY_APPEND;
	if ((write_mask && !au_br_writable(brperm))
	    || (au_test_nfs(h_inode->i_sb) && S_ISDIR(h_inode->i_mode)
		&& write_mask && !(mask & MAY_READ))
	    || !h_inode->i_op
	    || !h_inode->i_op->permission) {
		/* LKTRLabel(generic_permission); */
		err = generic_permission(h_inode, submask, NULL);
	} else {
		/* LKTRLabel(h_inode->permission); */
		err = h_inode->i_op->permission(h_inode, submask, fake_nd);
		AuTraceErr(err);
	}

#if 1 /* todo: export? */
	if (!err)
		err = au_security_inode_permission(h_inode, mask, fake_nd,
						   dlgt);
#endif

 out:
	AuTraceErr(err);
	return err;
}

static int aufs_permission(struct inode *inode, int mask, struct nameidata *nd)
{
	int err;
	aufs_bindex_t bindex, bend;
	unsigned char locked, dlgt, do_nd;
	const unsigned char isdir = S_ISDIR(inode->i_mode);
	struct inode *h_inode;
	struct super_block *sb;
	unsigned int mnt_flags;
	struct path path;
	const int write_mask = (mask & (MAY_WRITE | MAY_APPEND));

	LKTRTrace("ino %lu, mask 0x%x, isdir %d, write_mask %d, "
		  "nd %d{%d, %d}\n",
		  inode->i_ino, mask, isdir, write_mask,
		  !!nd, nd ? !!nd->dentry : 0, nd ? !!nd->mnt : 0);

	sb = inode->i_sb;
	locked = silly_lock(inode, nd);
	do_nd = (nd && locked >= 1);
	mnt_flags = au_mntflags(sb);
	dlgt = !!au_test_dlgt(mnt_flags);

	if (!isdir || write_mask || au_test_dirperm1(mnt_flags)) {
		h_inode = au_h_iptr(inode, au_ibstart(inode));
		AuDebugOn(!h_inode
			  || ((h_inode->i_mode & S_IFMT)
			      != (inode->i_mode & S_IFMT)));
		err = 0;
		bindex = au_ibstart(inode);
		LKTRTrace("b%d\n", bindex);
		if (do_nd) {
			path.mnt = nd->mnt;
			path.dentry = nd->dentry;
			nd->mnt = mntget(au_sbr_mnt(sb, bindex));
			nd->dentry = dget(au_h_dptr(nd->dentry, bindex));
			err = h_permission(h_inode, mask, nd,
					   au_sbr_perm(sb, bindex), dlgt);
			path_release(nd);
			nd->mnt = path.mnt;
			nd->dentry = path.dentry;
		} else {
			AuDebugOn(nd && nd->mnt);
			err = h_permission(h_inode, mask, nd,
					   au_sbr_perm(sb, bindex), dlgt);
		}

		if (write_mask && !err) {
			/* test whether the upper writable branch exists */
			err = -EROFS;
			for (; bindex >= 0; bindex--)
				if (!au_br_rdonly(au_sbr(sb, bindex))) {
					err = 0;
					break;
				}
		}
		goto out;
	}

	/* non-write to dir */
	if (do_nd) {
		path.mnt = nd->mnt;
		path.dentry = nd->dentry;
	} else {
		path.mnt = NULL;
		path.dentry = NULL;
	}
	err = 0;
	bend = au_ibend(inode);
	for (bindex = au_ibstart(inode); !err && bindex <= bend; bindex++) {
		h_inode = au_h_iptr(inode, bindex);
		if (!h_inode)
			continue;
		AuDebugOn(!S_ISDIR(h_inode->i_mode));

		LKTRTrace("b%d\n", bindex);
		if (do_nd) {
			nd->mnt = mntget(au_sbr_mnt(sb, bindex));
			nd->dentry = dget(au_h_dptr(path.dentry, bindex));
			err = h_permission(h_inode, mask, nd,
					   au_sbr_perm(sb, bindex), dlgt);
			path_release(nd);
		} else {
			AuDebugOn(nd && nd->mnt);
			err = h_permission(h_inode, mask, nd,
					   au_sbr_perm(sb, bindex), dlgt);
		}
	}
	if (do_nd) {
		nd->mnt = path.mnt;
		nd->dentry = path.dentry;
	}

 out:
	silly_unlock(locked, inode, nd);
	AuTraceErr(err);
	if (unlikely(err == -EBUSY && au_test_nfsd(current)))
		err = -ESTALE;
	return err;
}

/* ---------------------------------------------------------------------- */

static struct dentry *aufs_lookup(struct inode *dir, struct dentry *dentry,
				  struct nameidata *nd)
{
	struct dentry *ret, *parent;
	int err, npositive;
	struct inode *inode, *h_inode;
	struct nameidata tmp_nd, *ndp;
	aufs_bindex_t bstart;
	struct mutex *mtx;
	struct super_block *sb;

	LKTRTrace("dir %lu, %.*s, nd{0x%x}\n",
		  dir->i_ino, AuDLNPair(dentry), nd ? nd->flags : 0);
	AuDebugOn(IS_ROOT(dentry));
	IMustLock(dir);

	sb = dir->i_sb;
	si_read_lock(sb, AuLock_FLUSH);
	err = au_alloc_dinfo(dentry);
	ret = ERR_PTR(err);
	if (unlikely(err))
		goto out;

	/* nd can be NULL */
	ndp = au_dup_nd(au_sbi(sb), &tmp_nd, nd);
	parent = dentry->d_parent; /* dir inode is locked */
	di_read_lock_parent(parent, AuLock_IR);
	npositive = au_lkup_dentry(dentry, au_dbstart(parent), /*type*/0, ndp);
	di_read_unlock(parent, AuLock_IR);
	err = npositive;
	ret = ERR_PTR(err);
	if (unlikely(err < 0))
		goto out_unlock;

	inode = NULL;
	if (npositive) {
		bstart = au_dbstart(dentry);
		h_inode = au_h_dptr(dentry, bstart)->d_inode;
		AuDebugOn(!h_inode);
		if (!S_ISDIR(h_inode->i_mode)) {
			/*
			 * stop 'race'-ing between hardlinks under different
			 * parents.
			 */
			mtx = &au_sbr(sb, bstart)->br_xino.xi_nondir_mtx;
			mutex_lock(mtx);
			inode = au_new_inode(dentry, /*must_new*/0);
			mutex_unlock(mtx);
		} else
			inode = au_new_inode(dentry, /*must_new*/0);
		ret = (void *)inode;
	}
	if (!IS_ERR(inode)) {
		ret = d_splice_alias(inode, dentry);
		if (unlikely(IS_ERR(ret) && inode))
			ii_write_unlock(inode);
		AuDebugOn(nd
			  && (nd->flags & LOOKUP_OPEN)
			  && nd->intent.open.file
			  && nd->intent.open.file->f_dentry);
		au_store_fmode_exec(nd, inode);
	}

 out_unlock:
	di_write_unlock(dentry);
 out:
	si_read_unlock(sb);
	AuTraceErrPtr(ret);
	if (unlikely(err == -EBUSY && au_test_nfsd(current)))
		err = -ESTALE;
	return ret;
}

/* ---------------------------------------------------------------------- */

/*
 * decide the branch and the parent dir where we will create a new entry.
 * returns new bindex or an error.
 * copyup the parent dir if needed.
 */
int au_wr_dir(struct dentry *dentry, struct dentry *src_dentry,
	      struct au_wr_dir_args *args)
{
	int err;
	aufs_bindex_t bcpup, bstart, src_bstart;
	struct super_block *sb;
	struct dentry *parent;
	struct au_sbinfo *sbinfo;
	const int add_entry = au_ftest_wrdir(args->flags, ADD_ENTRY);

	LKTRTrace("%.*s, src %p, {%d, 0x%x}\n",
		  AuDLNPair(dentry), src_dentry, args->force_btgt, args->flags);

	sb = dentry->d_sb;
	sbinfo = au_sbi(sb);
	parent = dget_parent(dentry);
	bstart = au_dbstart(dentry);
	bcpup = bstart;
	if (args->force_btgt < 0) {
		if (src_dentry) {
			src_bstart = au_dbstart(src_dentry);
			if (src_bstart < bstart)
				bcpup = src_bstart;
		} else if (add_entry) {
			err = AuWbrCreate(sbinfo, dentry,
					  au_ftest_wrdir(args->flags, ISDIR));
			bcpup = err;
		}

		if (bcpup < 0 || au_test_ro(sb, bcpup, dentry->d_inode)) {
			if (add_entry)
				err = AuWbrCopyup(sbinfo, dentry);
			else {
				if (!IS_ROOT(dentry)) {
					di_read_lock_parent(parent, !AuLock_IR);
					err = AuWbrCopyup(sbinfo, dentry);
					di_read_unlock(parent, !AuLock_IR);
				} else
					err = AuWbrCopyup(sbinfo, dentry);
			}
			bcpup = err;
			if (unlikely(err < 0))
				goto out;
		}
	} else {
		bcpup = args->force_btgt;
		AuDebugOn(au_test_ro(sb, bcpup, dentry->d_inode));
	}
	LKTRTrace("bstart %d, bcpup %d\n", bstart, bcpup);
	if (bstart < bcpup)
		au_update_dbrange(dentry, /*do_put_zero*/1);

	err = bcpup;
	if (bcpup == bstart)
		goto out; /* success */

	/* copyup the new parent into the branch we process */
	if (add_entry) {
		au_update_dbstart(dentry);
		IMustLock(parent->d_inode);
		DiMustWriteLock(parent);
		IiMustWriteLock(parent->d_inode);
	} else
		di_write_lock_parent(parent);

	err = 0;
	if (!au_h_dptr(parent, bcpup)) {
		if (bstart < bcpup)
			err = au_cpdown_dirs(dentry, bcpup);
		else
			err = au_cpup_dirs(dentry, bcpup);
	}
	if (!err && add_entry) {
		struct dentry *h_parent;
		struct inode *h_dir;

		h_parent = au_h_dptr(parent, bcpup);
		AuDebugOn(!h_parent);
		h_dir = h_parent->d_inode;
		AuDebugOn(!h_dir);
		vfsub_i_lock_nested(h_parent->d_inode, AuLsc_I_PARENT);
		err = au_lkup_neg(dentry, bcpup);
		vfsub_i_unlock(h_parent->d_inode);
		if (bstart < bcpup && au_dbstart(dentry) < 0) {
			au_set_dbstart(dentry, 0);
			au_update_dbrange(dentry, /*do_put_zero*/0);
		}
	}

	if (!add_entry)
		di_write_unlock(parent);
	if (!err)
		err = bcpup; /* success */
 out:
	dput(parent);
	LKTRTrace("err %d\n", err);
	AuTraceErr(err);
	return err;
}

/* ---------------------------------------------------------------------- */

struct dentry *au_do_pinned_h_parent(struct au_pin1 *pin)
{
	if (pin && pin->parent)
		return au_h_dptr(pin->parent, pin->bindex);
	return NULL;
}

void au_do_unpin(struct au_pin1 *p, struct au_pin1 *gp)
{
	if (p->dentry)
		LKTRTrace("%.*s\n", AuDLNPair(p->dentry));
	LKTRTrace("p{%d, %d, %d, %d, 0x%x}, gp %p\n",
		  p->lsc_di, p->lsc_hi, !!p->parent, !!p->h_dir, p->flags, gp);

	if (au_ftest_pin(p->flags, MNT_WRITE))
		au_br_drop_write(au_sbr(p->dentry->d_sb, p->bindex));
	if (au_ftest_pin(p->flags, VFS_RENAME))
		vfsub_unlock_rename_mutex(au_sbr_sb(p->dentry->d_sb,
						    p->bindex));
	if (!p->h_dir)
		return;

	vfsub_i_unlock(p->h_dir);
	if (gp)
		au_do_unpin(gp, NULL);
	if (!au_ftest_pin(p->flags, DI_LOCKED))
		di_read_unlock(p->parent, AuLock_IR);
	iput(p->h_dir);
	dput(p->parent);
	p->parent = NULL;
	p->h_dir = NULL;
}

int au_do_pin(struct au_pin1 *p, struct au_pin1 *gp)
{
	int err;
	struct dentry *h_dentry;

	LKTRTrace("%.*s, %d, b%d, 0x%x\n",
		  AuDLNPair(p->dentry), !!gp, p->bindex, p->flags);
	AuDebugOn(au_ftest_pin(p->flags, DO_GPARENT) && !gp);
	/* AuDebugOn(!do_gp && gp); */

	err = 0;
	if (IS_ROOT(p->dentry)) {
		if (au_ftest_pin(p->flags, VFS_RENAME))
			vfsub_lock_rename_mutex(au_sbr_sb(p->dentry->d_sb,
							  p->bindex));
		if (au_ftest_pin(p->flags, MNT_WRITE)) {
			err = au_br_want_write(au_sbr(p->dentry->d_sb,
						      p->bindex));
			if (unlikely(err))
				au_fclr_pin(p->flags, MNT_WRITE);
		}
		goto out;
	}

	h_dentry = NULL;
	if (p->bindex <= au_dbend(p->dentry))
		h_dentry = au_h_dptr(p->dentry, p->bindex);

	p->parent = dget_parent(p->dentry);
	if (!au_ftest_pin(p->flags, DI_LOCKED))
		di_read_lock(p->parent, AuLock_IR, p->lsc_di);
	else
		DiMustAnyLock(p->parent);
	AuDebugOn(!p->parent->d_inode);
	p->h_dir = au_igrab(au_h_iptr(p->parent->d_inode, p->bindex));
	/* udba case */
	if (unlikely(au_ftest_pin(p->flags, VERIFY) && !p->h_dir)) {
		err = -EIO;
		if (!au_ftest_pin(p->flags, DI_LOCKED))
			di_read_unlock(p->parent, AuLock_IR);
		dput(p->parent);
		p->parent = NULL;
		goto out;
	}

	if (au_ftest_pin(p->flags, DO_GPARENT)) {
		gp->dentry = p->parent;
		err = au_do_pin(gp, NULL);
		if (unlikely(err))
			gp->dentry = NULL;
	}
	if (au_ftest_pin(p->flags, VFS_RENAME))
		vfsub_lock_rename_mutex(p->h_dir->i_sb);
	vfsub_i_lock_nested(p->h_dir, p->lsc_hi);
	if (!err) {
		/* todo: call d_revalidate() here? */
#if 0
		if (h_dentry && h_dentry->d_op && h_dentry->d_op->d_revalidate
		    && !h_dentry->d_op->d_revalidate(h_dentry, NULL))
			err = -EIO;
#endif
		if (!h_dentry
		    || !au_ftest_pin(p->flags, VERIFY)
		    || !au_verify_parent(h_dentry, p->h_dir)) {
			if (au_ftest_pin(p->flags, MNT_WRITE)) {
				err = au_br_want_write(au_sbr(p->dentry->d_sb,
							      p->bindex));
				if (unlikely(err))
					au_fclr_pin(p->flags, MNT_WRITE);
			}
			if (!err)
				goto out; /* success */
		} else
			err = -EIO;
	}

	AuDbgDentry(p->dentry);
	AuDbgDentry(h_dentry);
	AuDbgDentry(p->parent);
	AuDbgInode(p->h_dir);
	if (h_dentry)
		AuDbgDentry(h_dentry->d_parent);

	au_do_unpin(p, gp);
	if (au_ftest_pin(p->flags, DO_GPARENT))
		gp->dentry = NULL;

 out:
	AuTraceErr(err);
	return err;
}

void au_pin_init(struct au_pin *args, struct dentry *dentry,
		 aufs_bindex_t bindex, int lsc_di, int lsc_hi,
		 unsigned char flags)
{
	struct au_pin1 *p;
	unsigned char f;

	AuTraceEnter();

	memset(args, 0, sizeof(*args));
	p = args->pin + AuPin_PARENT;
	p->dentry = dentry;
	p->lsc_di = lsc_di;
	p->lsc_hi = lsc_hi;
	p->flags = flags;
	p->bindex = bindex;
	if (!au_opt_test(au_mntflags(dentry->d_sb), UDBA_NONE))
		au_fset_pin(p->flags, VERIFY);
	if (!au_ftest_pin(flags, DO_GPARENT))
		return;

	f = p->flags;
	p = au_pin_gp(args);
	if (p) {
		p->lsc_di = lsc_di + 1; /* child first */
		p->lsc_hi = lsc_hi - 1; /* parent first */
		p->bindex = bindex;
		p->flags = f & ~(AuPin_MNT_WRITE
				| AuPin_DO_GPARENT
				| AuPin_DI_LOCKED);
	}
}

int au_pin(struct au_pin *args, struct dentry *dentry, aufs_bindex_t bindex,
	   unsigned char flags)
{
	LKTRTrace("%.*s, b%d, 0x%x\n",
		  AuDLNPair(dentry), bindex, flags);

	au_pin_init(args, dentry, bindex, AuLsc_DI_PARENT, AuLsc_I_PARENT2,
		    flags);
	return au_do_pin(args->pin + AuPin_PARENT, au_pin_gp(args));
}

/* ---------------------------------------------------------------------- */

struct au_icpup_args {
	unsigned char isdir, hinotify, did_cpup; /* flags */
	unsigned char pin_flags;
	aufs_bindex_t btgt;
	struct au_pin pin;
	struct au_hin_ignore ign[2];
	struct vfsub_args vargs;
	struct dentry *h_dentry;
	struct inode *h_inode;
};

/* todo: refine it */
static int au_lock_and_icpup(struct dentry *dentry, loff_t sz,
			     struct au_icpup_args *a)
{
	int err;
	aufs_bindex_t bstart;
	struct super_block *sb;
	struct dentry *hi_wh, *parent;
	struct inode *inode;
	struct au_wr_dir_args wr_dir_args = {
		.force_btgt	= -1,
		.flags		= 0
	};

	LKTRTrace("%.*s, %lld\n", AuDLNPair(dentry), sz);

	di_write_lock_child(dentry);
	bstart = au_dbstart(dentry);
	sb = dentry->d_sb;
	inode = dentry->d_inode;
	a->isdir = !!S_ISDIR(inode->i_mode);
	if (a->isdir)
		au_fset_wrdir(wr_dir_args.flags, ISDIR);
	/* plink or hi_wh() */
	if (bstart != au_ibstart(inode))
		wr_dir_args.force_btgt = au_ibstart(inode);
	err = au_wr_dir(dentry, /*src_dentry*/NULL, &wr_dir_args);
	if (unlikely(err < 0))
		goto out_dentry;
	a->btgt = err;
	a->did_cpup = (err != bstart);
	err = 0;

	/* crazy udba locks */
	a->pin_flags = AuPin_MNT_WRITE;
	a->hinotify = !!au_opt_test(au_mntflags(sb), UDBA_INOTIFY);
	if (a->hinotify)
		au_fset_pin(a->pin_flags, DO_GPARENT);
	parent = NULL;
	if (!IS_ROOT(dentry)) {
		au_fset_pin(a->pin_flags, DI_LOCKED);
		parent = dget_parent(dentry);
		di_write_lock_parent(parent);
	}
	err = au_pin(&a->pin, dentry, a->btgt, a->pin_flags);
	if (unlikely(err)) {
		if (parent) {
			di_write_unlock(parent);
			dput(parent);
		}
		goto out_dentry;
	}
	a->h_dentry = au_h_dptr(dentry, bstart);
	a->h_inode = a->h_dentry->d_inode;
	AuDebugOn(!a->h_inode);
	vfsub_i_lock_nested(a->h_inode, AuLsc_I_CHILD);
	if (!a->did_cpup) {
		au_unpin_gp(&a->pin);
		if (parent) {
			au_pin_set_parent_lflag(&a->pin, /*lflag*/0);
			di_downgrade_lock(parent, AuLock_IR);
			dput(parent);
		}
		goto out; /* success */
	}

	hi_wh = NULL;
	if (!d_unhashed(dentry)) {
		if (parent) {
			au_pin_set_parent_lflag(&a->pin, /*lflag*/0);
			di_downgrade_lock(parent, AuLock_IR);
			dput(parent);
		}
		err = au_sio_cpup_simple(dentry, a->btgt, sz, AuCpup_DTIME);
		if (!err)
			a->h_dentry = au_h_dptr(dentry, a->btgt);
	} else {
		hi_wh = au_hi_wh(inode, a->btgt);
		if (!hi_wh) {
			err = au_sio_cpup_wh(dentry, a->btgt, sz,
					     /*file*/NULL);
			if (!err)
				hi_wh = au_hi_wh(inode, a->btgt);
			/* todo: revalidate hi_wh? */
		}
		if (parent) {
			au_pin_set_parent_lflag(&a->pin, /*lflag*/0);
			di_downgrade_lock(parent, AuLock_IR);
			dput(parent);
		}
		if (!hi_wh)
			a->h_dentry = au_h_dptr(dentry, a->btgt);
		else
			a->h_dentry = hi_wh; /* do not dget here */
	}

	vfsub_i_unlock(a->h_inode);
	a->h_inode = a->h_dentry->d_inode;
	AuDebugOn(!a->h_inode);
	if (!err) {
		vfsub_i_lock_nested(a->h_inode, AuLsc_I_CHILD);
		au_unpin_gp(&a->pin);
		goto out; /* success */
	}

	au_unpin(&a->pin);

 out_dentry:
	di_write_unlock(dentry);
 out:
	AuTraceErr(err);
	return err;
}

static int aufs_setattr(struct dentry *dentry, struct iattr *ia)
{
	int err;
	struct inode *inode;
	struct super_block *sb;
	__u32 events;
	struct file *file;
	loff_t sz;
	struct au_icpup_args *a;

	LKTRTrace("%.*s\n", AuDLNPair(dentry));
	inode = dentry->d_inode;
	IMustLock(inode);

	err = -ENOMEM;
	a = kzalloc(sizeof(*a), GFP_NOFS);
	if (unlikely(!a))
		goto out;

	file = NULL;
	sb = dentry->d_sb;
	si_read_lock(sb, AuLock_FLUSH);
	vfsub_args_init(&a->vargs, a->ign, au_test_dlgt(au_mntflags(sb)), 0);

	if (ia->ia_valid & ATTR_FILE) {
		/* currently ftruncate(2) only */
		file = ia->ia_file;
		fi_write_lock(file);
		ia->ia_file = au_h_fptr(file, au_fbstart(file));
	}

	sz = -1;
	if ((ia->ia_valid & ATTR_SIZE)
	    && ia->ia_size < i_size_read(inode))
		sz = ia->ia_size;
	err = au_lock_and_icpup(dentry, sz, a);
	if (unlikely(err < 0))
		goto out_si;
	if (a->did_cpup) {
		ia->ia_file = NULL;
		ia->ia_valid &= ~ATTR_FILE;
	}

	if ((ia->ia_valid & ATTR_SIZE)
	    && ia->ia_size < i_size_read(inode)) {
		err = vmtruncate(inode, ia->ia_size);
		if (unlikely(err))
			goto out_unlock;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 24)
	if (ia->ia_valid & (ATTR_KILL_SUID | ATTR_KILL_SGID))
		ia->ia_valid &= ~ATTR_MODE;
#endif

	events = 0;
	if (a->hinotify) {
		events = vfsub_events_notify_change(ia);
		if (events) {
			if (a->isdir)
				vfsub_ign_hinode(&a->vargs, events,
						 au_hi(inode, a->btgt));
			vfsub_ign_hinode(&a->vargs, events,
					 au_pinned_hdir(&a->pin));
		}
	}
	err = vfsub_notify_change(a->h_dentry, ia, &a->vargs);
	if (!err)
		au_cpup_attr_changeable(inode);

 out_unlock:
	vfsub_i_unlock(a->h_inode);
	au_unpin(&a->pin);
	di_write_unlock(dentry);
 out_si:
	if (file) {
		fi_write_unlock(file);
		ia->ia_file = file;
		ia->ia_valid |= ATTR_FILE;
	}
	si_read_unlock(sb);
	kfree(a);
 out:
	AuTraceErr(err);
	if (unlikely(err == -EBUSY && au_test_nfsd(current)))
		err = -ESTALE;
	return err;
}

/* ---------------------------------------------------------------------- */

static int h_readlink(struct dentry *dentry, int bindex, char __user *buf,
		      int bufsiz)
{
	struct super_block *sb;
	struct dentry *h_dentry;

	LKTRTrace("%.*s, b%d, %d\n", AuDLNPair(dentry), bindex, bufsiz);

	h_dentry = au_h_dptr(dentry, bindex);
	if (unlikely(/* !h_dentry
			|| !h_dentry->d_inode
			|| */
		    !h_dentry->d_inode->i_op
		    || !h_dentry->d_inode->i_op->readlink))
		return -EINVAL;

	sb = dentry->d_sb;
	if (!au_test_ro(sb, bindex, dentry->d_inode)) {
		touch_atime(au_sbr_mnt(sb, bindex), h_dentry);
		au_update_fuse_h_inode(NULL, h_dentry); /*ignore*/
		dentry->d_inode->i_atime = h_dentry->d_inode->i_atime;
	}
	return h_dentry->d_inode->i_op->readlink(h_dentry, buf, bufsiz);
}

static int aufs_readlink(struct dentry *dentry, char __user *buf, int bufsiz)
{
	int err;

	LKTRTrace("%.*s, %d\n", AuDLNPair(dentry), bufsiz);

	aufs_read_lock(dentry, AuLock_IR);
	err = h_readlink(dentry, au_dbstart(dentry), buf, bufsiz);
	aufs_read_unlock(dentry, AuLock_IR);
	AuTraceErr(err);
	if (unlikely(err == -EBUSY && au_test_nfsd(current)))
		err = -ESTALE;
	return err;
}

static void *aufs_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	int err;
	char *buf;
	mm_segment_t old_fs;

	LKTRTrace("%.*s, nd %.*s\n",
		  AuDLNPair(dentry), AuDLNPair(nd->dentry));

	err = -ENOMEM;
	buf = __getname();
	if (unlikely(!buf))
		goto out;

	aufs_read_lock(dentry, AuLock_IR);
	old_fs = get_fs();
	set_fs(KERNEL_DS);
	err = h_readlink(dentry, au_dbstart(dentry), (char __user *)buf,
			 PATH_MAX);
	set_fs(old_fs);
	aufs_read_unlock(dentry, AuLock_IR);

	if (err >= 0) {
		buf[err] = 0;
		/* will be freed by put_link */
		nd_set_link(nd, buf);
		return NULL; /* success */
	}
	__putname(buf);

 out:
	path_release(nd);
	AuTraceErr(err);
	if (unlikely(err == -EBUSY && au_test_nfsd(current)))
		err = -ESTALE;
	return ERR_PTR(err);
}

static void aufs_put_link(struct dentry *dentry, struct nameidata *nd,
			  void *cookie)
{
	LKTRTrace("%.*s\n", AuDLNPair(dentry));
	__putname(nd_get_link(nd));
}

/* ---------------------------------------------------------------------- */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 22)
static void aufs_truncate_range(struct inode *inode, loff_t start, loff_t end)
{
	AuUnsupport();
}
#endif

/* ---------------------------------------------------------------------- */

struct inode_operations aufs_symlink_iop = {
	.permission	= aufs_permission,
	.setattr	= aufs_setattr,
#ifdef CONFIG_AUFS_GETATTR
	.getattr	= aufs_getattr,
#endif

	.readlink	= aufs_readlink,
	.follow_link	= aufs_follow_link,
	.put_link	= aufs_put_link
};

struct inode_operations aufs_dir_iop = {
	.create		= aufs_create,
	.lookup		= aufs_lookup,
	.link		= aufs_link,
	.unlink		= aufs_unlink,
	.symlink	= aufs_symlink,
	.mkdir		= aufs_mkdir,
	.rmdir		= aufs_rmdir,
	.mknod		= aufs_mknod,
	.rename		= aufs_rename,

	.permission	= aufs_permission,
	.setattr	= aufs_setattr,
#ifdef CONFIG_AUFS_GETATTR
	.getattr	= aufs_getattr,
#endif

#if 0 /* reserved for future use */
	.setxattr	= aufs_setxattr,
	.getxattr	= aufs_getxattr,
	.listxattr	= aufs_listxattr,
	.removexattr	= aufs_removexattr,
#endif

#if 0 //LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 23)
	.fallocate	= aufs_fallocate
#endif
};

struct inode_operations aufs_iop = {
	.permission	= aufs_permission,
	.setattr	= aufs_setattr,
#ifdef CONFIG_AUFS_GETATTR
	.getattr	= aufs_getattr,
#endif

#if 0 /* reserved for future use */
	.setxattr	= aufs_setxattr,
	.getxattr	= aufs_getxattr,
	.listxattr	= aufs_listxattr,
	.removexattr	= aufs_removexattr,
#endif

	//void (*truncate) (struct inode *);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 22)
	.truncate_range	= aufs_truncate_range,
#endif

#if 0 //LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 23)
	.fallocate	= aufs_fallocate
#endif
};
