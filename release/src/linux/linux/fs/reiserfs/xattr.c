/*
 * linux/fs/reiserfs/xattr.c
 *
 * Copyright (c) 2002 by Jeff Mahoney, <jeffm@suse.com>
 *
 */

/*
 * In order to implement EA/ACLs in a clean, backwards compatible manner,
 * they are implemented as files in a "private" directory.
 * Each EA is in it's own file, with the directory layout like so (/ is assumed
 * to be relative to fs root). Inside the /.reiserfs_priv/xattrs directory,
 * directories named using the capital-hex form of the objectid and
 * generation number are used. Inside each directory are individual files
 * named with the name of the extended attribute.
 *
 * So, for objectid 12648430, we could have:
 * /.reiserfs_priv/xattrs/C0FFEE.0/system.posix_acl_access
 * /.reiserfs_priv/xattrs/C0FFEE.0/system.posix_acl_default
 * /.reiserfs_priv/xattrs/C0FFEE.0/user.Content-Type
 * .. or similar.
 *
 * The file contents are the text of the EA. The size is known based on the
 * stat data describing the file.
 *
 * In the case of system.posix_acl_access and system.posix_acl_default, since
 * these are special cases for filesystem ACLs, they are interpreted by the
 * kernel, in addition, they are negatively and positively cached and attached
 * to the inode so that unnecessary lookups are avoided.
 */

#include <linux/reiserfs_fs.h>
#include <linux/dcache.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/pagemap.h>
#include <linux/xattr.h>
#include <linux/reiserfs_xattr.h>
#include <linux/mbcache.h>
#include <asm/uaccess.h>
#include <asm/checksum.h>
#include <linux/smp_lock.h>
#include <linux/stat.h>
#include <asm/semaphore.h>

#define FL_READONLY 128
#define FL_DIR_SEM_HELD 256
#define PRIVROOT_NAME ".reiserfs_priv"
#define XAROOT_NAME   "xattrs"

static struct reiserfs_xattr_handler *find_xattr_handler_prefix (const char *prefix);

static struct dentry *
create_xa_root (struct super_block *sb)
{
    struct dentry *privroot = dget (sb->u.reiserfs_sb.priv_root);
    struct dentry *xaroot;

    /* This needs to be created at mount-time */
    if (!privroot)
        return ERR_PTR(-EOPNOTSUPP);

    xaroot = lookup_one_len (XAROOT_NAME, privroot, strlen (XAROOT_NAME));
    if (IS_ERR (xaroot)) {
        goto out;
    } else if (!xaroot->d_inode) {
        int err;
        down (&privroot->d_inode->i_sem);
        err = privroot->d_inode->i_op->mkdir (privroot->d_inode, xaroot, 0700);
        up (&privroot->d_inode->i_sem);

        if (err) {
            dput (xaroot);
            dput (privroot);
            return ERR_PTR (err);
        }
        sb->u.reiserfs_sb.xattr_root = dget (xaroot);
    }

out:
    dput (privroot);
    return xaroot;
}

/* This will return a dentry, or error, refering to the xa root directory.
 * If the xa root doesn't exist yet, the dentry will be returned without
 * an associated inode. This dentry can be used with ->mkdir to create
 * the xa directory. */
static struct dentry *
__get_xa_root (struct super_block *s)
{
    struct dentry *privroot = dget (s->u.reiserfs_sb.priv_root);
    struct dentry *xaroot = NULL;

    if (IS_ERR (privroot) || !privroot)
        return privroot;

    xaroot = lookup_one_len (XAROOT_NAME, privroot, strlen (XAROOT_NAME));
    if (IS_ERR (xaroot)) {
        goto out;
    } else if (!xaroot->d_inode) {
        dput (xaroot);
        xaroot = NULL;
        goto out;
    }

    s->u.reiserfs_sb.xattr_root = dget (xaroot);

out:
    dput (privroot);
    return xaroot;
}

/* Returns the dentry (or NULL) referring to the root of the extended
 * attribute directory tree. If it has already been retreived, it is used.
 * Otherwise, we attempt to retreive it from disk. It may also return
 * a pointer-encoded error.
 */
static inline struct dentry *
get_xa_root (struct super_block *s)
{
    struct dentry *dentry = s->u.reiserfs_sb.xattr_root;

    if (!dentry)
        dentry = __get_xa_root (s);
    else
        dget (dentry);
    return dentry;
}

/* Same as above, but only returns a valid dentry or NULL */
struct dentry *
reiserfs_get_xa_root (struct super_block *sb)
{
    struct dentry *dentry;

    dentry = get_xa_root (sb);
    if (IS_ERR (dentry)) {
        dentry = NULL;
    } else if (dentry && !dentry->d_inode) {
        dput (dentry);
        dentry = NULL;
    }

    return dentry;
}

/* Opens the directory corresponding to the inode's extended attribute store.
 * If flags allow, the tree to the directory may be created. If creation is
 * prohibited, -ENODATA is returned. */
