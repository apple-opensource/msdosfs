/*
 * Copyright (c) 2000-2010 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
/* $FreeBSD: src/sys/msdosfs/msdosfs_lookup.c,v 1.31 2000/05/05 09:58:35 phk Exp $ */
/*	$NetBSD: msdosfs_lookup.c,v 1.37 1997/11/17 15:36:54 ws Exp $	*/

/*-
 * Copyright (C) 1994, 1995, 1997 Wolfgang Solfrank.
 * Copyright (C) 1994, 1995, 1997 TooLs GmbH.
 * All rights reserved.
 * Original code by Paul Popelka (paulp@uts.amdahl.com) (see below).
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by TooLs GmbH.
 * 4. The name of TooLs GmbH may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY TOOLS GMBH ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL TOOLS GMBH BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/*
 * Written by Paul Popelka (paulp@uts.amdahl.com)
 *
 * You can do anything you want with this software, just don't say you wrote
 * it, and don't remove this notice.
 *
 * This software is provided "as is".
 *
 * The author supplies this software to be publicly redistributed on the
 * understanding that the author is not responsible for the correct
 * functioning of this software in any circumstances and is not liable for
 * any damages caused by this software.
 *
 * October 1992
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/utfconv.h>
#include <libkern/OSMalloc.h>

#include "bpb.h"
#include "direntry.h"
#include "denode.h"
#include "msdosfsmount.h"
#include "fat.h"

#ifndef DEBUG
#define DEBUG 0
#endif

int msdosfs_unicode_to_dos_lookup(u_int16_t *unicode, size_t unichars, u_char shortname[SHORT_NAME_LEN]);

/*
 * Convert a Unicode filename to the equivalent short name.
 *
 * Note: This is for use during lookup, not when creating new names.
 * Therefore, it does not cut out embedded spaces, and does not worry
 * about mixed case.
 *
 * Returns non-zero if the name was successfully converted to a short name.
 */
int msdosfs_unicode_to_dos_lookup(u_int16_t *unicode, size_t unichars, u_char shortname[SHORT_NAME_LEN])
{
	size_t i;
	int j;
	u_char c = ' ';
	
	if (unichars > SHORT_NAME_LEN+1)
		return 0;
	
	/* Fill the short name with spaces, the short name pad character */
	memset(shortname, ' ', SHORT_NAME_LEN);
	
	/* Process the base name, up to the first period */
	for (i=0; i<unichars && i<8; ++i)
	{
		if (unicode[i] == '.')	/* Dot => start extension */
			break;

		if (unicode[i] == ' ')
			c = ' ';
		else
			c = msdosfs_unicode2dos(unicode[i]);
		
		if (c < ' ')
			return 0;
		shortname[i] = c;
	}

	/*
	 * Fail if last char of base is a space (since msdosfs_dos2unicodefn would trim it),
	 * or if the base name is empty (the loop above never executed).
	 */
	if (c == ' ')
		return 0;
	
	/* Short names cannot start with a space. */
	if (shortname[0] == ' ')
		return 0;
	
	/* Is the name a base only, no extension? */
	if (i == unichars)
		return 1;
	
	/* Skip over the dot between base and extension */
	if (unicode[i] == '.')
		++i;
	else
		return 0;			/* Base name too long */
	
	/* Process the extension */
	for (j=8; j < 11 && i < unichars; ++i, ++j)
	{
		if (unicode[i] == '.')	/* No dots in the extension */
			return 0;

		if (unicode[i] == ' ')
			c = ' ';
		else
			c = msdosfs_unicode2dos(unicode[i]);

		if (c < ' ')
			return 0;
		shortname[j] = c;
	}
	
	/* Was the extension too long? */
	if (i < unichars)
		return 0;

	/* Was the extension empty? */
	if (j == 8)
		return 0;

	/* Fail if last char of extension is a space (since msdosfs_dos2unicodefn would trim it) */
	if (c == ' ')
		return 0;
		
	/* Do we need to bother with names starting with 0xE5? */
	if (shortname[0] == 0xE5)
		shortname[0] = SLOT_E5;
	
	return 1;
}


/*
 * msdosfs_lookup_name
 *
 * Search a directory for an entry with a given name.  If found, returns
 * the cluster containing the name's short name directory entry, and the
 * byte offset from the start of the directory (not the cluster!).
 *
 * Assumes dep's de_lock has been acquired.
 */
