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
 * whiteout for logical deletion and opaque directory
 *
 * $Id: whout.c,v 1.47 2009/01/26 06:24:19 sfjro Exp $
 */

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/random.h>
#include <linux/security.h>
#include "aufs.h"

#define WH_MASK			S_IRUGO

/* If a directory contains this file, then it is opaque.  We start with the
 * .wh. flag so that it is blocked by lookup.
 */
static struct qstr diropq_name = {
	.name = AUFS_WH_DIROPQ,
	.len = sizeof(AUFS_WH_DIROPQ) - 1
};

/*
 * generate whiteout name, which is NOT terminated by NULL.
 * @name: original d_name.name
 * @len: original d_name.len
 * @wh: whiteout qstr
 * returns zero when succeeds, otherwise error.
 * succeeded value as wh->name should be freed by au_wh_name_free().
 */
int au_wh_name_alloc(const char *name, int len, struct qstr *wh)
{
	char *p;

	AuDebugOn(!name || !len || !wh);

	if (unlikely(len > PATH_MAX - AUFS_WH_PFX_LEN))
		return -ENAMETOOLONG;

	wh->len = len + AUFS_WH_PFX_LEN;
	p = kmalloc(wh->len, GFP_NOFS);
	wh->name = p;
	if (p) {
		memcpy(p, AUFS_WH_PFX, AUFS_WH_PFX_LEN);
		memcpy(p + AUFS_WH_PFX_LEN, name, len);
		/* smp_mb(); */
		return 0;
	}
	return -ENOMEM;
}

void au_wh_name_free(struct qstr *wh)
{
	AuDebugOn(!wh || !wh->name);
	kfree(wh->name);
}

/* ---------------------------------------------------------------------- */

/*
 * test if the @wh_name exists under @h_parent.
 * @try_sio specifies the necessary of super-io.
 */
int au_wh_test(struct dentry *h_parent, struct qstr *wh_name, int try_sio,
	       struct au_ndx *ndx)
{
	int err;
	struct dentry *wh_dentry;
	struct inode *h_dir;
	unsigned int flags;

	LKTRTrace("%.*s/%.*s, ndx{%p, 0x%x}\n", AuDLNPair(h_parent),
		  wh_name->len, wh_name->name, ndx->nfsmnt, ndx->flags);
	h_dir = h_parent->d_inode;
	AuDebugOn(!S_ISDIR(h_dir->i_mode));

	flags = 0;
	if (ndx && ndx->nd) {
		flags = ndx->nd->flags;
		ndx->nd->flags &= ~(LOOKUP_OPEN | LOOKUP_CREATE);
	}

	if (!try_sio)
		wh_dentry = au_lkup_one(wh_name->name, h_parent,
					wh_name->len, ndx);
	else
		wh_dentry = au_sio_lkup_one(wh_name->name, h_parent,
					    wh_name->len, ndx);
	if (flags)
		ndx->nd->flags = flags;
	err = PTR_ERR(wh_dentry);
	if (IS_ERR(wh_dentry))
		goto out;

	err = 0;
	if (!wh_dentry->d_inode)
		goto out_wh; /* success */

	err = 1;
	if (S_ISREG(wh_dentry->d_inode->i_mode))
		goto out_wh; /* success */

	err = -EIO;
	AuIOErr("%.*s Invalid whiteout entry type 0%o.\n",
		AuDLNPair(wh_dentry), wh_dentry->d_inode->i_mode);

 out_wh:
	dput(wh_dentry);
 out:
	AuTraceErr(err);
	return err;
}

/*
 * test if the @h_dentry sets opaque or not.
 */
int au_diropq_test(struct dentry *h_dentry, struct au_ndx *ndx)
{
	int err, try_sio;
	struct inode *h_dir;

	LKTRTrace("dentry %.*s\n", AuDLNPair(h_dentry));
	h_dir = h_dentry->d_inode;
	AuDebugOn(!S_ISDIR(h_dir->i_mode));

	try_sio = au_test_h_perm_sio(h_dir, MAY_EXEC,
				     au_ftest_ndx(ndx->flags, DLGT));
	err = au_wh_test(h_dentry, &diropq_name, try_sio, ndx);
	AuTraceErr(err);
	return err;
}

/*
 * returns a negative dentry whose name is unique and temporary.
 */
struct dentry *au_whtmp_lkup(struct dentry *h_parent, struct qstr *prefix,
			     struct au_ndx *ndx)
{
#define HEX_LEN 4
	struct dentry *dentry;
	int len, i;
	char defname[AUFS_WH_PFX_LEN * 2 + DNAME_INLINE_LEN_MIN + 1
		     + HEX_LEN + 1], *name, *p;
	static unsigned char cnt;

	LKTRTrace("hp %.*s, prefix %.*s\n",
		  AuDLNPair(h_parent), prefix->len, prefix->name);
	AuDebugOn(!h_parent->d_inode);