static struct dentry *
open_xa_dir (const struct inode *inode, int flags)
{
    struct dentry *xaroot, *xadir;
    char namebuf[17];

    xaroot = get_xa_root (inode->i_sb);
    if (IS_ERR (xaroot)) {
        return xaroot;
    } else if (!xaroot) {
        if (flags == 0 || flags & XATTR_CREATE) {
            xaroot = create_xa_root (inode->i_sb);
            if (IS_ERR (xaroot))
                return xaroot;
        }
        if (!xaroot)
            return ERR_PTR (-ENODATA);
    }

    /* ok, we have xaroot open */

    snprintf (namebuf, sizeof (namebuf), "%X.%X",
              le32_to_cpu (INODE_PKEY (inode)->k_objectid),
              inode->i_generation);
    xadir = lookup_one_len (namebuf, xaroot, strlen (namebuf));
    if (IS_ERR (xadir)) {
        dput (xaroot);
        return xadir;
    }
    
    if (!xadir->d_inode) {
        int err;
        if (flags == 0 || flags & XATTR_CREATE) {
            /* Although there is nothing else trying to create this directory,
             * another directory with the same hash may be created, so we need
             * to protect against that */
            err = xaroot->d_inode->i_op->mkdir (xaroot->d_inode, xadir, 0700);
            if (err) {
                dput (xaroot);
                dput (xadir);
                return ERR_PTR (err);
            }
        }
        if (!xadir->d_inode) {
            dput (xaroot);
            dput (xadir);
            return ERR_PTR (-ENODATA);
        }
    }

    dput (xaroot);
    return xadir;
}

/* Returns a dentry corresponding to a specific extended attribute file
 * for the inode. If flags allow, the file is created. Otherwise, a
 * valid or negative dentry, or an error is returned. */
static struct dentry *
get_xa_file_dentry (const struct inode *inode, const char *name, int flags)
{
    struct dentry *xadir, *xafile;
    int err = 0;

    xadir = open_xa_dir (inode, flags);
    if (IS_ERR (xadir)) {
        return ERR_PTR (PTR_ERR (xadir));
    } else if (xadir && !xadir->d_inode) {
        dput (xadir);
        return ERR_PTR (-ENODATA);
    }

    xafile = lookup_one_len (name, xadir, strlen (name));
    if (IS_ERR (xafile)) {
        dput (xadir);
        return ERR_PTR (PTR_ERR (xafile));
    }

    if (xafile->d_inode) { /* file exists */
        if (flags & XATTR_CREATE) {
            err = -EEXIST;
            dput (xafile);
            goto out;
        }
    } else if (flags & XATTR_REPLACE || flags & FL_READONLY) {
        goto out;
    } else {
        /* inode->i_sem is down, so nothing else can try to create
         * the same xattr */
        err = xadir->d_inode->i_op->create (xadir->d_inode, xafile,
                                            0700|S_IFREG);

        if (err) {
            dput (xafile);
            goto out;
        }
    }

out:
    dput (xadir);
    if (err)
        xafile = ERR_PTR (err);
    return xafile;
}


/* Opens a file pointer to the attribute associated with inode */
static struct file *
open_xa_file (const struct inode *inode, const char *name, int flags)
{
    struct dentry *xafile;
    struct file *fp;

    xafile = get_xa_file_dentry (inode, name, flags);
    if (IS_ERR (xafile))
        return ERR_PTR (PTR_ERR (xafile));
    else if (!xafile->d_inode) {
        dput (xafile);
        return ERR_PTR (-ENODATA);
    }

    fp = dentry_open (xafile, NULL, O_RDWR);
    /* dentry_open dputs the dentry if it fails */

    return fp;
}


/*
 * this is very similar to fs/reiserfs/dir.c:reiserfs_readdir, but
 * we need to drop the path before calling the filldir struct.  That
 * would be a big performance hit to the non-xattr case, so I've copied
 * the whole thing for now. --clm
 *
 * the big difference is that I go backwards through the directory, 
 * and don't mess with f->f_pos, but the idea is the same.  Do some
 * action on each and every entry in the directory.
 *
 * we're called with i_sem held, so there are no worries about the directory
 * changing underneath us.
 */