int msdosfs_lookup_name(
	struct denode *dep,		/* parent directory */
	struct componentname *cnp, /* the name to look up */
	uint32_t *dirclust,		/* cluster containing short name entry */
	uint32_t *diroffset,		/* byte offset from start of directory */
	struct dosdirentry *direntry,	/* copy of found directory entry */
	vfs_context_t context)
{
	int error;
	int chksum;			/* checksum of short name entry */
	struct dosdirentry *dirp;
	buf_t bp;
	daddr64_t bn;
	int frcn;			/* file relative cluster (within parent directory) */
	uint32_t cluster;		/* physical cluster containing directory entry */
	unsigned blkoff;			/* offset within directory block */
	unsigned diroff;			/* offset from start of directory */
	uint32_t blsize;		/* size of one directory block */
	u_int16_t ucfn[WIN_MAXLEN];
	u_char shortname[SHORT_NAME_LEN];
	size_t unichars;	/* number of UTF-16 characters in original name */
	int try_short_name;	/* If true, compare short names */
	
	dirp = NULL;
	chksum = -1;
		
	/*
	 * Decode lookup name into UCS-2 (Unicode)
	 */
	error = utf8_decodestr((uint8_t *)cnp->cn_nameptr, cnp->cn_namelen, ucfn, &unichars, sizeof(ucfn), 0, UTF_PRECOMPOSED|UTF_SFM_CONVERSIONS);
	if (error) 	goto exit;
	unichars /= 2; /* bytes to chars */

	/*
	 * Try to convert the name to a short name.  Unlike the case of creating
	 * a new name in the directory, allow embedded spaces and mixed case,
	 * but do not mangle the short name.  Keep track of whether there is
	 * a valid short name to look up.
	 */
	try_short_name = msdosfs_unicode_to_dos_lookup(ucfn, unichars, shortname);
	
	/*
	 * Search the directory pointed at by dep for the name in ucfn.
	 */

	/*
	 * The outer loop ranges over the clusters that make up the
	 * directory.  Note that the root directory is different from all
	 * other directories.  It has a fixed number of blocks that are not
	 * part of the pool of allocatable clusters.  So, we treat it a
	 * little differently. The root directory starts at "cluster" 0.
	 */
	diroff = 0;
	for (frcn = 0; error == 0; frcn++) {
		error = msdosfs_pcbmap(dep, frcn, 1, &bn, &cluster, &blsize);
		if (error) {
			if (error == E2BIG)
				error = ENOENT;
			break;
		}
		error = (int)buf_meta_bread(dep->de_pmp->pm_devvp, bn, blsize, vfs_context_ucred(context), &bp);
		if (error) {
			buf_brelse(bp);
			break;
		}
		for (blkoff = 0; blkoff < blsize;
		     blkoff += sizeof(struct dosdirentry),
		     diroff += sizeof(struct dosdirentry))
		{
			dirp = (struct dosdirentry *)((char *)buf_dataptr(bp) + blkoff);
			/*
			 * If the slot is empty and we are still looking
			 * for an empty then remember this one.  If the
			 * slot is not empty then check to see if it
			 * matches what we are looking for.  If the slot
			 * has never been filled with anything, then the
			 * remainder of the directory has never been used,
			 * so there is no point in searching it.
			 */
			if (dirp->deName[0] == SLOT_EMPTY ||
			    dirp->deName[0] == SLOT_DELETED) {
				/*
				 * Drop memory of previous long matches
				 */
				chksum = -1;

				/*
				 * If we found SLOT_EMPTY, then we've reached a part of
				 * the directory that has never been used, so we can
				 * stop early.
				 */
				if (dirp->deName[0] == SLOT_EMPTY) {
					error = ENOENT;
					break;
				}
			} else {
				/*
				 * Check for Win95 long filename entry
				 */
				if (dirp->deAttributes == ATTR_WIN95) {
					chksum = msdosfs_winChkName(ucfn,
							    unichars,
							    (struct winentry *)dirp,
							    chksum);
					continue;
				}

				/*
				 * Ignore volume labels (anywhere, not just
				 * the root directory).
				 */
				if (dirp->deAttributes & ATTR_VOLUME) {
					chksum = -1;
					continue;
				}

				/*
				 * If we get here, we've found a short name entry.
				 *
				 * If there was a long name, and it matched, then verify the
				 * checksum.  If the checksum doesn't match, then compare the
				 * short name.
				 */
				if (chksum != msdosfs_winChksum(dirp->deName) &&
					(!try_short_name || bcmp(shortname, dirp->deName, SHORT_NAME_LEN)))
				{
					/* No match.  Forget long name checksum, if any. */
					chksum = -1;
					continue;
				}

				/*
				 * If we get here, we found a matching name.
				 */
				if (dirclust)
					*dirclust = cluster;
				if (diroffset)
					*diroffset = diroff;
				if (direntry)
					*direntry = *dirp;
				error = 0;
				buf_brelse(bp);
				goto exit;
			}
		}	/* for (blkoff = 0; .... */
		
		/*
		 * Release the buffer holding the directory cluster just
		 * searched.
		 */
		buf_brelse(bp);
	}	/* for (frcn = 0; error == 0; frcn++) */

exit:
	return error;
}

/*
 * Try to find the file/directory in the name cache.  If not
 * found there, then look in the directory on disk.
 *
 * When we search a directory the blocks containing directory entries are
 * read and examined.  The directory entries contain information that would
 * normally be in the inode of a unix filesystem.  This means that some of
 * a directory's contents may also be in memory resident denodes (sort of
 * an inode).  This can cause problems if we are searching while some other
 * process is modifying a directory.  To prevent one process from accessing
 * incompletely modified directory information we depend upon being the
 * sole owner of a directory block.  buf_bread/buf_brelse provide this service.
 * This being the case, when a process modifies a directory it must first
 * acquire the disk block that contains the directory entry to be modified.
 * Then update the disk block and the denode, and then write the disk block
 * out to disk.  This way disk blocks containing directory entries and in
 * memory denode's will be in synch.
 */