	name = defname;
	len = sizeof(defname) - DNAME_INLINE_LEN_MIN + prefix->len - 1;
	if (unlikely(prefix->len > DNAME_INLINE_LEN_MIN)) {
		dentry = ERR_PTR(-ENAMETOOLONG);
		if (unlikely(len >= PATH_MAX))
			goto out;
		dentry = ERR_PTR(-ENOMEM);
		name = kmalloc(len + 1, GFP_NOFS);
		if (unlikely(!name))
			goto out;
	}

	/* doubly whiteout-ed */
	memcpy(name, AUFS_WH_PFX AUFS_WH_PFX, AUFS_WH_PFX_LEN * 2);
	p = name + AUFS_WH_PFX_LEN * 2;
	memcpy(p, prefix->name, prefix->len);
	p += prefix->len;
	*p++ = '.';
	AuDebugOn(name + len + 1 - p <= HEX_LEN);

	for (i = 0; i < 3; i++) {
		sprintf(p, "%.*d", HEX_LEN, cnt++);
		dentry = au_sio_lkup_one(name, h_parent, len, ndx);
		if (IS_ERR(dentry) || !dentry->d_inode)
			goto out_name;
		dput(dentry);
	}
	/* AuWarn("could not get random name\n"); */
	dentry = ERR_PTR(-EEXIST);
	AuDbg("%.*s\n", len, name);
	BUG();

 out_name:
	if (name != defname)
		kfree(name);
 out:
	AuTraceErrPtr(dentry);
	return dentry;
#undef HEX_LEN
}

/*
 * rename the @dentry of @bindex to the whiteouted temporary name.
 */
int au_whtmp_ren(struct inode *dir, aufs_bindex_t bindex,
		 struct dentry *h_dentry)
{
	int err, dlgt;
	struct inode *h_dir;
	struct dentry *h_parent, *tmp_dentry;
	struct super_block *sb;
	unsigned int mnt_flags;
	struct au_hin_ignore ign;
	struct vfsub_args vargs;
	struct au_ndx ndx = {
		.flags	= 0,
		.nd	= NULL,
		/* .br	= NULL */
	};

	LKTRTrace("%.*s\n", AuDLNPair(h_dentry));
	AuDebugOn(!h_dentry->d_inode);
	h_parent = h_dentry->d_parent; /* dir inode is locked */
	h_dir = h_parent->d_inode;
	IMustLock(h_dir);

	sb = dir->i_sb;
	mnt_flags = au_mntflags(sb);
	dlgt = !!au_test_dlgt(mnt_flags);
	if (dlgt)
		au_fset_ndx(ndx.flags, DLGT);
	ndx.nfsmnt = au_nfsmnt(sb, bindex);
	tmp_dentry = au_whtmp_lkup(h_parent, &h_dentry->d_name, &ndx);
	err = PTR_ERR(tmp_dentry);
	if (IS_ERR(tmp_dentry))
		goto out;

	/* under the same dir, no need to lock_rename() */
	vfsub_args_init(&vargs, &ign, dlgt, 0);
	AuDebugOn(!S_ISDIR(h_dentry->d_inode->i_mode));
	vfsub_ign_hinode(&vargs, IN_MOVED_FROM | IN_MOVED_TO,
			 au_hi(dir, bindex));
	err = vfsub_rename(h_dir, h_dentry, h_dir, tmp_dentry, &vargs);
	AuTraceErr(err);
	dput(tmp_dentry);

 out:
	AuTraceErr(err);
	return err;
}

/* ---------------------------------------------------------------------- */

static int do_unlink_wh(struct au_hinode *hdir, struct inode *h_dir,
			struct dentry *wh_dentry, const int dlgt)
{
	int err;
	struct au_hin_ignore ign;
	struct vfsub_args vargs;

	AuDebugOn(hdir && h_dir);
	AuDebugOn(!hdir && !h_dir);
	if (!h_dir)
		h_dir = hdir->hi_inode;
	LKTRTrace("hi%lu, wh %.*s\n", h_dir->i_ino, AuDLNPair(wh_dentry));
	AuDebugOn(!wh_dentry->d_inode || !S_ISREG(wh_dentry->d_inode->i_mode));

	/*
	 * forces superio when the dir has a sticky bit.
	 * this may be a violation of unix fs semantics.
	 */
	vfsub_args_init(&vargs, &ign, dlgt,
			(h_dir->i_mode & S_ISVTX)
			&& wh_dentry->d_inode->i_uid != current->fsuid);
	vfsub_ign_hinode(&vargs, IN_DELETE, hdir);
	err = vfsub_unlink(h_dir, wh_dentry, &vargs);
	AuTraceErr(err);
	return err;
}

int au_wh_unlink_dentry(struct au_hinode *hdir, struct dentry *wh_dentry,
			struct dentry *dentry, int dlgt)
{
	int err;

	LKTRTrace("i%lu, wh %.*s, d %p\n",
		  hdir->hi_inode->i_ino, AuDLNPair(wh_dentry), dentry);
	AuDebugOn((dentry && au_dbwh(dentry) < 0)
		  || !wh_dentry->d_inode
		  || !S_ISREG(wh_dentry->d_inode->i_mode));

	err = do_unlink_wh(hdir, /*h_dir*/NULL, wh_dentry, dlgt);
	if (!err && dentry)
		au_set_dbwh(dentry, -1);