static int __xattr_readdir(struct file * filp, void * dirent, filldir_t filldir)
{
    struct inode *inode = filp->f_dentry->d_inode;
    struct cpu_key pos_key;	/* key of current position in the directory (key of directory entry) */
    INITIALIZE_PATH (path_to_entry);
    struct buffer_head * bh;
    int entry_num;
    struct item_head * ih, tmp_ih;
    int search_res;
    char * local_buf;
    loff_t next_pos;
    char small_buf[32] ; /* avoid kmalloc if we can */
    struct reiserfs_de_head *deh;
    int d_reclen;
    char * d_name;
    off_t d_off;
    ino_t d_ino;
    struct reiserfs_dir_entry de;


    /* form key for search the next directory entry using f_pos field of
       file structure */
    next_pos = max_reiserfs_offset(inode);

    while (1) {
research:
	if (next_pos <= DOT_DOT_OFFSET)
	    break;
	make_cpu_key (&pos_key, inode, next_pos, TYPE_DIRENTRY, 3);

	search_res = search_by_entry_key(inode->i_sb, &pos_key, &path_to_entry, &de);
	if (search_res == IO_ERROR) {
	    // FIXME: we could just skip part of directory which could
	    // not be read
	    pathrelse(&path_to_entry);
	    return -EIO;
	}

	if (search_res == NAME_NOT_FOUND)
	    de.de_entry_num--;

	set_de_name_and_namelen(&de);
	entry_num = de.de_entry_num;
	deh = &(de.de_deh[entry_num]);

	bh = de.de_bh;
	ih = de.de_ih;

	if (!is_direntry_le_ih(ih)) {
reiserfs_warning("not direntry %h\n", ih);
	    break;
        }
	copy_item_head(&tmp_ih, ih);
		
	/* we must have found item, that is item of this directory, */
	RFALSE( COMP_SHORT_KEYS (&(ih->ih_key), &pos_key),
		"vs-9000: found item %h does not match to dir we readdir %K",
		ih, &pos_key);

	if (deh_offset(deh) <= DOT_DOT_OFFSET) {
	    break;
	}

	/* look for the previous entry in the directory */
	next_pos = deh_offset (deh) - 1;

	if (!de_visible (deh))
	    /* it is hidden entry */
	    continue;

	d_reclen = entry_length(bh, ih, entry_num);
	d_name = B_I_DEH_ENTRY_FILE_NAME (bh, ih, deh);
	d_off = deh_offset (deh);
	d_ino = deh_objectid (deh);

	if (!d_name[d_reclen - 1])
	    d_reclen = strlen (d_name);

	if (d_reclen > REISERFS_MAX_NAME(inode->i_sb->s_blocksize)){
	    /* too big to send back to VFS */
	    continue ;
	}

        /* Ignore the .reiserfs_priv entry */
        if (reiserfs_xattrs (inode->i_sb) && 
            !old_format_only(inode->i_sb) &&
            deh_objectid (deh) == le32_to_cpu (INODE_PKEY(inode->i_sb->u.reiserfs_sb.priv_root->d_inode)->k_objectid))
          continue;

	if (d_reclen <= 32) {
	  local_buf = small_buf ;
	} else {
	    local_buf = reiserfs_kmalloc(d_reclen, GFP_NOFS, inode->i_sb) ;
	    if (!local_buf) {
		pathrelse (&path_to_entry);
		return -ENOMEM ;
	    }
	    if (item_moved (&tmp_ih, &path_to_entry)) {
		reiserfs_kfree(local_buf, d_reclen, inode->i_sb) ;

		/* sigh, must retry.  Do this same offset again */
		next_pos = d_off;
		goto research;
	    }
	}

	// Note, that we copy name to user space via temporary
	// buffer (local_buf) because filldir will block if
	// user space buffer is swapped out. At that time
	// entry can move to somewhere else
	memcpy (local_buf, d_name, d_reclen);

	/* the filldir function might need to start transactions,
	 * or do who knows what.  Release the path now that we've
	 * copied all the important stuff out of the deh
	 */
	pathrelse (&path_to_entry);

	if (filldir (dirent, local_buf, d_reclen, d_off, d_ino, 
		     DT_UNKNOWN) < 0) {
	    if (local_buf != small_buf) {
		reiserfs_kfree(local_buf, d_reclen, inode->i_sb) ;
	    }
	    goto end;
	}
	if (local_buf != small_buf) {
	    reiserfs_kfree(local_buf, d_reclen, inode->i_sb) ;
	}
    } /* while */

end:
    pathrelse (&path_to_entry);
    return 0;
}

/* 
 * this could be done with dedicated readdir ops for the xattr files,
 * but I want to get something working asap
 * this is stolen from vfs_readdir
 *
 */
static
int xattr_readdir(struct file *file, filldir_t filler, void *buf)
{
        struct inode *inode = file->f_dentry->d_inode;
        int res = -ENOTDIR;
        if (!file->f_op || !file->f_op->readdir)
                goto out;
        down(&inode->i_sem);
        down(&inode->i_zombie);
        res = -ENOENT;
        if (!IS_DEADDIR(inode)) {
                lock_kernel();
                res = __xattr_readdir(file, buf, filler);
                unlock_kernel();
        }
        up(&inode->i_zombie);
        up(&inode->i_sem);
out:
        return res;
}


/* Internal operations on file data */
static inline void
reiserfs_put_page(struct page *page)
{
        kunmap(page);
        page_cache_release(page);
}

static struct page *
reiserfs_get_page(struct inode *dir, unsigned long n)
{
        struct address_space *mapping = dir->i_mapping; 
        struct page *page;
        /* We can deadlock if we try to free dentries,
           and an unlink/rmdir has just occured - GFP_NOFS avoids this */
        mapping->gfp_mask = GFP_NOFS;
        page = read_cache_page (mapping, n,
                                (filler_t*)mapping->a_ops->readpage, NULL);
        if (!IS_ERR(page)) {
                wait_on_page(page);
                kmap(page);
                if (!Page_Uptodate(page))
                        goto fail;

                if (PageError(page))
                        goto fail;
        }
        return page;

fail:
        reiserfs_put_page(page);
        return ERR_PTR(-EIO);
}