int msdosfs_vnop_lookup(struct vnop_lookup_args *ap)
/* {
		vnode_t a_dvp;
		vnode_t *a_vpp;
		struct componentname *a_cnp;
		vfs_context_t a_context;
	} */
{
	vnode_t dvp = ap->a_dvp;
	vnode_t *vpp = ap->a_vpp;
	struct componentname *cnp = ap->a_cnp;
	vfs_context_t context = ap->a_context;
	int flags = cnp->cn_flags;
	int nameiop = cnp->cn_nameiop;
	int error;
	struct msdosfsmount *pmp;
	struct denode *pdp;	/* denode of dvp */
	struct denode *dp;	/* denode of found item */
	uint32_t cluster;		/* physical cluster containing directory entry */
	uint32_t diroff;		/* offset from start of directory */
	uint32_t scn;			/* starting cluster number of found item */
	int isadir;			/* non-zero if found dosdirentry is a directory */
	struct dosdirentry direntry;
	
	*vpp = NULL;	/* In case we return an error */
	
	error = cache_lookup(dvp, vpp, cnp);

	if (error)
	{
		/* We found a cache entry, positive or negative. */
		if (error == -1)	/* Positive entry? */
			error = 0;		/* Yes.  Caller expects no error */
		
		return error;
	}

	/* If we get here, we need to look for the item on disk. */

	pdp = VTODE(dvp);
	lck_mtx_lock(pdp->de_lock);
	pmp = pdp->de_pmp;

	/*
	 * If they are going after the . or .. entry in the root directory,
	 * they won't find it.  DOS filesystems don't have them in the root
	 * directory.  So, we fake it. msdosfs_deget() is in on this scam too.
	 */
	if ((pdp->de_flag & DE_ROOT) && cnp->cn_nameptr[0] == '.' &&
	    (cnp->cn_namelen == 1 ||
		(cnp->cn_namelen == 2 && cnp->cn_nameptr[1] == '.')))
	{
		isadir = ATTR_DIRECTORY;
		scn = MSDOSFSROOT;
		cluster = MSDOSFSROOT;
		diroff = MSDOSFSROOT_OFS;
		goto foundroot;
	}

	/*
	 * If they're looking for ".", then just take another reference on dvp.
	 */
	if (cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.')
	{
		vnode_get(dvp);
		*vpp = dvp;
		goto exit;	/* error must be 0 if we got here */
	}
	
	/*
	 * If they're looking for "..", then just take another reference on dvp's
	 * parent vnode.
	 */
	if (flags & ISDOTDOT)
	{
		vnode_t vp;
		
		dp = pdp->de_parent;
		if (dp == NULL)
			panic("msdosfs_vnop_lookup: de_parent == NULL when looking for ..\n");
		vp = DETOV(dp);
		if (vp == NULLVP)
			panic("msdosfs_vnop_lookup: vp == NULL when looking for ..\n");
		vnode_get(vp);
		*vpp = vp;
		goto exit;	/* error must be 0 if we got here */
	}
	
	error = msdosfs_lookup_name(pdp, cnp, &cluster, &diroff, &direntry, context);

	if (error == ENOENT)
	{
		/*
		 * If we get here we didn't find the entry we were looking for. But
		 * that's ok if we are creating or renaming and are at the end of
		 * the pathname and the directory hasn't been removed.
		 */
		if ((nameiop == CREATE || nameiop == RENAME) &&
			(flags & ISLASTCN) && pdp->de_refcnt != 0)
		{
			error = EJUSTRETURN;
			goto exit;
		}
		
		/*
		 * Insert name into cache (as non-existent) if appropriate.
		 */
		if ((flags & MAKEENTRY) && nameiop != CREATE)
			cache_enter(dvp, *vpp, cnp);

		goto exit;
	}

	/*
	 * If we got any other error from msdosfs_lookup_name, return it now.
	 */
	if (error)
		goto exit;

	/*
	 * If we get here, we've found the directory entry.
	 */
	isadir = direntry.deAttributes & ATTR_DIRECTORY;
	scn = getuint16(direntry.deStartCluster);
	if (FAT32(pmp)) {
		scn |= getuint16(direntry.deHighClust) << 16;
		if (scn == pmp->pm_rootdirblk) {
			/*
			 * There should actually be 0 here.
			 * Just ignore the error.
			 */
			scn = MSDOSFSROOT;
		}
	}

foundroot:
	/*
	 * If we entered at foundroot, then we are looking for the . or ..
	 * entry of the filesystems root directory.  isadir and scn were
	 * setup before jumping here.
	 */
	if (FAT32(pmp) && scn == MSDOSFSROOT)
		scn = pmp->pm_rootdirblk;

	if (nameiop == DELETE && (flags & ISLASTCN)) {
		/*
		 * Don't allow deleting the root.
		 */
		if (diroff == MSDOSFSROOT_OFS)
		{
			error = EROFS;				/* correct error? */
			goto exit;
		}
	}

	if (nameiop == RENAME && (flags & ISLASTCN)) {
		if (diroff == MSDOSFSROOT_OFS)
		{
			error = EROFS;				/* really? XXX */
			goto exit;
		}

		if (pdp->de_StartCluster == scn && isadir)
		{
			error = EISDIR;
			goto exit;
		}
	}

	/*
	 * Return a vnode for the found directory entry.
	 */
	error = msdosfs_deget(pmp, cluster, diroff, dvp, cnp, &dp, context);
	if (error == 0)
		*vpp = DETOV(dp);

exit:
	lck_mtx_unlock(pdp->de_lock);
	return error;
}


/*
 * dep  - directory entry to copy into the directory
 * ddep - directory to add to
 * depp - return the address of the denode for the created directory entry
 *	  if depp != 0
 * cnp  - componentname needed for Win95 long filenames
 * offset - directory offset for short name entry
 * long_count - number of long name entries needed
 */
int msdosfs_createde(
	struct denode *dep,
	struct denode *ddep,
	struct denode **depp,
	struct componentname *cnp,
	uint32_t offset,		/* also offset of current entry being written */
	uint32_t long_count,	/* also count of entries remaining to write */
	vfs_context_t context)
{
	int error;
	uint32_t dirclust, diroffset;
	struct dosdirentry *ndep;
	struct msdosfsmount *pmp = ddep->de_pmp;
	struct buf *bp;
	daddr64_t bn;
	uint32_t blsize;

	/*
	 * If no space left in the directory then allocate another cluster
	 * and chain it onto the end of the file.  There is one exception
	 * to this.  That is, if the root directory has no more space it
	 * can NOT be expanded.  msdosfs_extendfile() checks for and fails attempts
	 * to extend the root directory.  We just return an error in that
	 * case.
	 */
    if (offset >= ddep->de_FileSize) {
        diroffset = offset + sizeof(struct dosdirentry)
        		- ddep->de_FileSize;
        dirclust = de_clcount(pmp, diroffset);
        error = msdosfs_extendfile(ddep, dirclust);
        if (error) {
            (void)msdosfs_detrunc(ddep, ddep->de_FileSize, 0, context);
            return error;
        }

        /*
         * Update the size of the directory
         */
        ddep->de_FileSize += de_cn2off(pmp, dirclust);
    }

	/*
	 * We just read in the cluster with space.  Copy the new directory
	 * entry in.  Then write it to disk. NOTE:  DOS directories
	 * do not get smaller as clusters are emptied.
	 */
	error = msdosfs_pcbmap(ddep, de_cluster(pmp, offset), 1,
		       &bn, &dirclust, &blsize);
	if (error)
		return error;
	diroffset = offset;
	if ((error = (int)buf_meta_bread(pmp->pm_devvp, bn, blsize, vfs_context_ucred(context), &bp)) != 0) {
		buf_brelse(bp);
		return error;
	}
	ndep = bptoep(pmp, bp, offset);

	if (DEBUG)
	{
		/* Make sure the slot is not in use */
		if (ndep->deName[0] != 0 && ndep->deName[0] != 0xE5)
			panic("msdosfs_createde: short name slot in use!");
		
		/* If it's a directory, make sure it's start cluster is non-zero */
		if ((ndep->deAttributes & ATTR_DIRECTORY) &&
			*(uint16_t *)(ndep->deStartCluster) == 0 &&
			*(uint16_t *)(ndep->deHighClust) == 0)
		{
			panic("msdosfs_createde: directory with start cluster == 0");
		}
	}
	
	/* NOTE: DE_EXTERNALIZE does not set the name or lower case flags */
	bcopy(dep->de_Name, ndep->deName, 11);
	ndep->deLowerCase = dep->de_LowerCase;
	DE_EXTERNALIZE(ndep, dep);

	/*
	 * Now write the Win95 long name
	 */
	if (long_count > 0) {
		u_int8_t chksum = msdosfs_winChksum(ndep->deName);
		u_int16_t ucfn[WIN_MAXLEN];
		size_t unichars;
		int cnt = 1;
		
		/*
		 * Decode component name into Unicode
		 * NOTE: We should be using a "precompose" flag
		 */
		(void) utf8_decodestr((u_int8_t*)cnp->cn_nameptr, cnp->cn_namelen, ucfn,
					&unichars, sizeof(ucfn), 0,
					UTF_PRECOMPOSED|UTF_SFM_CONVERSIONS);
		unichars /= 2; /* bytes to chars */

		while (long_count-- > 0) {
			if (!(offset & pmp->pm_crbomask)) {
				error = (int)buf_bdwrite(bp);
				if (error)
					return error;

				offset -= sizeof(struct dosdirentry);
				error = msdosfs_pcbmap(ddep,
							de_cluster(pmp, offset), 1,
							&bn, 0, &blsize);
				if (error)
					return error;

				error = (int)buf_meta_bread(pmp->pm_devvp, bn, blsize,
					      vfs_context_ucred(context), &bp);
				if (error) {
					buf_brelse(bp);
					return error;
				}
				ndep = bptoep(pmp, bp, offset);
			} else {
				ndep--;
				offset -= sizeof(struct dosdirentry);
			}
			
			if (DEBUG)
			{
				/* Make sure the slot is not in use */
				if (ndep->deName[0] != 0 && ndep->deName[0] != 0xE5)
					panic("msdosfs_createde: long name slot in use!\n");
			}
			
			if (!msdosfs_unicode2winfn(ucfn, unichars, (struct winentry *)ndep, cnt++, chksum))
				break;
		}
	}

	error = (int)buf_bdwrite(bp);
	if (error)
		return error;

    ddep->de_flag |= DE_UPDATE;
    
	/*
	 * If they want us to return with the denode gotten.
	 */
	if (depp)
		error = msdosfs_deget(pmp, dirclust, diroffset, DETOV(ddep), cnp, depp, context);
	
	return error;
}

/*
 * Be sure a directory is empty except for "." and "..". Return 1 if empty,
 * return 0 if not empty or error.
 */
int msdosfs_dosdirempty(struct denode *dep, vfs_context_t context)
{
	uint32_t blsize;
	int error;
	uint32_t cn;
	daddr64_t bn;
	struct buf *bp;
	struct msdosfsmount *pmp = dep->de_pmp;
	struct dosdirentry *dentp;
	char *bdata;

	/*
	 * Since the filesize field in directory entries for a directory is
	 * zero, we just have to feel our way through the directory until
	 * we hit end of file.
	 */
	for (cn = 0;; cn++) {
		if ((error = msdosfs_pcbmap(dep, cn, 1, &bn, NULL, &blsize)) != 0) {
			if (error == E2BIG)
				return (1);	/* it's empty */
			return (0);
		}
		error = (int)buf_meta_bread(pmp->pm_devvp, bn, blsize, vfs_context_ucred(context), &bp);
		if (error) {
			buf_brelse(bp);
			return (0);
		}
		bdata = (char *)buf_dataptr(bp);

		for (dentp = (struct dosdirentry *)bdata;
		     (char *)dentp < bdata + blsize;
		     dentp++)
		{
			if (dentp->deName[0] != SLOT_DELETED &&
			    (dentp->deAttributes & ATTR_VOLUME) == 0) {
				/*
				 * In dos directories an entry whose name
				 * starts with SLOT_EMPTY (0) starts the
				 * beginning of the unused part of the
				 * directory, so we can just return that it
				 * is empty.
				 */
				if (dentp->deName[0] == SLOT_EMPTY) {
					buf_brelse(bp);
					return 1;
				}
				/*
				 * Any names other than "." and ".." in a
				 * directory mean it is not empty.
				 */
				if (bcmp(dentp->deName, ".          ", SHORT_NAME_LEN) &&
				    bcmp(dentp->deName, "..         ", SHORT_NAME_LEN)) {
					buf_brelse(bp);
					return (0);	/* not empty */
				}
			}
		}
		buf_brelse(bp);
	}

	return 1;
}

/*
 * Check to see if the directory described by target is in some
 * subdirectory of source.  This prevents something like the following from
 * succeeding and leaving a bunch or files and directories orphaned:
 *   mv /a/b/c /a/b/c/d/e/f
 * where c and f are directories.
 *
 * source - the inode for /a/b/c (the directory being moved)
 * target - the inode for /a/b/c/d/e/f (the destination parent directory)
 *
 * Returns 0 if target is NOT a subdirectory of source.
 * Otherwise returns a non-zero error number.
 *
 * This routine works by following the chain of ".." entries starting at
 * target until we reach source, or the root of the volume.  It reads the
 * directory entries directly, not via directory denodes.  It assumes that
 * the caller has prevented the hierarchy from changing.  This routine takes
 * no locks.
 */
int msdosfs_doscheckpath(struct denode *source, struct denode *target, vfs_context_t context)
{
	daddr64_t scn, source_scn;
	struct msdosfsmount *pmp;
	struct dosdirentry *ep;
	struct buf *bp = NULL;
	int error = 0;
	int isFAT32;
	char *bdata;

	pmp = target->de_pmp;
	isFAT32 = FAT32(pmp);
	scn = target->de_StartCluster;
	if (scn == pmp->pm_rootdirblk)
		scn = 0;
	source_scn = source->de_StartCluster;
	/* Assumes the caller has prevented the source from being the root */
	
	/* scn == 0 means the root directory */
	while (scn != 0)
	{
		if (scn == source_scn)
			return EINVAL;
		
		/* Read the first cluster of the current directory */
		error = (int)buf_meta_bread(pmp->pm_devvp, cntobn(pmp, scn),
			pmp->pm_bpcluster, vfs_context_ucred(context), &bp);
		if (error) break;
		bdata = (char *)buf_dataptr(bp);
		
		/* Point to the second entry, which should be the ".." entry */
		ep = (struct dosdirentry *) bdata + 1;
		if ((ep->deAttributes & ATTR_DIRECTORY) == 0 ||
		    bcmp(ep->deName, "..         ", SHORT_NAME_LEN) != 0)
		{
			error = ENOTDIR;
			break;
		}
		
		/* Get the cluster number from the ".." entry */
		scn = getuint16(ep->deStartCluster);
		if (isFAT32)
			scn |= getuint16(ep->deHighClust) << 16;
		
		/*
		 * When ".." points to the root, the cluster number should be 0.
		 * On FAT32, it's conceivable that an implementation might incorrectly
		 * have set the cluster number to the first cluster of the root.
		 * If so, we need to exit.  For FAT12 and FAT16, pm_rootdirblk will be
		 * 0, in which case this is just a slightly early exit of the loop
		 * (the while condition would be false the next time through).
		 */
		if (scn == pmp->pm_rootdirblk)
			break;
		
		/* Release the block we read above */
		buf_brelse(bp);
		bp = NULL;
	}
	
	if (bp)
		buf_brelse(bp);
	if (error == ENOTDIR)
		printf("msdosfs_doscheckpath(): .. not a directory?\n");
	return (error);
}

/*
 * Read in the disk block containing the directory entry (dirclu, dirofs)
 * and return the address of the buf header, and the address of the
 * directory entry within the block.
 */
int msdosfs_readep(struct msdosfsmount *pmp,
	uint32_t dirclust, uint32_t diroffset,
	struct buf **bpp, struct dosdirentry **epp, vfs_context_t context)
{
	int error;
	daddr64_t bn;
	int blsize;

	/*
	 * Handle the special case of the root directory.  If there is a volume
	 * label entry, then get that.  Otherwise, return an error.
	 */
	if ((dirclust == MSDOSFSROOT
	     || (FAT32(pmp) && dirclust == pmp->pm_rootdirblk))
	    && diroffset == MSDOSFSROOT_OFS)
	{
		if (pmp->pm_label_cluster == CLUST_EOFE)
			return EIO;
		else
		{
			dirclust = pmp->pm_label_cluster;
			diroffset = pmp->pm_label_offset;
		}
	}
        
	/*
	 * Sanity check the diroffset.  It should be a multiple of the directory
	 * entry size (i.e. a multiple of 32).
	 */
	if (diroffset % sizeof(struct dosdirentry))
	{
		printf("msdosfs: msdosfs_readep: invalid diroffset (%u)\n", diroffset);
		return EIO;
	}
	
	/* Figure out which block contains the directory entry. */
	blsize = pmp->pm_bpcluster;
	if (dirclust == MSDOSFSROOT
	    && de_blk(pmp, diroffset + blsize) > pmp->pm_rootdirsize)
	{
		blsize = de_bn2off(pmp, pmp->pm_rootdirsize) & pmp->pm_crbomask;
	}
	bn = detobn(pmp, dirclust, diroffset);
	
	/*
	 * We occasionally get panic reports that appear to be caused because
	 * blsize == 0.  I haven't figured out what can cause that, so try
	 * logging some information that might help, and return an error.
	 */
	if (blsize == 0)
	{
		printf("msdosfs: msdosfs_readep: blsize==0; pm_fatmask=0x%x, pm_bpcluster=0x%x, "
			"pm_BlockSize=0x%x, pm_PhysBlockSize=0x%x, pm_BlocksPerSec=0x%x, "
			"pm_cnshift=0x%x, pm_bnshift=0x%x, pm_crbomask=0x%x, "
			"pm_rootdirblk=0x%x, pm_rootdirsize=0x%x, "
			"pm_label_cluster=0x%x, pm_label_offset=0x%x, "
			"dirclust=0x%x, diroffset=0x%x, bn=0x%llx\n",
			pmp->pm_fatmask, pmp->pm_bpcluster,
			pmp->pm_BlockSize, pmp->pm_PhysBlockSize, pmp->pm_BlocksPerSec,
			pmp->pm_cnshift, pmp->pm_bnshift, pmp->pm_crbomask,
			pmp->pm_rootdirblk, pmp->pm_rootdirsize,
			pmp->pm_label_cluster, pmp->pm_label_offset,
			dirclust, diroffset, bn);
		return EIO;
	}
	
	/* Read the block containing the directory entry, then return the requested entry. */
	if ((error = (int)buf_meta_bread(pmp->pm_devvp, bn, blsize, vfs_context_ucred(context), bpp)) != 0)
	{
		buf_brelse(*bpp);
		*bpp = NULL;
		return (error);
	}
	if (epp)
		*epp = bptoep(pmp, *bpp, diroffset);
	return (0);
}


/*
 * Remove a directory entry. At this point the file represented by the
 * directory entry to be removed is still full length until noone has it
 * open.  When the file no longer being used msdosfs_vnop_inactive() is called
 * and will truncate the file to 0 length.  When the vnode containing the
 * denode is needed for some other purpose by VFS it will call
 * msdosfs_vnop_reclaim() which will remove the denode from the denode cache.
 */
int msdosfs_removede(struct denode *pdep, uint32_t offset, vfs_context_t context)
{
    int error;
    struct dosdirentry *ep;
    struct buf *bp;
    daddr64_t bn;
    uint32_t blsize;
    struct msdosfsmount *pmp = pdep->de_pmp;
	uint32_t cur_offset;
	
	cur_offset = offset;
    cur_offset += sizeof(struct dosdirentry);
    do {
        cur_offset -= sizeof(struct dosdirentry);
        error = msdosfs_pcbmap(pdep, de_cluster(pmp, cur_offset), 1, &bn, NULL, &blsize);
        if (error)
            return error;
        error = (int)buf_meta_bread(pmp->pm_devvp, bn, blsize, vfs_context_ucred(context), &bp);
        if (error) {
            buf_brelse(bp);
            return error;
        }
        ep = bptoep(pmp, bp, cur_offset);
		
        /*
         * Stop deleting long name entries when we find some other short
         * name entry.
         */
        if (ep->deAttributes != ATTR_WIN95
            && cur_offset != offset) {
            buf_brelse(bp);
            break;
        }
        cur_offset += sizeof(struct dosdirentry);
        while (1) {
            /*
             * We are a bit agressive here in that we delete any Win95
             * entries preceding this entry, not just the ones we "own".
             * Since these presumably aren't valid anyway,
             * there should be no harm.
             */
            cur_offset -= sizeof(struct dosdirentry);
            ep--->deName[0] = SLOT_DELETED;
            if ((cur_offset & pmp->pm_crbomask) == 0
                || ep->deAttributes != ATTR_WIN95)
                break;
        }
		error = (int)buf_bdwrite(bp);
        if (error)
            return error;
    } while ((cur_offset & pmp->pm_crbomask) == 0
             && cur_offset);
    pdep->de_flag |= DE_UPDATE;
	
    return error;
}

/*
 * Create a unique DOS name in dvp
 */
int msdosfs_uniqdosname(
	struct denode *dep,
	struct componentname *cnp,
	u_char *cp,
	u_int8_t *lower_case,
	vfs_context_t context)
{
	struct msdosfsmount *pmp = dep->de_pmp;
	struct dosdirentry *dentp;
	int gen;
	uint32_t blsize;
	uint32_t cn;
	daddr64_t bn;
	struct buf *bp;
	int error;
	u_int16_t ucfn[WIN_MAXLEN];
	size_t unichars;
	char *bdata;
	
	/*
	 * Decode component name into Unicode
	 */
	(void) utf8_decodestr((u_int8_t*)cnp->cn_nameptr, cnp->cn_namelen, ucfn,
				&unichars, sizeof(ucfn), 0, UTF_PRECOMPOSED);
	unichars /= 2; /* bytes to chars */
	
	for (gen = 1;; gen++) {
		/*
		 * Generate DOS name with generation number
		 */
		if (!msdosfs_unicode2dosfn(ucfn, cp, unichars, gen, lower_case))
			return gen == 1 ? ENAMETOOLONG : EEXIST;

		/* This function calls msdosfs_unicode2dosfn to get short name for given
	 	 * file name and does not use long names any further.  Therefore 
		 * we do not call mac2sfmfn to convert Mac Unicode to SFM Unicode 
	 	 */
		 
		/*
		 * Now look for a dir entry with this exact name
		 */
		for (cn = error = 0; !error; cn++) {
			if ((error = msdosfs_pcbmap(dep, cn, 1, &bn, NULL, &blsize)) != 0) {
				if (error == E2BIG)	/* EOF reached and not found */
					return 0;
				return error;
			}
			error = (int)buf_meta_bread(pmp->pm_devvp, bn, blsize, vfs_context_ucred(context), &bp);
			if (error) {
				buf_brelse(bp);
				return error;
			}
			bdata = (char *)buf_dataptr(bp);

			for (dentp = (struct dosdirentry *)bdata;
			     (char *)dentp < bdata + blsize;
			     dentp++) {
				if (dentp->deName[0] == SLOT_EMPTY) {
					/*
					 * Last used entry and not found
					 */
					buf_brelse(bp);
					return 0;
				}
				/*
				 * Ignore volume labels and Win95 entries
				 */
				if (dentp->deAttributes & ATTR_VOLUME)
					continue;
				if (!bcmp(dentp->deName, cp, SHORT_NAME_LEN)) {
					error = EEXIST;
					break;
				}
			}
			buf_brelse(bp);
		}
	}
}

/*
 * Find room in a directory to create the entries for a given name.
 *
 * Inputs:
 *	dep			directory to search for free/unused entries
 *	cnp			the name to be created (used to determine number of slots needed).
 *
 * Outputs:
 *	lower_case	The case ("NT") flags for the new name
 *	offset		Byte offset from start of directory where short name enty goes
 *	long_count	Number of entries needed for long name entries
 */
int msdosfs_findslots(
	struct denode *dep,
	struct componentname *cnp,
	u_int8_t *lower_case,
	uint32_t *offset,
	uint32_t *long_count,
	vfs_context_t context)
{
	int error;
	u_char dosfilename[12];
	u_int16_t ucfn[WIN_MAXLEN];
	size_t unichars;
	int wincnt=0;	/* Number of consecutive entries needed for long name + dir entry */
	int slotcount;	/* Number of consecutive entries found so far */
	uint32_t diroff;	/* Byte offset of entry from start of directory */
	unsigned blkoff;	/* Byte offset of entry from start of block */
	int frcn;	/* File (directory) relative cluster number */
	daddr64_t bn;	/* Physical disk block number */
	uint32_t blsize;	/* Size of directory cluster, in bytes */
	struct dosdirentry *entry;
	struct msdosfsmount *pmp;
	struct buf *bp;
	char *bdata;

	pmp = dep->de_pmp;

	/*
	 * Decode name into UCS-2 (Unicode)
	 */
	(void) utf8_decodestr((u_int8_t*)cnp->cn_nameptr, cnp->cn_namelen, ucfn, &unichars,
							sizeof(ucfn), 0, UTF_PRECOMPOSED);
	unichars /= 2; /* bytes to chars */

	/*
	 * Determine the number of consecutive directory entries we'll need.
	 */
	switch (msdosfs_unicode2dosfn(ucfn, dosfilename, unichars, 0, lower_case)) {
	case 0:
			/*
			 * The name is syntactically invalid.  Normally, we'd return EINVAL,
			 * but ENAMETOOLONG makes it clear that the name is the problem (and
			 * allows Carbon to return a more meaningful error).
			 */
			return (ENAMETOOLONG);
	case 1:
			/*
			 * The name is already a short, DOS name, so no long name entries needed.
			 */
			wincnt = 1;
			break;
	case 2:
	case 3:
			wincnt = msdosfs_winSlotCnt(ucfn, unichars) + 1;
			break;
	}

	/* This function calls msdosfs_unicode2dosfn to get short name for given
	 * file name and does not use long names any further.  Therefore
	 * we do not call mac2sfmfn to convert Mac Unicode to SFM Unicode 
	 */

	/*
	 * Look for some consecutive unused directory entries.
	 */
	slotcount = 0;		/* None found yet. */
	
	/* The outer loop ranges over the clusters in the directory. */
	diroff = 0;
	for (frcn = 0; ; frcn++) {
		error = msdosfs_pcbmap(dep, frcn, 1, &bn, NULL, &blsize);
		if (error) {
			if (error == E2BIG)
				break;
			return (error);
		}
		error = (int)buf_meta_bread(pmp->pm_devvp, bn, blsize, vfs_context_ucred(context), &bp);
		if (error) {
			buf_brelse(bp);
			return (error);
		}
		bdata = (char *)buf_dataptr(bp);
	
		/* Loop over entries in the cluster. */
		for (blkoff = 0; blkoff < blsize;
			 blkoff += sizeof(struct dosdirentry),
			 diroff += sizeof(struct dosdirentry))
		{
			entry = (struct dosdirentry *)(bdata + blkoff);
					
			if (entry->deName[0] == SLOT_EMPTY ||
				entry->deName[0] == SLOT_DELETED)
			{
				slotcount++;
				if (slotcount == wincnt) {
					/* Found enough space! */
					*offset = diroff;
					goto found;
				}
			} else {
				/* Empty space wasn't big enough, so forget about it. */
				slotcount = 0;
			}
		}
		buf_brelse(bp);
		bp = NULL;
	}
	/*
	 * Fix up the slot description to point to where we would put the
	 * DOS entry (with Win95 long name entries before that).  If we
	 * would need to grow the directory, then the offset will be greater
	 * than or equal to the size of the directory.
	 */
found:        
	if (wincnt > slotcount) {
		/*
		 * If we get here, we hit the end of the directory without finding
		 * enough consecutive slots.  "slotcount" is the number of free slots
		 * at the end of the last cluster.  "diroff" is the size of the
		 * directory, in bytes.
		 *
		 * Note the "- 1" below; that's because the returned offset is the
		 * offset of the last slot that would be used (for the short name
		 * entry); without subtracting one, we'd end up pointing to the slot
		 * immediately past the last one being used.
		 */
		*offset = diroff + sizeof(struct dosdirentry) * (wincnt - slotcount - 1);
	}
	*long_count = wincnt - 1;

	if (bp)
		buf_brelse(bp);

	return 0;
}


/*
 * Write all modified blocks for a given directory.
 */
int msdosfs_dir_flush(struct denode *dep, int sync)
{
	int error;
	uint32_t frcn;	/* File (directory) relative cluster number */
	uint32_t blsize;	/* Size of directory block */
	daddr64_t bn;	/* Device block number */
	vnode_t devvp = dep->de_pmp->pm_devvp;
	buf_t bp;

	if (dep->de_refcnt <= 0)
	{
		/* Don't bother updating a deleted directory */
		return 0;
	}
	
	for (frcn=0; ; frcn++)
	{
		error = msdosfs_pcbmap(dep, frcn, 1, &bn, NULL, &blsize);
		if (error)
		{
			if (error == E2BIG)
				break;
			return error;
		}
		
		bp = buf_getblk(devvp, bn, blsize, 0, 0, BLK_META|BLK_ONLYVALID);
		if (bp)
		{
			if (buf_flags(bp) & B_DELWRI)
			{
				if (sync)
					buf_bwrite(bp);
				else
					buf_bawrite(bp);
			}
			else
			{
				buf_brelse(bp);
			}
		}
	}
	
	return 0;
}


/*
 * Invalidate all blocks for a given directory.
 */
int msdosfs_dir_invalidate(struct denode *dep)
{
	int error;
	uint32_t frcn;	/* File (directory) relative cluster number */
	uint32_t blsize;	/* Size of directory block */
	daddr64_t bn;	/* Device block number */
	vnode_t devvp = dep->de_pmp->pm_devvp;

	for (frcn=0; ; frcn++)
	{
		error = msdosfs_pcbmap(dep, frcn, 1, &bn, NULL, &blsize);
		if (error)
		{
			if (error == E2BIG)
				break;
			return error;
		}
		
		(void) buf_invalblkno(devvp, bn, BUF_WAIT);
	}
	
	return 0;
}