	AuTraceErr(err);
	return err;
}

static int unlink_wh_name(struct dentry *h_parent, struct qstr *wh,
			  struct au_ndx *ndx)
{
	int err;
	struct dentry *wh_dentry;

	LKTRTrace("%.*s/%.*s\n", AuDLNPair(h_parent), AuLNPair(wh));

	/* au_test_h_perm() is already done */
	wh_dentry = au_lkup_one(wh->name, h_parent, wh->len, ndx);
	if (IS_ERR(wh_dentry))
		err = PTR_ERR(wh_dentry);
	else {
		err = 0;
		if (wh_dentry->d_inode && S_ISREG(wh_dentry->d_inode->i_mode))
			err = do_unlink_wh(/*hdir*/NULL, h_parent->d_inode,
					   wh_dentry,
					   au_ftest_ndx(ndx->flags, DLGT));
		dput(wh_dentry);
	}

	AuTraceErr(err);
	return err;
}

/* ---------------------------------------------------------------------- */

static void clean_wh(struct inode *h_dir, struct path *whpath,
		     struct au_hinode *hdir, struct vfsub_args *vargs)
{
	int err;

	AuTraceEnter();

	if (!whpath->dentry->d_inode)
		return;

	err = au_mnt_want_write(whpath->mnt);
	if (!err) {
		vfsub_args_reinit(vargs);
		vfsub_ign_hinode(vargs, IN_DELETE, hdir);
		err = vfsub_unlink(h_dir, whpath->dentry, vargs);
		au_mnt_drop_write(whpath->mnt);
	}
	if (unlikely(err))
		AuWarn("failed unlink %.*s (%d), ignored.\n",
		       AuDLNPair(whpath->dentry), err);
}

static void au_whdir_clean(struct inode *h_dir, struct path *whpath,
			   struct au_hinode *hdir, struct vfsub_args *vargs)
{
	int err;

	AuTraceEnter();

	if (!whpath->dentry->d_inode)
		return;

	err = au_mnt_want_write(whpath->mnt);
	if (!err) {
		vfsub_args_reinit(vargs);
		vfsub_ign_hinode(vargs, IN_DELETE, hdir);
		err = vfsub_rmdir(h_dir, whpath->dentry, vargs);
		au_mnt_drop_write(whpath->mnt);
	}
	if (unlikely(err))
		AuWarn("failed rmdir %.*s (%d), ignored.\n",
		       AuDLNPair(whpath->dentry), err);
}

static int test_linkable(struct inode *h_dir)
{
	if (h_dir->i_op && h_dir->i_op->link)
		return 0;
	return -ENOSYS;
}

/* todo: should this mkdir be done in /sbin/mount.aufs script? */
static int au_whdir(struct inode *h_dir, struct path *path,
		    struct au_hinode *hdir, struct vfsub_args *vargs)
{
	int err;

	err = -EEXIST;
	if (!path->dentry->d_inode) {
		int mode = S_IRWXU;
		if (au_test_nfs(path->dentry->d_sb))
			mode |= S_IXUGO;
		err = au_mnt_want_write(path->mnt);
		if (!err) {
			vfsub_args_reinit(vargs);
			vfsub_ign_hinode(vargs, IN_CREATE, hdir);
			err = vfsub_mkdir(h_dir, path->dentry, mode, vargs);
			au_mnt_drop_write(path->mnt);
		}
	} else if (S_ISDIR(path->dentry->d_inode->i_mode))
		err = 0;
	else
		AuErr("unknown %.*s exists\n", AuDLNPair(path->dentry));

	return err;
}

/*
 * initialize the whiteout base file/dir for @br.
 */