static inline __u32
xattr_hash (const char *msg, int len)
{
    return csum_partial (msg, len, 0);
}

/* Generic extended attribute operations that can be used by xa plugins */

/*
 * inode->i_sem: down
 */
int
reiserfs_xattr_set (struct inode *inode, const char *name, const void *buffer,
                    size_t buffer_size, int flags)
{
    int err = 0;
    struct file *fp;
    struct page *page;
    char *data;
    struct address_space *mapping;
    size_t file_pos = 0;
    size_t buffer_pos = 0;
    struct inode *xinode;
    struct iattr newattrs;
    __u32 xahash = 0;

    if (get_inode_sd_version (inode) == STAT_DATA_V1)
        return -EOPNOTSUPP;

    /* Empty xattrs are ok, they're just empty files, no hash */
    if (buffer && buffer_size)
        xahash = xattr_hash (buffer, buffer_size);

open_file:
    fp = open_xa_file (inode, name, flags);
    if (IS_ERR (fp)) {
        err = PTR_ERR (fp);
        goto out;
    }

    xinode = fp->f_dentry->d_inode;

    /* we need to copy it off.. */
    if (xinode->i_nlink > 1) {
	fput(fp);
        err = reiserfs_xattr_del (inode, name);
        if (err < 0)
            goto out;
        /* We just killed the old one, we're not replacing anymore */
        if (flags & XATTR_REPLACE)
            flags &= ~XATTR_REPLACE;
        goto open_file;
    }

    /* Resize it so we're ok to write there */
    newattrs.ia_size = buffer_size;
    newattrs.ia_valid = ATTR_SIZE | ATTR_CTIME;
    down (&xinode->i_sem);
    err = notify_change(fp->f_dentry, &newattrs);
    if (err)
        goto out_filp;

    mapping = xinode->i_mapping;
    while (buffer_pos < buffer_size || buffer_pos == 0) {
        size_t chunk;
        size_t skip = 0;
        size_t page_offset = (file_pos & (PAGE_CACHE_SIZE - 1));
        if (buffer_size - buffer_pos > PAGE_CACHE_SIZE)
            chunk = PAGE_CACHE_SIZE;
        else
            chunk = buffer_size - buffer_pos;

        page = reiserfs_get_page (xinode, file_pos >> PAGE_CACHE_SHIFT);
        if (IS_ERR (page)) {
            err = PTR_ERR (page);
            goto out_filp;
        }

        lock_page (page);
        data = page_address (page);

        if (file_pos == 0) {
            struct reiserfs_xattr_header *rxh;
            skip = file_pos = sizeof (struct reiserfs_xattr_header);
            if (chunk + skip > PAGE_CACHE_SIZE)
                chunk = PAGE_CACHE_SIZE - skip;
            rxh = (struct reiserfs_xattr_header *)data;
            rxh->h_magic = cpu_to_le32 (REISERFS_XATTR_MAGIC);
            rxh->h_hash = cpu_to_le32 (xahash);
        }

        err = mapping->a_ops->prepare_write (fp, page, page_offset,
                                             page_offset + chunk + skip);
        if (!err) {
	    if (buffer)
		memcpy (data + skip, buffer + buffer_pos, chunk);
            err = mapping->a_ops->commit_write (fp, page, page_offset,
                                                page_offset + chunk + skip);
	}
        UnlockPage (page);
        reiserfs_put_page (page);
        buffer_pos += chunk;
        file_pos += chunk;
        skip = 0;
        if (err || buffer_size == 0 || !buffer)
            break;
    }

out_filp:
    up (&xinode->i_sem);
    fput(fp);

out:
    return err;
}

/*
 * inode->i_sem: down
 */
