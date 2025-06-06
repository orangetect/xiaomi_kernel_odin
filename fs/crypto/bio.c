// SPDX-License-Identifier: GPL-2.0
/*
 * Utility functions for file contents encryption/decryption on
 * block device-based filesystems.
 *
 * Copyright (C) 2015, Google, Inc.
 * Copyright (C) 2015, Motorola Mobility
 */

#include <linux/pagemap.h>
#include <linux/module.h>
#include <linux/bio.h>
#include <linux/namei.h>
#include "fscrypt_private.h"

/**
 * fscrypt_decrypt_bio() - decrypt the contents of a bio
 * @bio: the bio to decrypt
 *
 * Decrypt the contents of a "read" bio following successful completion of the
 * underlying disk read.  The bio must be reading a whole number of blocks of an
 * encrypted file directly into the page cache.  If the bio is reading the
 * ciphertext into bounce pages instead of the page cache (for example, because
 * the file is also compressed, so decompression is required after decryption),
 * then this function isn't applicable.  This function may sleep, so it must be
 * called from a workqueue rather than from the bio's bi_end_io callback.
 *
 * This function sets PG_error on any pages that contain any blocks that failed
 * to be decrypted.  The filesystem must not mark such pages uptodate.
 */
void fscrypt_decrypt_bio(struct bio *bio)
{
	struct bio_vec *bv;
	struct bvec_iter_all iter_all;

	bio_for_each_segment_all(bv, bio, iter_all) {
		struct page *page = bv->bv_page;
		int ret = fscrypt_decrypt_pagecache_blocks(page, bv->bv_len,
							   bv->bv_offset);
		if (ret)
			SetPageError(page);
	}
}
EXPORT_SYMBOL(fscrypt_decrypt_bio);

static int fscrypt_zeroout_range_inlinecrypt(const struct inode *inode,
					     pgoff_t lblk,
					     sector_t pblk, unsigned int len)
{
	const unsigned int blockbits = inode->i_blkbits;
	const unsigned int blocks_per_page_bits = PAGE_SHIFT - blockbits;
	const unsigned int blocks_per_page = 1 << blocks_per_page_bits;
	unsigned int i;
	struct bio *bio;
	int ret, err;

	/* This always succeeds since __GFP_DIRECT_RECLAIM is set. */
	bio = bio_alloc(GFP_NOFS, BIO_MAX_PAGES);

	do {
		bio_set_dev(bio, inode->i_sb->s_bdev);
		bio->bi_iter.bi_sector = pblk << (blockbits - 9);
		bio_set_op_attrs(bio, REQ_OP_WRITE, 0);
		fscrypt_set_bio_crypt_ctx(bio, inode, lblk, GFP_NOFS);

		i = 0;
		do {
			unsigned int blocks_this_page =
				min(len, blocks_per_page);
			unsigned int bytes_this_page =
				blocks_this_page << blockbits;

			ret = bio_add_page(bio, ZERO_PAGE(0),
					   bytes_this_page, 0);
			if (WARN_ON_ONCE(ret != bytes_this_page)) {
				err = -EIO;
				goto out;
			}
			lblk += blocks_this_page;
			pblk += blocks_this_page;
			len -= blocks_this_page;
		} while (++i != BIO_MAX_PAGES && len != 0 &&
			 fscrypt_mergeable_bio(bio, inode, lblk));

		err = submit_bio_wait(bio);
		if (err)
			goto out;
		bio_reset(bio);
	} while (len != 0);
	err = 0;
out:
	bio_put(bio);
	return err;
}

/**
 * fscrypt_zeroout_range() - zero out a range of blocks in an encrypted file
 * @inode: the file's inode
 * @lblk: the first file logical block to zero out
 * @pblk: the first filesystem physical block to zero out
 * @len: number of blocks to zero out
 *
 * Zero out filesystem blocks in an encrypted regular file on-disk, i.e. write
 * ciphertext blocks which decrypt to the all-zeroes block.  The blocks must be
 * both logically and physically contiguous.  It's also assumed that the
 * filesystem only uses a single block device, ->s_bdev.
 *
 * Note that since each block uses a different IV, this involves writing a
 * different ciphertext to each block; we can't simply reuse the same one.
 *
 * Return: 0 on success; -errno on failure.
 */
int fscrypt_zeroout_range(const struct inode *inode, pgoff_t lblk,
			  sector_t pblk, unsigned int len)
{
	const unsigned int blockbits = inode->i_blkbits;
	const unsigned int blocksize = 1 << blockbits;
	const unsigned int blocks_per_page_bits = PAGE_SHIFT - blockbits;
	const unsigned int blocks_per_page = 1 << blocks_per_page_bits;
	struct page *pages[16]; /* write up to 16 pages at a time */
	unsigned int nr_pages;
	unsigned int i;
	unsigned int offset;
	struct bio *bio;
	int ret, err;

	if (len == 0)
		return 0;

	if (fscrypt_inode_uses_inline_crypto(inode))
		return fscrypt_zeroout_range_inlinecrypt(inode, lblk, pblk,
							 len);

	BUILD_BUG_ON(ARRAY_SIZE(pages) > BIO_MAX_PAGES);
	nr_pages = min_t(unsigned int, ARRAY_SIZE(pages),
			 (len + blocks_per_page - 1) >> blocks_per_page_bits);

	/*
	 * We need at least one page for ciphertext.  Allocate the first one
	 * from a mempool, with __GFP_DIRECT_RECLAIM set so that it can't fail.
	 *
	 * Any additional page allocations are allowed to fail, as they only
	 * help performance, and waiting on the mempool for them could deadlock.
	 */
	for (i = 0; i < nr_pages; i++) {
		pages[i] = fscrypt_alloc_bounce_page(i == 0 ? GFP_NOFS :
						     GFP_NOWAIT | __GFP_NOWARN);
		if (!pages[i])
			break;
	}
	nr_pages = i;
	if (WARN_ON_ONCE(nr_pages <= 0))
		return -EINVAL;

	/* This always succeeds since __GFP_DIRECT_RECLAIM is set. */
	bio = bio_alloc(GFP_NOFS, nr_pages);

	do {
		bio_set_dev(bio, inode->i_sb->s_bdev);
		bio->bi_iter.bi_sector = pblk << (blockbits - 9);
		bio_set_op_attrs(bio, REQ_OP_WRITE, 0);

		i = 0;
		offset = 0;
		do {
			err = fscrypt_crypt_block(inode, FS_ENCRYPT, lblk,
						  ZERO_PAGE(0), pages[i],
						  blocksize, offset, GFP_NOFS);
			if (err)
				goto out;
			lblk++;
			pblk++;
			len--;
			offset += blocksize;
			if (offset == PAGE_SIZE || len == 0) {
				ret = bio_add_page(bio, pages[i++], offset, 0);
				if (WARN_ON_ONCE(ret != offset)) {
					err = -EIO;
					goto out;
				}
				offset = 0;
			}
		} while (i != nr_pages && len != 0);

		err = submit_bio_wait(bio);
		if (err)
			goto out;
		bio_reset(bio);
	} while (len != 0);
	err = 0;
out:
	bio_put(bio);
	for (i = 0; i < nr_pages; i++)
		fscrypt_free_bounce_page(pages[i]);
	return err;
}
EXPORT_SYMBOL(fscrypt_zeroout_range);