int au_wh_init(struct dentry *h_root, struct au_branch *br,
	       struct vfsmount *h_mnt, struct super_block *sb,
	       aufs_bindex_t bindex)
{
	int err, i;
	const unsigned int mnt_flags = au_mntflags(sb);
	const unsigned char do_plink = !!au_opt_test(mnt_flags, PLINK),
		do_hinotify = au_opt_test(mnt_flags, UDBA_INOTIFY);
	struct path path = {
		.mnt = h_mnt
	};
	struct au_hin_ignore ign;
	struct vfsub_args vargs;
	struct inode *h_dir;
	struct au_hinode *hdir;
	struct au_wbr *wbr = br->br_wbr;
	struct vfsmount *nfsmnt = au_do_nfsmnt(h_mnt);
	static const struct qstr base_name[] = {
		[AuBrWh_BASE] = {
			.name	= AUFS_BASE_NAME,
			.len	= sizeof(AUFS_BASE_NAME) - 1
		},
		[AuBrWh_PLINK] = {
			.name	= AUFS_PLINKDIR_NAME,
			.len	= sizeof(AUFS_PLINKDIR_NAME) - 1
		},
		[AuBrWh_TMP] = {
			.name	= AUFS_TMPDIR_NAME,
			.len	= sizeof(AUFS_TMPDIR_NAME) - 1
		}
	};
	struct {
		const struct qstr *name;
		struct dentry *dentry;
	} base[] = {
		[AuBrWh_BASE] = {
			.name	= base_name + AuBrWh_BASE,
			.dentry	= NULL
		},
		[AuBrWh_PLINK] = {
			.name	= base_name + AuBrWh_PLINK,
			.dentry	= NULL
		},
		[AuBrWh_TMP] = {
			.name	= base_name + AuBrWh_TMP,
			.dentry	= NULL
		}
	};
	struct au_ndx ndx = {
		.nfsmnt	= nfsmnt,
		.flags	= 0, /* always no dlgt */
		.nd	= NULL,
		/* .br	= NULL */
	};

	LKTRTrace("nfsmnt %p\n", nfsmnt);
	WbrWhMustWriteLock(wbr);
	SiMustWriteLock(sb);
	h_dir = h_root->d_inode;

	for (i = 0; i < AuBrWh_Last; i++) {
		/* doubly whiteouted */
		struct dentry *d;
		d = au_wh_lkup(h_root, (void *)base[i].name, &ndx);
		err = PTR_ERR(d);
		if (IS_ERR(d))
			goto out;
		base[i].dentry = d;
		if (!au_test_ecryptfs(d->d_sb))
			AuDebugOn(wbr
				  && wbr->wbr_wh[i]
				  && wbr->wbr_wh[i] != base[i].dentry);
		else
			/* ecryptfs problem?: it returns different dentry */
			AuDebugOn(wbr
				  && wbr->wbr_wh[i]
				  && !!wbr->wbr_wh[i] != !!base[i].dentry);
	}

	if (wbr)
		for (i = 0; i < AuBrWh_Last; i++) {
			dput(wbr->wbr_wh[i]);
			wbr->wbr_wh[i] = NULL;
		}

	err = 0;
	hdir = NULL;
	if (bindex >= 0 && do_hinotify)
		hdir = au_hi(sb->s_root->d_inode, bindex);
	vfsub_args_init(&vargs, &ign, au_test_dlgt(mnt_flags), 0);

	switch (br->br_perm) {
	case AuBrPerm_RR:
	case AuBrPerm_RO:
	case AuBrPerm_RRWH:
	case AuBrPerm_ROWH:
		path.dentry = base[AuBrWh_BASE].dentry;
		clean_wh(h_dir, &path, hdir, &vargs);
		path.dentry = base[AuBrWh_PLINK].dentry;
		au_whdir_clean(h_dir, &path, hdir, &vargs);
		path.dentry = base[AuBrWh_TMP].dentry;
		au_whdir_clean(h_dir, &path, hdir, &vargs);
		break;

	case AuBrPerm_RWNoLinkWH:
		path.dentry = base[AuBrWh_BASE].dentry;
		clean_wh(h_dir, &path, hdir, &vargs);
		path.dentry = base[AuBrWh_PLINK].dentry;
		if (do_plink) {
			err = test_linkable(h_dir);
			if (unlikely(err))
				goto out_nolink;

			err = au_whdir(h_dir, &path, hdir, &vargs);
			if (unlikely(err))
				goto out_err;
			wbr->wbr_plink = dget(base[AuBrWh_PLINK].dentry);
		} else
			au_whdir_clean(h_dir, &path, hdir, &vargs);
		path.dentry = base[AuBrWh_TMP].dentry;
		err = au_whdir(h_dir, &path, hdir, &vargs);
		if (unlikely(err))
			goto out_err;
		wbr->wbr_tmp = dget(base[AuBrWh_TMP].dentry);
		break;

	case AuBrPerm_RW:
		/*
		 * for the moment, aufs supports the branch filesystem
		 * which does not support link(2).
		 * testing on FAT which does not support i_op->setattr() fully
		 * either, copyup failed.
		 * finally, such filesystem will not be used as the writable
		 * branch.
		 */
		err = test_linkable(h_dir);
		if (unlikely(err))
			goto out_nolink;

		err = -EEXIST;
		/*
		 * todo: should this create be done
		 * in /sbin/mount.aufs script?
		 */
		if (!base[AuBrWh_BASE].dentry->d_inode) {
			err = au_mnt_want_write(h_mnt);
			if (!err) {
				vfsub_args_reinit(&vargs);
				vfsub_ign_hinode(&vargs, IN_CREATE, hdir);
				err = au_h_create
					(h_dir, base[AuBrWh_BASE].dentry,
					 WH_MASK, &vargs, /*nd*/NULL, nfsmnt);
				au_mnt_drop_write(h_mnt);
			}
		} else if (S_ISREG(base[AuBrWh_BASE].dentry->d_inode->i_mode))
			err = 0;
		else
			AuErr("unknown %.*s/%.*s exists\n",
			      AuDLNPair(h_root),
			      AuDLNPair(base[AuBrWh_BASE].dentry));
		if (unlikely(err))
			goto out_err;

		path.dentry = base[AuBrWh_PLINK].dentry;
		if (do_plink) {
			err = au_whdir(h_dir, &path, hdir, &vargs);
			if (unlikely(err))
				goto out_err;
			wbr->wbr_plink = dget(base[AuBrWh_PLINK].dentry);
		} else
			au_whdir_clean(h_dir, &path, hdir, &vargs);
		wbr->wbr_whbase = dget(base[AuBrWh_BASE].dentry);

		path.dentry = base[AuBrWh_TMP].dentry;
		err = au_whdir(h_dir, &path, hdir, &vargs);
		if (unlikely(err))
			goto out_err;
		wbr->wbr_tmp = dget(base[AuBrWh_TMP].dentry);
		break;

	default:
		BUG();
	}

 out:
	for (i = 0; i < AuBrWh_Last; i++)
		dput(base[i].dentry);
	AuTraceErr(err);
	return err;
 out_nolink:
	AuErr("%.*s doesn't support link(2), use noplink and rw+nolwh\n",
	      AuDLNPair(h_root));
	goto out;
 out_err:
	AuErr("an error(%d) on the writable branch %.*s(%s)\n",
	      err, AuDLNPair(h_root), au_sbtype(h_root->d_sb));
	goto out;
}