int
reiserfs_xattr_get (const struct inode *inode, const char *name, void *buffer,
                    size_t buffer_size)
{
    ssize_t err = 0;
    struct file *fp;
    size_t isize;
    size_t file_pos = 0;
    size_t buffer_pos = 0;
    struct page *page;
    struct inode *xinode;
    __u32 hash = 0;

    /* We can't have xattrs attached to v1 items since they don't have
     * generation numbers */
    if (get_inode_sd_version (inode) == STAT_DATA_V1)
        return -EOPNOTSUPP;

    fp = open_xa_file (inode, name, FL_READONLY);
    if (IS_ERR (fp)) {
        err = PTR_ERR (fp);
        goto out;
    }

    xinode = fp->f_dentry->d_inode;
    isize = xinode->i_size;

    /* Just return the size needed */
    if (buffer == NULL) {
        err = isize - sizeof (struct reiserfs_xattr_header);
        goto out_dput;
    }

    if (buffer_size < isize - sizeof (struct reiserfs_xattr_header)) {
        err = -ERANGE;
        goto out_dput;
    }
    
    while (file_pos < isize) {
        size_t chunk;
        char *data;
        size_t skip = 0;
        if (isize - file_pos > PAGE_CACHE_SIZE)
            chunk = PAGE_CACHE_SIZE;
        else
            chunk = isize - file_pos;

        page = reiserfs_get_page (xinode, file_pos >> PAGE_CACHE_SHIFT);
        if (IS_ERR (page)) {
            err = PTR_ERR (page);
            goto out_dput;
        }

        lock_page (page);
        data = page_address (page);
        if (file_pos == 0) {
            struct reiserfs_xattr_header *rxh =
                                        (struct reiserfs_xattr_header *)data;
            skip = file_pos = sizeof (struct reiserfs_xattr_header);
            chunk -= skip;
            /* Magic doesn't match up.. */
            if (rxh->h_magic != cpu_to_le32 (REISERFS_XATTR_MAGIC)) {
                UnlockPage (page);
                reiserfs_put_page (page);
                err = -EIO;
                goto out_dput;
            }
            hash = le32_to_cpu (rxh->h_hash);
        }
        memcpy (buffer + buffer_pos, data + skip, chunk);
        UnlockPage (page);
        reiserfs_put_page (page);
        file_pos += chunk;
        buffer_pos += chunk;
        skip = 0;
    }
    err = isize - sizeof (struct reiserfs_xattr_header);

    if (xattr_hash (buffer, isize - sizeof (struct reiserfs_xattr_header)) != hash)
        err = -EIO;

out_dput:
    fput(fp);

out:
    return err;
}

static int
__reiserfs_xattr_del (struct dentry *xadir, const char *name, int namelen)
{
    struct dentry *file;
    struct inode *dir = xadir->d_inode;
    int err = 0;

    file = lookup_one_len (name, xadir, namelen);
    if (IS_ERR (file)) {
        err = PTR_ERR (file);
        goto out;
    } else if (!file->d_inode) {
        err = -ENODATA;
        goto out_file;
    }

    /* Skip directories.. */
    if (S_ISDIR (file->d_inode->i_mode))
        goto out_file;

    if (!is_reiserfs_priv_object (file->d_inode)) {
        reiserfs_warning ("trying to delete objectid %08x, which isn't an xattr!\n", le32_to_cpu (INODE_PKEY (file->d_inode)->k_objectid));
        dput (file);
        return -EIO;
    }

    err = dir->i_op->unlink (dir, file);
    if (!err)
        d_delete (file);

out_file:
    dput (file);

out:
    return err;
}


int
reiserfs_xattr_del (struct inode *inode, const char *name)
{
    struct dentry *dir;
    int err;

    dir = open_xa_dir (inode, FL_READONLY);
    if (IS_ERR (dir)) {
        err = PTR_ERR (dir);
        goto out;
    }

    err = __reiserfs_xattr_del (dir, name, strlen (name));
    dput (dir);

out:
    return err;
}

/* The following are side effects of other operations that aren't explicitly
 * modifying extended attributes. This includes operations such as permissions
 * or ownership changes, object deletions, etc. */
 
static int
reiserfs_delete_xattrs_filler (void *buf, const char *name, int namelen,
                               loff_t offset, ino_t ino, unsigned int d_type)
{
    struct dentry *xadir = (struct dentry *)buf;

    return __reiserfs_xattr_del (xadir, name, namelen);

}

int
reiserfs_delete_xattrs (struct inode *inode)
{
    struct file *fp;
    struct dentry *dir, *root;
    int err = 0;

    /* Skip out, an xattr has no xattrs associated with it */
    if (is_reiserfs_priv_object (inode) ||
        get_inode_sd_version (inode) == STAT_DATA_V1 || 
        !reiserfs_xattrs(inode->i_sb))
    {
        return 0;
    }
    reiserfs_read_lock_xattrs (inode->i_sb);
    dir = open_xa_dir (inode, FL_READONLY);
    reiserfs_read_unlock_xattrs (inode->i_sb);
    if (IS_ERR (dir)) {
        err = PTR_ERR (dir);
        goto out;
    } else if (!dir->d_inode) {
        dput (dir);
        return 0;
    }

    fp = dentry_open (dir, NULL, O_RDWR);
    if (IS_ERR (fp)) {
        err = PTR_ERR (fp);
        /* dentry_open dputs the dentry if it fails */
        goto out;
    }

    lock_kernel ();
    err = xattr_readdir (fp, reiserfs_delete_xattrs_filler, dir);
    if (err) {
        unlock_kernel ();
        goto out_dir;
    }

    /* Leftovers besides . and .. -- that's not good. */
    if (dir->d_inode->i_nlink <= 2) {
        root = get_xa_root (inode->i_sb);
        reiserfs_write_lock_xattrs (inode->i_sb);
        err = vfs_rmdir (root->d_inode, dir);
        reiserfs_write_unlock_xattrs (inode->i_sb);
        dput (root);
    } else {
        reiserfs_warning ("Couldn't remove all entries in directory\n");
    }
    unlock_kernel ();

out_dir:
    fput(fp);

out:
    return err;
}