struct reinit_br_wh {
	struct super_block *sb;
	struct au_branch *br;
};

static void reinit_br_wh(void *arg)
{
	int err;
	struct reinit_br_wh *a = arg;
	struct au_wbr *wbr;
	struct inode *h_dir, *dir;
	struct dentry *h_root;
	aufs_bindex_t bindex;
	struct au_hin_ignore ign;
	struct vfsub_args vargs;

	AuTraceEnter();
	AuDebugOn(current->fsuid);

	err = 0;
	wbr = a->br->br_wbr;
	/* big aufs lock */
	si_noflush_write_lock(a->sb);
	if (!au_br_writable(a->br->br_perm))
		goto out;
	bindex = au_br_index(a->sb, a->br->br_id);
	if (unlikely(bindex < 0))
		goto out;

	AuDebugOn(!wbr);
	AuDebugOn(!wbr->wbr_whbase || !wbr->wbr_whbase->d_inode);

	dir = a->sb->s_root->d_inode;
	ii_read_lock_parent(dir);
	h_root = dget_parent(wbr->wbr_whbase);
	h_dir = h_root->d_inode;
	AuDebugOn(!h_dir->i_op || !h_dir->i_op->link);
	vfsub_i_lock_nested(h_dir, AuLsc_I_PARENT);
	wbr_wh_write_lock(wbr);
	if (!au_verify_parent(wbr->wbr_whbase, h_dir)) {
		err = au_br_want_write(a->br);
		if (!err) {
			vfsub_args_init(&vargs, &ign, /*dlgt*/0, 0);
			vfsub_ign_hinode(&vargs, IN_DELETE, au_hi(dir, bindex));
			err = vfsub_unlink(h_dir, wbr->wbr_whbase, &vargs);
			au_br_drop_write(a->br);
		}
	} else {
		AuWarn("%.*s is moved, ignored\n", AuDLNPair(wbr->wbr_whbase));
		err = 0;
	}
	dput(wbr->wbr_whbase);
	wbr->wbr_whbase = NULL;
	if (!err)
		err = au_wh_init(h_root, a->br, a->br->br_mnt, a->sb, bindex);
	wbr_wh_write_unlock(wbr);
	vfsub_i_unlock(h_dir);
	dput(h_root);
	ii_read_unlock(dir);

 out:
	if (wbr)
		atomic_dec_return(&wbr->wbr_wh_running);
	au_br_put(a->br);
	au_nwt_done(&au_sbi(a->sb)->si_nowait);
	si_write_unlock(a->sb);
	kfree(arg);
	if (unlikely(err))
		AuIOErr("err %d\n", err);
}

static void kick_reinit_br_wh(struct super_block *sb, struct au_branch *br)
{
	int do_dec, wkq_err;
	struct reinit_br_wh *arg;

	AuTraceEnter();
	AuDebugOn(!br->br_wbr);

	do_dec = 1;
	if (atomic_inc_return(&br->br_wbr->wbr_wh_running) != 1)
		goto out;

	/* ignore ENOMEM */
	arg = kmalloc(sizeof(*arg), GFP_NOFS);
	if (arg) {
		/*
		 * dec(wh_running), kfree(arg) and au_br_put()
		 * in reinit function
		 */
		arg->sb = sb;
		arg->br = br;
		au_br_get(br);
		wkq_err = au_wkq_nowait(reinit_br_wh, arg, sb, /*dlgt*/0);
		if (unlikely(wkq_err)) {
			atomic_dec_return(&br->br_wbr->wbr_wh_running);
			au_br_put(br);
			kfree(arg);
		}
		do_dec = 0;
	}

 out:
	if (do_dec)
		atomic_dec_return(&br->br_wbr->wbr_wh_running);
}

/*
 * create the whiteout @wh.
 */
static int link_or_create_wh(struct super_block *sb, aufs_bindex_t bindex,
			     struct dentry *wh, struct inode *dir)
{
	int err, dlgt;
	struct au_branch *br;
	struct au_wbr *wbr;
	struct dentry *h_parent;
	struct inode *h_dir;
	struct au_hin_ignore ign;
	struct vfsub_args vargs;