struct reiserfs_chown_buf {
    struct inode *inode;
    struct dentry *xadir;
    struct iattr *attrs;
};

/* XXX: If there is a better way to do this, I'd love to hear about it */
static int
reiserfs_chown_xattrs_filler (void *buf, const char *name, int namelen,
                               loff_t offset, ino_t ino, unsigned int d_type)
{
    struct reiserfs_chown_buf *chown_buf = (struct reiserfs_chown_buf *)buf;
    struct dentry *xafile, *xadir = chown_buf->xadir;
    struct iattr *attrs = chown_buf->attrs;
    int err = 0;

    xafile = lookup_one_len (name, xadir, namelen);
    if (IS_ERR (xafile))
        return PTR_ERR (xafile);
    else if (!xafile->d_inode) {
        dput (xafile);
        return -ENODATA;
    }

    if (!S_ISDIR (xafile->d_inode->i_mode))
        err = notify_change (xafile, attrs);
    dput (xafile);

    return err;
}

int
reiserfs_chown_xattrs (struct inode *inode, struct iattr *attrs)
{
    struct file *fp;
    struct dentry *dir;
    int err = 0;
    struct reiserfs_chown_buf buf;
    unsigned int ia_valid = attrs->ia_valid;

    /* Skip out, an xattr has no xattrs associated with it */
    if (is_reiserfs_priv_object (inode) ||
        get_inode_sd_version (inode) == STAT_DATA_V1 || 
        !reiserfs_xattrs(inode->i_sb))
    {
        return 0;
    }
    reiserfs_read_lock_xattrs (inode->i_sb);
    dir = open_xa_dir (inode, FL_READONLY);
    reiserfs_read_unlock_xattrs (inode->i_sb);
    if (IS_ERR (dir)) {
        if (PTR_ERR (dir) != -ENODATA)
            err = PTR_ERR (dir);
        goto out;
    } else if (!dir->d_inode) {
        dput (dir);
        goto out;
    }

    fp = dentry_open (dir, NULL, O_RDWR);
    if (IS_ERR (fp)) {
        err = PTR_ERR (fp);
        /* dentry_open dputs the dentry if it fails */
        goto out;
    }

    lock_kernel ();

    attrs->ia_valid &= (ATTR_UID | ATTR_GID | ATTR_CTIME);
    buf.xadir = dir;
    buf.attrs = attrs;
    buf.inode = inode;

    err = xattr_readdir (fp, reiserfs_chown_xattrs_filler, &buf);
    if (err) {
        unlock_kernel ();
        goto out_dir;
    }

    err = notify_change (dir, attrs);
    unlock_kernel ();

out_dir:
    fput(fp);

out:
    attrs->ia_valid = ia_valid;
    return err;
}


/* Actual operations that are exported to VFS-land */

/*
 * Inode operation getxattr()
 * dentry->d_inode->i_sem down
 * BKL held [before 2.5.x]
 */
ssize_t
reiserfs_getxattr (struct dentry *dentry, const char *name, void *buffer,
                   size_t size)
{
    struct reiserfs_xattr_handler *xah = find_xattr_handler_prefix (name);
    int err;

    if (!xah || !reiserfs_xattrs(dentry->d_sb) ||
        get_inode_sd_version (dentry->d_inode) == STAT_DATA_V1)
        return -EOPNOTSUPP;
    
    reiserfs_read_lock_xattrs (dentry->d_sb);
    err = xah->get (dentry->d_inode, name, buffer, size);
    reiserfs_read_unlock_xattrs (dentry->d_sb);
    return err;
}


/*
 * Inode operation setxattr()
 *
 * dentry->d_inode->i_sem down
 * BKL held [before 2.5.x]
 */
int
reiserfs_setxattr (struct dentry *dentry, const char *name, const void *value,
                   size_t size, int flags)
{
    struct reiserfs_xattr_handler *xah = find_xattr_handler_prefix (name);
    int err;

    if (!xah || !reiserfs_xattrs(dentry->d_sb) ||
        get_inode_sd_version (dentry->d_inode) == STAT_DATA_V1)
        return -EOPNOTSUPP;
    
    
    reiserfs_write_lock_xattrs (dentry->d_sb);
    err = xah->set (dentry->d_inode, name, value, size, flags);
    reiserfs_write_unlock_xattrs (dentry->d_sb);
    return err;
}

/*
 * Inode operation removexattr()
 *
 * dentry->d_inode->i_sem down
 * BKL held [before 2.5.x]
 */
int
reiserfs_removexattr (struct dentry *dentry, const char *name)
{
    int err;
    struct reiserfs_xattr_handler *xah = find_xattr_handler_prefix (name);

    if (!xah || !reiserfs_xattrs(dentry->d_sb) ||
        get_inode_sd_version (dentry->d_inode) == STAT_DATA_V1)
        return -EOPNOTSUPP;

    down (&dentry->d_inode->i_zombie);
    reiserfs_read_lock_xattrs (dentry->d_sb);

    /* Deletion pre-operation */
    if (xah->del) {
        err = xah->del (dentry->d_inode, name);
        if (err) {
            reiserfs_read_unlock_xattrs (dentry->d_sb);
            up (&dentry->d_inode->i_zombie);
            return err;
        }
    }

    err = reiserfs_xattr_del (dentry->d_inode, name);
    reiserfs_read_unlock_xattrs (dentry->d_sb);
    up (&dentry->d_inode->i_zombie);
    return err;
}


/* This is what filldir will use:
 * r_pos will always contain the amount of space required for the entire
 * list. If r_pos becomes larger than r_size, we need more space and we
 * return an error indicating this. If r_pos is less than r_size, then we've
 * filled the buffer successfully and we return success */
struct reiserfs_listxattr_buf {
    int r_pos;
    int r_size;
    char *r_buf;
    struct inode *r_inode;
};

static int
reiserfs_listxattr_filler (void *buf, const char *name, int namelen,
                           loff_t offset, ino_t ino, unsigned int d_type)
{
    struct reiserfs_listxattr_buf *b = (struct reiserfs_listxattr_buf *)buf;
    int len = 0;
    if (name[0] != '.' || (namelen != 1 && (name[1] != '.' || namelen != 2))) {
        struct reiserfs_xattr_handler *xah = find_xattr_handler_prefix (name);
        if (!xah) return 0; /* Unsupported xattr name, skip it */

        /* We call ->list() twice because the operation isn't required to just
         * return the name back - we want to make sure we have enough space */
        len += xah->list (b->r_inode, name, namelen, NULL);

        if (len) {
            if (b->r_pos + len + 1 <= b->r_size) {
                char *p = b->r_buf + b->r_pos;
                p += xah->list (b->r_inode, name, namelen, p);
                *p++ = '\0';
            }
            b->r_pos += len + 1;
        }
    }

    return 0;
}
/*
 * Inode operation listxattr()
 *
 * dentry->d_inode->i_sem down
 * BKL held [before 2.5.x]
 */
ssize_t
reiserfs_listxattr (struct dentry *dentry, char *buffer, size_t size)
{
    struct file *fp;
    struct dentry *dir;
    int err = 0;
    struct reiserfs_listxattr_buf buf;

    if (!dentry->d_inode)
        return -EINVAL;

    if (!reiserfs_xattrs(dentry->d_sb) ||
        get_inode_sd_version (dentry->d_inode) == STAT_DATA_V1)
        return -EOPNOTSUPP;

    reiserfs_read_lock_xattrs (dentry->d_sb);
    dir = open_xa_dir (dentry->d_inode, FL_READONLY);
    reiserfs_read_unlock_xattrs (dentry->d_sb);
    if (IS_ERR (dir)) {
        err = PTR_ERR (dir);
        if (err == -ENODATA)
            err = 0; /* Not an error if there aren't any xattrs */
        goto out;
    }

    fp = dentry_open (dir, NULL, O_RDWR);
    if (IS_ERR (fp)) {
        err = PTR_ERR (fp);
        /* dentry_open dputs the dentry if it fails */
        goto out;
    }

    buf.r_buf = buffer;
    buf.r_size = buffer ? size : 0;
    buf.r_pos = 0;
    buf.r_inode = dentry->d_inode;

    err = xattr_readdir (fp, reiserfs_listxattr_filler, &buf);
    if (err)
        goto out_dir;

    if (buf.r_pos > buf.r_size && buffer != NULL)
        err = -ERANGE;
    else
        err = buf.r_pos;

out_dir:
    fput(fp);

out:
    return err;
}

/* This is the implementation for the xattr plugin infrastructure */
static struct reiserfs_xattr_handler *xattr_handlers;
static rwlock_t handler_lock = RW_LOCK_UNLOCKED;

static struct reiserfs_xattr_handler *
find_xattr_handler_prefix (const char *prefix)
{
    struct reiserfs_xattr_handler **xah;
    read_lock (&handler_lock);
    for (xah = &xattr_handlers; *xah; xah=&(*xah)->next)
        if (strncmp ((*xah)->prefix, prefix, strlen ((*xah)->prefix)) == 0)
            break;
    read_unlock (&handler_lock);
    return *xah;
}

int
reiserfs_xattr_register_handler (struct reiserfs_xattr_handler *handler)
{
    int res = 0;
    struct reiserfs_xattr_handler **xah;

    if (!handler)
        return -EINVAL;

    if (handler->next)
        return -EBUSY;

    write_lock (&handler_lock);

    for (xah = &xattr_handlers; *xah; xah=&(*xah)->next) {
        if (strcmp ((*xah)->prefix, handler->prefix) == 0)
            break;
    }
    if (*xah)
        res = -EBUSY;
    else
        *xah = handler;

    /*
    if (!res)
        printk ("ReiserFS: Registered xattr handler for %s\n", handler->prefix);
    */

    write_unlock (&handler_lock);
    return res;
}