	LKTRTrace("%.*s\n", AuDLNPair(wh));
	h_parent = wh->d_parent; /* dir inode is locked */
	h_dir = h_parent->d_inode;
	IMustLock(h_dir);
	br = au_sbr(sb, bindex);
	wbr = br->br_wbr;
	AuDebugOn(!wbr);

	dlgt = !!au_test_dlgt(au_mntflags(sb));
	wbr_wh_read_lock(wbr);
	if (wbr->wbr_whbase) {
		vfsub_args_init(&vargs, &ign, dlgt, 0);
		if (dir)
			vfsub_ign_hinode(&vargs, IN_CREATE, au_hi(dir, bindex));
		err = vfsub_link(wbr->wbr_whbase, h_dir, wh, &vargs);
		if (!err || err != -EMLINK)
			goto out;

		/* link count full. re-initialize br_whbase. */
		kick_reinit_br_wh(sb, br);
	}

	/* return this error in this context */
	vfsub_args_init(&vargs, &ign, dlgt, 0);
	if (dir)
		vfsub_ign_hinode(&vargs, IN_CREATE, au_hi(dir, bindex));
	err = au_h_create(h_dir, wh, WH_MASK, &vargs, /*nd*/NULL,
			  au_do_nfsmnt(br->br_mnt));

 out:
	wbr_wh_read_unlock(wbr);
	AuTraceErr(err);
	return err;
}

/* ---------------------------------------------------------------------- */

/*
 * create or remove the diropq.
 */
static struct dentry *do_diropq(struct dentry *dentry, aufs_bindex_t bindex,
				unsigned int flags)
{
	struct dentry *opq_dentry, *h_dentry;
	struct inode *h_dir;
	int err, dlgt;
	struct super_block *sb;
	struct au_ndx ndx = {
		.flags	= 0,
		.nd	= NULL,
		/* .br	= NULL */
	};

	LKTRTrace("%.*s, bindex %d, flags 0x%x\n",
		  AuDLNPair(dentry), bindex, flags);
	h_dentry = au_h_dptr(dentry, bindex);
	AuDebugOn(!h_dentry);
	h_dir = h_dentry->d_inode;
	AuDebugOn(!h_dir || !S_ISDIR(h_dir->i_mode));

	/* already checked by au_test_h_perm(). */
	sb = dentry->d_sb;
	ndx.nfsmnt = au_nfsmnt(sb, bindex);
	dlgt = 0;
	if (au_ftest_diropq(flags, DLGT)) {
		dlgt = 1;
		au_fset_ndx(ndx.flags, DLGT);
	}
	opq_dentry = au_lkup_one(diropq_name.name, h_dentry, diropq_name.len,
				 &ndx);
	if (IS_ERR(opq_dentry))
		goto out;

	if (au_ftest_diropq(flags, CREATE)) {
		AuDebugOn(opq_dentry->d_inode);
		err = link_or_create_wh(dentry->d_sb, bindex, opq_dentry,
					dentry->d_inode);
		if (!err) {
			au_set_dbdiropq(dentry, bindex);
			goto out; /* success */
		}
	} else {
		AuDebugOn(/* !S_ISDIR(dentry->d_inode->i_mode)
			   * ||  */!opq_dentry->d_inode);
		err = do_unlink_wh(au_hi(dentry->d_inode, bindex),
				   /*h_dir*/NULL, opq_dentry, dlgt);
		if (!err)
			au_set_dbdiropq(dentry, -1);
	}
	dput(opq_dentry);
	opq_dentry = ERR_PTR(err);

 out:
	AuTraceErrPtr(opq_dentry);
	return opq_dentry;
}

struct do_diropq_args {
	struct dentry **errp;
	struct dentry *dentry;
	aufs_bindex_t bindex;
	unsigned int flags;
};

static void call_do_diropq(void *args)
{
	struct do_diropq_args *a = args;
	*a->errp = do_diropq(a->dentry, a->bindex, a->flags);
}

struct dentry *au_diropq_sio(struct dentry *dentry, aufs_bindex_t bindex,
			     unsigned int flags)
{
	struct dentry *diropq, *h_dentry;

	LKTRTrace("%.*s, bindex %d, flags 0x%x\n",
		  AuDLNPair(dentry), bindex, flags);

	h_dentry = au_h_dptr(dentry, bindex);
	if (!au_test_h_perm_sio(h_dentry->d_inode, MAY_EXEC | MAY_WRITE,
				au_ftest_diropq(flags, DLGT)))
		diropq = do_diropq(dentry, bindex, flags);
	else {
		int wkq_err;
		struct do_diropq_args args = {
			.errp		= &diropq,
			.dentry		= dentry,
			.bindex		= bindex,
			.flags		= flags
		};
		wkq_err = au_wkq_wait(call_do_diropq, &args, /*dlgt*/0);
		if (unlikely(wkq_err))
			diropq = ERR_PTR(wkq_err);
	}

	AuTraceErrPtr(diropq);
	return diropq;
}

/* ---------------------------------------------------------------------- */