int
reiserfs_xattr_unregister_handler (struct reiserfs_xattr_handler *handler)
{
    struct reiserfs_xattr_handler **xah;
    write_lock (&handler_lock);

    xah = &xattr_handlers;
    while (*xah) {
        if (handler == *xah) {
            *xah = handler->next;
            handler->next = NULL;
            write_unlock (&handler_lock);
            /*
            printk ("ReiserFS: Unregistered xattr handler for %s\n",
                    handler->prefix);
            */
            return 0;
        }
        xah = &(*xah)->next;
    }
    write_unlock (&handler_lock);
    return -EINVAL;
}

/* We need to take a copy of the mount flags since things like
 * MS_RDONLY don't get set until *after* we're called.
 * mount_flags != mount_options */

int
reiserfs_xattr_init (struct super_block *s, int mount_flags)
{
  int err = 0;

  /* I hate using the _NO_ variant to clear bits. We use the normal variant
   * everywhere else, so just clear them both here */
  if (test_bit (REISERFS_NO_XATTRS_USER, &(s->u.reiserfs_sb.s_mount_opt))) {
    clear_bit (REISERFS_XATTRS_USER, &(s->u.reiserfs_sb.s_mount_opt));
    clear_bit (REISERFS_NO_XATTRS_USER, &(s->u.reiserfs_sb.s_mount_opt));
  }

  if (test_bit (REISERFS_NO_POSIXACL, &(s->u.reiserfs_sb.s_mount_opt))) {
    clear_bit (REISERFS_POSIXACL, &(s->u.reiserfs_sb.s_mount_opt));
    clear_bit (REISERFS_NO_POSIXACL, &(s->u.reiserfs_sb.s_mount_opt));
  }

  /* If the user has requested an optional xattrs type (e.g. user/acl), then
   * enable xattrs. If we're a v3.5 filesystem, this will get caught and
   * error out. If no optional xattrs are enabled, disable xattrs */
  if (reiserfs_xattrs_optional (s))
    set_bit (REISERFS_XATTRS, &(s->u.reiserfs_sb.s_mount_opt));
  else
    clear_bit (REISERFS_XATTRS, &(s->u.reiserfs_sb.s_mount_opt));

  if (reiserfs_xattrs (s)) {
    /* We need generation numbers to ensure that the oid mapping is correct
     * v3.5 filesystems don't have them. */
    if (old_format_only (s)) {
      reiserfs_warning ("reiserfs: xattrs/ACLs not supported on pre v3.6 "
                        "format filesystem. Failing mount.\n");
      err = -EOPNOTSUPP;
      goto error;
    } else if (!s->u.reiserfs_sb.priv_root) {
      struct dentry *dentry;
      dentry = lookup_one_len (PRIVROOT_NAME, s->s_root,
                               strlen (PRIVROOT_NAME));
      if (!IS_ERR (dentry)) {
        if (!(mount_flags & MS_RDONLY) && !dentry->d_inode) {
            struct inode *inode = dentry->d_parent->d_inode;
            down (&inode->i_sem);
            err = inode->i_op->mkdir (inode, dentry, 0700);
            up (&inode->i_sem);
            if (err) {
                dput (dentry);
                dentry = NULL;
            }

            if (dentry && dentry->d_inode)
                reiserfs_warning ("reiserfs: Created %s on %s - reserved for "
                                  "xattr storage.\n", PRIVROOT_NAME, 
                                  bdevname (inode->i_sb->s_dev));
        }
      } else
        err = PTR_ERR (dentry);

      if (!err) {
          d_drop (dentry);
          dentry->d_inode->u.reiserfs_i.i_flags |= i_priv_object;
          s->u.reiserfs_sb.priv_root = dentry;
      } else { /* xattrs are unavailable */
          /* If we're read-only it just means that the dir hasn't been
           * created. Not an error -- just no xattrs on the fs. We'll
           * check again if we go read-write */
          if (!(mount_flags & MS_RDONLY)) {
              reiserfs_warning ("reiserfs: xattrs/ACLs enabled and couldn't "
                             "find/create .reiserfs_priv. Failing mount.\n");
            err = -EOPNOTSUPP;
            goto error;
          }
          /* Just to speed things up a bit since it won't find anything and
           * we're read-only */
          clear_bit (REISERFS_XATTRS, &(s->u.reiserfs_sb.s_mount_opt));
          clear_bit (REISERFS_XATTRS_USER, &(s->u.reiserfs_sb.s_mount_opt));
          clear_bit (REISERFS_POSIXACL, &(s->u.reiserfs_sb.s_mount_opt));
      }
    }
  }

error:
   /* This is only nonzero if there was an error initializing the xattr
    * directory or if there is a condition where we don't support them. */

    if (err) {
          clear_bit (REISERFS_XATTRS, &(s->u.reiserfs_sb.s_mount_opt));
          clear_bit (REISERFS_XATTRS_USER, &(s->u.reiserfs_sb.s_mount_opt));
          clear_bit (REISERFS_POSIXACL, &(s->u.reiserfs_sb.s_mount_opt));
    }

    s->s_flags = (s->s_flags & ~MS_POSIXACL) |
                 (reiserfs_posixacl (s) ? MS_POSIXACL : 0);
    return err;
}