/*
 * lookup whiteout dentry.
 * @h_parent: hidden parent dentry which must exist and be locked
 * @base_name: name of dentry which will be whiteouted
 * returns dentry for whiteout.
 */
struct dentry *au_wh_lkup(struct dentry *h_parent, struct qstr *base_name,
			  struct au_ndx *ndx)
{
	int err;
	struct qstr wh_name;
	struct dentry *wh_dentry;

	LKTRTrace("%.*s/%.*s\n", AuDLNPair(h_parent), AuLNPair(base_name));

	err = au_wh_name_alloc(base_name->name, base_name->len, &wh_name);
	wh_dentry = ERR_PTR(err);
	if (!err) {
		/* do not superio. */
		wh_dentry = au_lkup_one(wh_name.name, h_parent,
					wh_name.len, ndx);
		au_wh_name_free(&wh_name);
	}
	AuTraceErrPtr(wh_dentry);
	return wh_dentry;
}

/*
 * link/create a whiteout for @dentry on @bindex.
 */
struct dentry *au_wh_create(struct dentry *dentry, aufs_bindex_t bindex,
			    struct dentry *h_parent, struct au_ndx *ndx)
{
	struct dentry *wh_dentry;
	struct inode *dir;
	int err;
	struct super_block *sb;

	LKTRTrace("%.*s/%.*s on b%d\n", AuDLNPair(h_parent),
		  AuDLNPair(dentry), bindex);

	sb = dentry->d_sb;
	wh_dentry = au_wh_lkup(h_parent, &dentry->d_name, ndx);
	if (!IS_ERR(wh_dentry) && !wh_dentry->d_inode) {
		dir = dentry->d_parent->d_inode; /* dir is locked */
		IMustLock(dir);
		err = link_or_create_wh(sb, bindex, wh_dentry, dir);
		if (!err)
			au_set_dbwh(dentry, bindex);
		else {
			dput(wh_dentry);
			wh_dentry = ERR_PTR(err);
		}
	}

	AuTraceErrPtr(wh_dentry);
	return wh_dentry;
}

/* ---------------------------------------------------------------------- */

/* Delete all whiteouts in this directory on branch bindex. */
static int del_wh_children(struct dentry *h_dentry, struct au_nhash *whlist,
			   aufs_bindex_t bindex, struct au_ndx *ndx)
{
	int err, i;
	struct qstr wh_name;
	char *p;
	struct inode *h_inode;
	struct hlist_head *head;
	struct au_vdir_wh *tpos;
	struct hlist_node *pos;
	struct au_vdir_destr *str;

	LKTRTrace("%.*s\n", AuDLNPair(h_dentry));
	h_inode = h_dentry->d_inode;
	AuDebugOn(IS_RDONLY(h_inode));

	err = -ENOMEM;
	p = __getname();
	wh_name.name = p;
	if (unlikely(!wh_name.name))
		goto out;
	memcpy(p, AUFS_WH_PFX, AUFS_WH_PFX_LEN);
	p += AUFS_WH_PFX_LEN;

	/* already checked by au_test_h_perm(). */
	err = 0;
	for (i = 0; !err && i < AuSize_NHASH; i++) {
		head = whlist->heads + i;
		hlist_for_each_entry(tpos, pos, head, wh_hash) {
			if (tpos->wh_bindex != bindex)
				continue;
			str = &tpos->wh_str;
			if (str->len + AUFS_WH_PFX_LEN <= PATH_MAX) {
				memcpy(p, str->name, str->len);
				wh_name.len = AUFS_WH_PFX_LEN + str->len;
				err = unlink_wh_name(h_dentry, &wh_name, ndx);
				if (!err)
					continue;
				break;
			}
			AuIOErr("whiteout name too long %.*s\n",
				str->len, str->name);
			err = -EIO;
			break;
		}
	}
	__putname(wh_name.name);

 out:
	AuTraceErr(err);
	return err;
}

struct del_wh_children_args {
	int *errp;
	struct dentry *h_dentry;
	struct au_nhash *whlist;
	aufs_bindex_t bindex;
	struct au_ndx *ndx;
};

static void call_del_wh_children(void *args)
{
	struct del_wh_children_args *a = args;
	*a->errp = del_wh_children(a->h_dentry, a->whlist, a->bindex, a->ndx);
}

/* ---------------------------------------------------------------------- */

/*
 * rmdir the whiteouted temporary named dir @h_dentry.
 * @whlist: whiteouted children.
 */
int au_whtmp_rmdir(struct inode *dir, aufs_bindex_t bindex,
		   struct dentry *wh_dentry, struct au_nhash *whlist)
{
	int err, dlgt;
	struct inode *wh_inode, *h_dir;
	struct super_block *sb;
	unsigned int mnt_flags;
	struct au_hin_ignore ign;
	struct vfsub_args vargs;
	struct au_ndx ndx = {
		.flags	= 0,
		.nd	= NULL,
		/* .br	= NULL */
	};

	LKTRTrace("i%lu, %.*s, b%d\n",
		  dir->i_ino, AuDLNPair(wh_dentry), bindex);
	/* IMustLock(dir); */
	IiMustAnyLock(dir);
	h_dir = wh_dentry->d_parent->d_inode; /* dir inode is locked */
	IMustLock(h_dir);

	sb = dir->i_sb;
	mnt_flags = au_mntflags(sb);
	dlgt = !!au_test_dlgt(mnt_flags);
	if (dlgt)
		au_fset_ndx(ndx.flags, DLGT);
	ndx.nfsmnt = au_nfsmnt(sb, bindex);
	wh_inode = wh_dentry->d_inode;
	vfsub_i_lock_nested(wh_inode, AuLsc_I_CHILD);

	/*
	 * someone else might change some whiteouts while we were sleeping.
	 * it means this whlist may have an obsoleted entry.
	 */
	if (!au_test_h_perm_sio(wh_inode, MAY_EXEC | MAY_WRITE, dlgt))
		err = del_wh_children(wh_dentry, whlist, bindex, &ndx);
	else {
		int wkq_err;
		/* ugly */
		unsigned int flags = ndx.flags;
		struct del_wh_children_args args = {
			.errp		= &err,
			.h_dentry	= wh_dentry,
			.whlist		= whlist,
			.bindex		= bindex,
			.ndx		= &ndx
		};

		ndx.flags = 0;
		wkq_err = au_wkq_wait(call_del_wh_children, &args, /*dlgt*/0);
		if (unlikely(wkq_err))
			err = wkq_err;
		ndx.flags = flags;
	}
	vfsub_i_unlock(wh_inode);

	if (!err) {
		vfsub_args_init(&vargs, &ign, dlgt, 0);
		vfsub_ign_hinode(&vargs, IN_DELETE, au_hi(dir, bindex));
		err = vfsub_rmdir(h_dir, wh_dentry, &vargs);
		/* d_drop(h_dentry); */
	}

	if (!err) {
		if (au_ibstart(dir) == bindex) {
			au_cpup_attr_timesizes(dir);
			/* au_cpup_attr_nlink(dir); */
			dir->i_nlink--;
		}
		return 0; /* success */
	}

	AuWarn("failed removing %.*s(%d), ignored\n",
	       AuDLNPair(wh_dentry), err);
	return err;
}

static void au_whtmp_rmdir_free_args(struct au_whtmp_rmdir_args *args)
{
	au_nhash_fin(&args->whlist);
	dput(args->wh_dentry);
	iput(args->dir);
	kfree(args);
}

static void call_rmdir_whtmp(void *args)
{
	int err;
	struct au_whtmp_rmdir_args *a = args;
	struct super_block *sb;
	struct dentry *h_parent;
	struct inode *h_dir;

	LKTRTrace("%.*s, b%d, dir i%lu\n",
		  AuDLNPair(a->wh_dentry), a->bindex, a->dir->i_ino);

	/* rmdir by nfsd may cause deadlock with this i_mutex */
	/* vfsub_i_lock(a->dir); */
	sb = a->dir->i_sb;
	si_noflush_read_lock(sb);
	err = au_test_ro(sb, a->bindex, NULL);
	if (unlikely(err))
		goto out;

	err = -EIO;
	ii_write_lock_parent(a->dir);
	h_parent = dget_parent(a->wh_dentry);
	h_dir = h_parent->d_inode;
 	vfsub_i_lock_nested(h_dir, AuLsc_I_PARENT);
	if (!au_verify_parent(a->wh_dentry, h_dir)) {
		err = au_br_want_write(au_sbr(sb, a->bindex));
		if (!err) {
			err = au_whtmp_rmdir(a->dir, a->bindex, a->wh_dentry,
					     &a->whlist);
			au_br_drop_write(au_sbr(sb, a->bindex));
		}
	}
 	vfsub_i_unlock(h_dir);
	dput(h_parent);
	ii_write_unlock(a->dir);

 out:
 	/* vfsub_i_unlock(a->dir); */
	au_nwt_done(&au_sbi(sb)->si_nowait);
	si_read_unlock(sb);
	au_whtmp_rmdir_free_args(a);
	if (unlikely(err))
		AuIOErr("err %d\n", err);
}

void au_whtmp_kick_rmdir(struct inode *dir, aufs_bindex_t bindex,
			 struct dentry *wh_dentry, struct au_nhash *whlist,
			 struct au_whtmp_rmdir_args *args)
{
	int wkq_err;

	LKTRTrace("%.*s\n", AuDLNPair(wh_dentry));
	IMustLock(dir);

	/* all post-process will be done in do_rmdir_whtmp(). */
	args->dir = au_igrab(dir);
	args->bindex = bindex;
	args->wh_dentry = dget(wh_dentry);
	au_nhash_init(&args->whlist);
	au_nhash_move(&args->whlist, whlist);
	wkq_err = au_wkq_nowait(call_rmdir_whtmp, args, dir->i_sb, /*dlgt*/0);
	if (unlikely(wkq_err)) {
		AuWarn("rmdir error %.*s (%d), ignored\n",
		       AuDLNPair(wh_dentry), wkq_err);
		au_whtmp_rmdir_free_args(args);
	}
}
