/*
 * TODO impl persistent b+tree slice sequence
 */

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __unix__
	#include <fcntl.h>
	#include <unistd.h>
	#include <sys/mman.h>
#else
	#error TODO mmap for non-unix systems
#endif

#include "st.h"

#define HIGH_WATER 4096

enum blktype { LARGE, LARGE_MMAP, SMALL };
struct block {
	enum blktype type;
	int refs; // only for LARGE blocks
	char *data; // owned by the block
	size_t len;
};

#define SLICESIZE sizeof(struct slice)
#define SZSIZE sizeof(size_t)
struct slice {
	struct block *blk;
	size_t offset;
};

#define NODESIZE 128
#define PER_B (SZSIZE + sizeof(void *))
#define B (NODESIZE / PER_B)
struct inner {
	size_t spans[B];
	// TODO check for aliasing UB
	struct inner *children[B];
};

#define PER_BL (SZSIZE + SLICESIZE)
#define BL (NODESIZE / PER_BL)
struct leaf {
	size_t spans[BL];
	struct slice slices[BL];
};

#define LVAL(leaf, i) (leaf->spans[i] != ULONG_MAX ? &leaf->slices[i] : NULL)

struct slicetable {
	struct inner *root;
	unsigned height;
};

static void print_node(unsigned level, const struct inner *node);

/* blocks */

static struct block *new_block(const char *data, size_t len)
{
	struct block *new = malloc(sizeof *new);
	new->len = len;
	new->data = malloc(MAX(HIGH_WATER, len));
	memcpy(new->data, data, len);

	if(len > HIGH_WATER) {
		new->type = LARGE;
		new->refs = 1;
	} else {
		new->type = SMALL;
		new->refs = 0;
	}
	return new;
}

static void drop_block(struct block *block)
{
	switch(block->type) {
	case SMALL:
		free(block->data);
		free(block);
		break;
	case LARGE_MMAP:
		if(--block->refs == 0) {
			munmap(block->data, block->len);
			free(block);
		}
		break;
	case LARGE:
		if(--block->refs == 0) {
			free(block->data);
			free(block);
		}
		break;
	}
}

static void block_insert(struct block *block, size_t offset, const char *data,
			 size_t len)
{
	assert(block->type == SMALL);
	assert(offset <= block->len);
	// we maintain the invariant that SMALL blocks->len <= HIGH_WATER
	if(block->len + len > HIGH_WATER)
		block->data = realloc(block->data, block->len + len);
	char *start = block->data + offset;
	memmove(start + len, start, block->len - offset);
	memcpy(start, data, len);
	block->len += len;
	if(block->len > HIGH_WATER) {
		block->type = LARGE;
		block->refs = 1;
	}
}

static void block_delete(struct block *block, size_t offset, size_t len)
{
	assert(block->type == SMALL);
	assert(offset + len <= block->len);
	char *start = block->data + offset;
	memmove(start, start + len, block->len - offset - len);
	block->len -= len;
}

static struct slice new_slice(char *data, size_t len)
{
	struct block *block = new_block(data, len);
	struct slice s = (struct slice){ .blk = block, .offset = 0 };
	return s;
}

/* tree utilities */

static void inner_clrslots(struct inner *node, unsigned from, unsigned to)
{
	assert(to <= B);
	for(unsigned i = from; i < to; i++)
		node->spans[i] = ULONG_MAX;

	memset(&node->children[from], 0, (to - from) * sizeof(void *));
}

static void leaf_clrslots(struct leaf *leaf, unsigned from, unsigned to)
{
	assert(to <= BL);
	for(unsigned i = from; i < to; i++)
		leaf->spans[i] = ULONG_MAX;
}

static struct inner *new_inner(void)
{
	struct inner *node = malloc(sizeof *node);
	inner_clrslots(node, 0, B);
	return node;
}

static struct leaf *new_leaf(void)
{
	struct leaf *leaf = malloc(sizeof *leaf);
	leaf_clrslots(leaf, 0, BL);
	return leaf;
}

// sums the spans of entries in node, up to fill
static size_t inner_sum(const struct inner *node, unsigned fill)
{
	size_t sum = 0;
	for(unsigned i = 0; i < fill; i++)
		sum += node->spans[i];
	return sum;
}

// sums the spans of entries in leaf, up to fill
static size_t leaf_sum(const struct leaf *leaf, unsigned fill)
{
	size_t sum = 0;
	for(unsigned i = 0; i < fill; i++)
		sum += leaf->spans[i];
	return sum;
}

// returns index of the first key spanning the search key in node
// key contains the offset at the end
static unsigned inner_offset(const struct inner *node, size_t *key)
{
	unsigned i = 0;
	while(*key && *key >= node->spans[i]) {
		*key -= node->spans[i];
		i++;
#ifndef NDEBUG
		if(i == B) assert(!*key); // off end is permitted, terminates loop
#endif
	}
	return i;
}

// returns index of the first key spanning the search key in leaf
// key contains the offset at the end
static unsigned leaf_offset(const struct leaf *leaf, size_t *key)
{
	unsigned i = 0;
	while(*key && *key >= leaf->spans[i]) {
		*key -= leaf->spans[i];
		i++;
#ifndef NDEBUG
		if(i == BL) assert(!*key); // off end is permitted, terminates loop
#endif
	}
	return i;
}

// count the number of live entries in node counting up from START
static unsigned inner_fill(const struct inner *node, unsigned start)
{
	unsigned i;
	for(i = start; i < B; i++)
		if(!node->children[i])
			break;
	return i;
}

// count the number of live entries in leaf counting up from start
static unsigned leaf_fill(const struct leaf *leaf, unsigned start)
{
	unsigned i;
	for(i = start; i < BL; i++)
		if(!LVAL(leaf, i))
			break;
	return i;
}

/* simple */

SliceTable *st_new(void)
{
	SliceTable *st = malloc(sizeof *st);
	st->root = (struct inner *)new_leaf();
	st->height = 1;
	return st;
}

SliceTable *st_new_from_file(const char *path)
{
	int fd = open(path, O_RDONLY);
	if(!fd)
		return NULL;
	size_t len = lseek(fd, 0, SEEK_END);
	if(!len)
		return st_new(); // mmap cannot handle 0-length mappings

	enum blktype type;
	void *data;
	if(len <= HIGH_WATER) {
		data = malloc(HIGH_WATER);
		lseek(fd, 0, SEEK_SET);
		// FIXME
		if(read(fd, data, len) != len) {
			free(data);
			return NULL;
		}
		type = SMALL;
	} else {
		data = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
		close(fd);
		if(data == MAP_FAILED)
			return NULL;
		type = LARGE_MMAP;
	}

	SliceTable *st = malloc(sizeof *st);
	struct block *init = malloc(sizeof(struct block));
	*init = (struct block){ type, .refs = 1, data, len };

	struct leaf *leaf = new_leaf();
	leaf->slices[0] = (struct slice){ .blk = init, .offset = 0 };
	leaf->spans[0] = len;
	st->height = 1;
	st->root = (struct inner *)leaf;
	return st;
}

void st_free(SliceTable *st)
{
	// TODO impl
	free(st);
}

size_t st_size(SliceTable *st)
{
	struct inner *root = st->root;
	if(st->height == 1) {
		return leaf_sum((struct leaf *)root, leaf_fill((struct leaf *)root, 0));
	} else
		return inner_sum(root, inner_fill(root, 0));
}

/* editing utilities */

void demote_slice(struct slice *slice, size_t span) {
	struct block *new = new_block(slice->blk->data + slice->offset, span);
	drop_block(slice->blk);
	slice->blk = new;
	slice->offset = 0;
}

void slice_insert(struct slice *slice, size_t offset, char *data, size_t len,
				size_t *span)
{
	if(*span <= HIGH_WATER && slice->blk->type != SMALL)
		demote_slice(slice, *span);
	block_insert(slice->blk, offset, data, len);
	*span += len;
}

void slice_delete(struct slice *slice, size_t offset, size_t len, size_t *span)
{
	if(*span <= HIGH_WATER && slice->blk->type != SMALL)
		demote_slice(slice, *span);
	block_delete(slice->blk, offset, len);
	*span -= len;
}

// merge slices in slices, returning the new number of slices
unsigned merge_slices(size_t spans[static 5], struct slice slices[static 5],
						unsigned fill)
{
	unsigned i = 1;
	while(i < fill) {
		if(spans[i] > HIGH_WATER)
			i += 2; // X|L__ to XL_|_
		else if(spans[i-1] <= HIGH_WATER) {
			// both are smaller S|S, i doesn't move
			slice_insert(&slices[i-1], spans[i-1],
						slices[i].blk->data + slices[i].offset, spans[i],
						&spans[i-1]);
			memmove(&spans[i], &spans[i+1], (fill - (i+1)) * SZSIZE);
			memmove(&slices[i], &slices[i+1], (fill - (i+1)) * SLICESIZE);
			fill--;
		} else
			i++; // L|S_ -> LS|_
	}
	return fill;
}

static struct inner *split_inner(struct inner *node, unsigned offset)
{
	struct inner *split = new_inner();
	unsigned count = B - offset;
	memcpy(&split->spans[0], &node->spans[offset], count * SZSIZE);
	memcpy(&split->children[0], &node->children[offset], count * sizeof(void *));
	inner_clrslots(node, offset, B);
	return split;
}

static struct leaf *split_leaf(struct leaf *leaf, unsigned offset)
{
	struct leaf *split = new_leaf();
	unsigned count = BL - offset;
	memcpy(&split->spans[0], &leaf->spans[offset], count * SZSIZE);
	memcpy(&split->slices[0], &leaf->slices[offset], count * SLICESIZE);
	leaf_clrslots(leaf, offset, BL);
	return split;
}

typedef long (*leaf_case)(struct leaf *leaf, size_t pos, long *span,
						struct leaf **split, size_t *splitsize, void *ctx);

static long edit_recurse(unsigned level, struct inner *root,
						size_t pos, long *span,
						leaf_case base_case, void *ctx,
						struct inner **split, size_t *splitsize)
{
	if(level == 1) {
		return base_case((void *)root, pos, span, (void*)split, splitsize, ctx);
	} else { // level > 1: inner node recursion
		struct inner *childsplit = NULL;
		size_t childsize = 0;
		unsigned i = inner_offset(root, &pos);
		long delta = edit_recurse(level - 1, root->children[i], pos, span,
								base_case, ctx, &childsplit, &childsize);
		st_dbg("applying upwards delta at level %u: %ld\n", level, delta);
		root->spans[i] += delta;
		// reset delta
		delta = *span;

		if(childsize) {
			if(childsplit) { // overflow: attempt to insert childsplit at i+1
				i++;
				unsigned fill = inner_fill(root, i);
				if(fill == B) {
					fill = B / 2 + (i > B / 2);
					*split = split_inner(root, fill);
					*splitsize = inner_sum(*split, B - fill);
					delta -= *splitsize;
					if(i > B / 2) {
						delta -= childsize;
						*splitsize += childsize;
						root = *split;
						i -= fill;
					}
				}
				memmove(&root->spans[i+1], &root->spans[i],
						(fill - i) * SZSIZE);
				memmove(&root->children[i+1], &root->children[i],
						(fill - i) * sizeof(void *));
				st_dbg("child split size from level %u: %lu\n", level-1, childsize);
				root->spans[i] = childsize;
				root->children[i] = childsplit;
			} else {
				// TODO impl handle underflow, childsize contains underfill.
				// propagate underflow through *splitsize when necessary
				// delta should increase after merge
			}
		}
		return delta;
	}
}

/* insertion */

// handle insertion within LARGE* slices
static long insert_within_slice(struct leaf *leaf, unsigned fill, 
							unsigned i, size_t off, struct slice *new,
							struct leaf **split, size_t *splitsize)
{
	size_t *left_span = &leaf->spans[i];
	struct slice *left = &leaf->slices[i];
	size_t right_span = *left_span - off;
	size_t newlen = new->blk->len; // NOTE: is is necessary to save this delta
	struct slice right = (struct slice) {
		.blk = left->blk,
		.offset = left->offset + off
	};
	left->blk->refs++;
	*left_span = off; // truncate
	// fill tmp
	size_t tmpspans[5];
	struct slice tmp[5];
	unsigned tmpfill = 0;
	if(i > 0) {
		tmpspans[tmpfill] = leaf->spans[i-1];
		tmp[tmpfill++] = leaf->slices[i-1];
	}
	tmpspans[tmpfill] = *left_span;
	tmp[tmpfill++] = *left;
	tmpspans[tmpfill] = newlen;
	tmp[tmpfill++] = *new;
	tmpspans[tmpfill] = right_span;
	tmp[tmpfill++] = right;

	if(i+1 < fill) {
		tmpspans[tmpfill] = leaf->spans[i+1];
		tmp[tmpfill++] = leaf->slices[i+1];
	}
	unsigned newfill = merge_slices(tmpspans, tmp, tmpfill);
	int delta = tmpfill - newfill;
	assert(delta <= 3); // [S][S1|Si|S2][S] -> [L][S], S1+S2 > HIGH_WATER
	st_dbg("merged %u nodes\n", delta);
	if(i > 0) {
		i--, left_span--, left--; // see above
	}
	unsigned realfill = fill - (delta-2);
	if(realfill <= BL) {
		size_t count = fill - (i + (tmpfill-2));
		memmove(left_span + newfill, left_span + (tmpfill-2), count * SZSIZE);
		memmove(left + newfill, left + (tmpfill-2), count * SLICESIZE);
		// when delta == 0, newfill exceeds tmpfill-2 and may overwrite
		// old slots, so we copy afterwards
		memcpy(left_span, tmpspans, newfill * SZSIZE);
		memcpy(left, tmp, newfill * SLICESIZE);
		if(delta > 0)
			leaf_clrslots(leaf, realfill, fill);
		return newlen;
	} else { // realfill > BL: leaf split, we have at most 2 new slices
		// TODO test this
		size_t spans[BL + 2];
		struct slice slices[BL + 2];
		// copy all data to temporary buffers and distribute
		// this is the simplest I can think of for handling all cases
		memcpy(spans, leaf->spans, i * SZSIZE);
		memcpy(slices, leaf->slices, i * SZSIZE);
		memcpy(&spans[i], tmpspans, newfill * SZSIZE);
		memcpy(&slices[i], tmp, newfill * SZSIZE);
		unsigned count = fill - (i + (tmpfill-2));
		memcpy(&spans[i+newfill], &leaf->spans[i+tmpfill-2], count * SZSIZE);
		memcpy(&slices[i+newfill], &leaf->slices[i+tmpfill-2], count * SZSIZE);
		struct leaf *right_split = new_leaf();
		// we must compute delta directly as merging moves the insert around
		size_t oldsum = leaf_sum(leaf, fill) + right_span;
		size_t new_leaf_fill = BL/2;
		size_t right_fill = realfill - BL/2;
		memcpy(leaf->spans, spans, new_leaf_fill * SZSIZE);
		memcpy(leaf->slices, slices, new_leaf_fill * SLICESIZE);
		memcpy(right_split->spans, &spans[new_leaf_fill], right_fill * SZSIZE);
		memcpy(right_split->slices,&slices[new_leaf_fill],right_fill*SLICESIZE);
		leaf_clrslots(leaf, new_leaf_fill, fill);
		leaf_clrslots(right_split, right_fill, BL);
		size_t newsum = leaf_sum(leaf, new_leaf_fill);
		*splitsize = leaf_sum(right_split, right_fill) + right_span;
		*split = right_split;
		return newsum - oldsum;
	}
}

static long insert_leaf(struct leaf *leaf, size_t pos, long *span,
						struct leaf **split, size_t *splitsize, void *data)
{
	unsigned i = leaf_offset(leaf, &pos);
	unsigned fill = leaf_fill(leaf, i);
	st_dbg("insertion: found slot %u, target fill %u\n", i, fill);
	size_t len = *span;
	long delta = len;
	// first try appending to slices[i-1], or mutating slices[i]
	if(pos == 0 && i > 0 && leaf->spans[i-1] <= HIGH_WATER) {
		size_t *ispan = &leaf->spans[i-1];
		slice_insert(&leaf->slices[i-1], *ispan, data, len, ispan);
		assert(i == fill || leaf->spans[i] > HIGH_WATER);
		return delta;
	}
	else if(i < fill && leaf->spans[i] <= HIGH_WATER) {
		assert(pos > 0 || i == 0 || leaf->spans[i-1] > HIGH_WATER);
		size_t *ispan = &leaf->spans[i];
		slice_insert(&leaf->slices[i], pos, data, len, ispan);
		return delta;
	}
	else if(pos == 0) { // insertion on boundary [L]|[L], no merging possible
		if(fill == BL) {
			fill = BL / 2 + (i > BL / 2);
			*split = split_leaf(leaf, fill);
			*splitsize = leaf_sum((struct leaf *)*split, BL - fill);
			delta -= *splitsize;
			if(i > BL / 2) {
				delta -= len;
				*splitsize += len;
				leaf = (struct leaf *)*split;
				i -= fill;
			}
		}
		memmove(&leaf->spans[i+1], &leaf->spans[i], (fill - i) * SZSIZE);
		memmove(&leaf->slices[i+1], &leaf->slices[i], (fill - i) * SLICESIZE);
		leaf->spans[i] = len;
		leaf->slices[i] = new_slice(data, len);
		return delta;
	} else {
		struct slice new = new_slice(data, len);
		return insert_within_slice(leaf, fill, i, pos, &new, split, splitsize);
	}
}

void st_insert(SliceTable *st, size_t pos, char *data, size_t len)
{
	if(len == 0)
		return;

	st_dbg("st_insert at pos %zd of len %zd\n", pos, len);
	struct inner *split = NULL;
	size_t splitsize;
	long span = (long)len;
	edit_recurse(st->height, st->root, pos, &span, insert_leaf, data,
				&split, &splitsize);
	// handle root underflow
	if(st->height > 1 && inner_fill(st->root, 0) == 1) {
		st->root = &st->root[0];
		st->height--;
	}
	// handle root split
	if(split) {
		st_dbg("allocating new root\n");
		struct inner *newroot = new_inner();
		newroot->spans[0] = st_size(st);
		newroot->children[0] = st->root;
		newroot->spans[1] = splitsize;
		newroot->children[1] = split;
		st->root = newroot;
		st->height++;
	}
}

/* deletion */

static unsigned delete_within_slice(struct leaf *leaf, unsigned fill, unsigned i,
								size_t off, size_t len)
{
	size_t *slice_span = &leaf->spans[i];
	struct slice *slice = &leaf->slices[i];
	if(*slice_span <= HIGH_WATER) {
		slice_delete(slice, off, len, slice_span);
		return fill;
	} else {
		size_t new_right_span = *slice_span - off - len;
		struct slice new_right = (struct slice){
			.blk = slice->blk,
			.offset = slice->offset + off + len
		};
		slice->blk->refs++;
		*slice_span = off; // truncate slice

		size_t tmpspans[5];
		struct slice tmp[5];
		unsigned tmpfill = 0;
		if(i > 0) {
			tmpspans[tmpfill] = leaf->spans[i-1];
			tmp[tmpfill++] = leaf->slices[i-1];
		}
		tmpspans[tmpfill] = *slice_span;
		tmp[tmpfill++] = *slice;
		tmpspans[tmpfill] = new_right_span;
		tmp[tmpfill++] = new_right;

		if(i+1 < fill) {
			tmpspans[tmpfill] = leaf->spans[i+1];
			tmp[tmpfill++] = leaf->slices[i+1];
		}
		// clearly we can create at most one extra slice
		// unmergeable [L]*[L] -> [L]*[X]|[L] <=> full leaf +1 overflow
		// delta == 0 means +1 for new_right being inserted
		unsigned newfill = merge_slices(tmpspans, tmp, tmpfill);
		int delta = tmpfill - newfill;
		assert(delta <= 3); // [S][S|S][S] -> [S]
		unsigned realfill = fill - (delta-1);

		if(realfill > BL)
			return BL + 1;
		st_dbg("merged %u nodes\n", delta);
		if(i > 0) {
			i--, slice_span--, slice--; // see above
		}
		unsigned count = fill - (i + (tmpfill-1)); // exclude new_right
		memmove(slice_span + newfill, slice_span + (tmpfill-1), count * SZSIZE);
		memmove(slice + newfill, slice + (tmpfill-1), count * SLICESIZE);
		memcpy(slice_span, tmpspans, newfill * SZSIZE);
		memcpy(slice, tmp, newfill * SLICESIZE);
		if(delta > 0)
			leaf_clrslots(leaf, realfill, fill);
		return realfill;
	}
}

// span is negative to indicate deltas for partial deletions
static long delete_leaf(struct leaf *leaf, size_t pos, long *span,
						struct leaf **split, size_t *splitsize, void *ctx)
{
	st_dbg("delete_leaf: pos: %zd span %zd\n", pos, *span);
	unsigned i = leaf_offset(leaf, &pos);
	unsigned fill = leaf_fill(leaf, i);
	st_dbg("deletion: found slot %u, target fill %u\n", i, fill);
	size_t len = -*span;

	if(pos + len < leaf->spans[i]) {
		size_t oldspan = leaf->spans[i];
		size_t delta = -len;
		unsigned newfill = delete_within_slice(leaf, fill, i, pos, len);
		if(newfill > BL) {
			assert(newfill == BL+1);
			st_dbg("deletion within piece: overflow\n");
			leaf->spans[i] = oldspan; // restore for right_span calculation
			size_t right_span = leaf->spans[i] - pos - len;
			struct slice new_right = {
				.blk = leaf->slices[i].blk,
				.offset = leaf->slices[i].offset + pos + len
			};
			leaf->slices[i].blk->refs++;
			leaf->spans[i] = pos;
			i++;
			// fill == BL, we must split
			fill = BL / 2 + (i > BL / 2);
			*split = split_leaf(leaf, fill);
			*splitsize = leaf_sum((struct leaf *)*split, BL - fill);
			delta -= *splitsize; // - len - splitsize
			if(i > BL / 2) {
				delta -= right_span;
				*splitsize += right_span;
				leaf = (struct leaf *)*split;
				i -= fill;
			}
			size_t n = fill - i;
			memmove(&leaf->spans[i+1], &leaf->spans[i], n * SZSIZE);
			memmove(&leaf->slices[i+1], &leaf->slices[i], n * SLICESIZE);
			leaf->spans[i] = right_span;
			leaf->slices[i] = new_right;
		}
		else if(newfill < BL/2) // underflow
			*splitsize = newfill;
		return delta;
	} else {
		// TODO impl delete B slices, update *span, underflow and return delta
		assert(!"unhandled multiple slice case");
		return 0;
	}
}

void st_delete(SliceTable *st, size_t pos, size_t len)
{
	len = MIN(len, st_size(st) - pos);
	if(len == 0)
		return;

	st_dbg("st_delete at pos %zd of len %zd\n", pos, len);
	struct inner *split = NULL;
	size_t splitsize;
	do {
		long remaining = -len;
		// NOTE: remaining is bytes left to delete.
		st_dbg("deleting... %ld bytes remaining\n", remaining);
		edit_recurse(st->height, st->root, pos, &remaining,
					delete_leaf, NULL, &split, &splitsize);
		len += remaining;
		// handle underflow
		if(st->height > 1 && inner_fill(st->root, 0) == 1) {
			st->root = st->root->children[0];
			st->height--;
		}
		// handle root split
		if(split) {
			st_dbg("allocating new root\n");
			struct inner *newroot = new_inner();
			newroot->spans[0] = st_size(st);
			newroot->children[0] = st->root;
			newroot->spans[1] = splitsize;
			newroot->children[1] = split;
			st->root = newroot;
			st->height++;
		}
	} while(len > 0);
}

/* debugging */

void st_print_struct_sizes(void)
{
	printf(
		"Implementation: \e[38;5;1mst\e[0m\n"
		"sizeof(struct inner): %zd\n"
		"sizeof(struct leaf): %zd\n"
		"sizeof(PieceTable): %zd\n",
		sizeof(struct inner), sizeof(struct leaf), sizeof(SliceTable)
	);
}

static bool check_recurse(struct inner *root, unsigned level)
{
	if(level == 1) {
		struct leaf *leaf = (struct leaf *)root;
		unsigned fill = leaf_fill(leaf, 0);
		bool fillcheck = fill >= BL/2;
		if(!fillcheck)
			return false;

		bool last_issmall = false, issmall;
		for(unsigned i = 0; i < fill; i++) {
			issmall = leaf->spans[i] <= HIGH_WATER;
			if(last_issmall && issmall) {
				st_dbg("leaf size violation in slot %u of ", i);
				print_node(1, root);
				return false;
			}
			last_issmall = issmall;
		}
		return true;
	} else {
		unsigned fill = inner_fill(root, 0);
		// TODO uncomment after handling underflow
		/*
		bool fillcheck = fill >= B/2;
		if(!fillcheck)
			return false;
			*/

		for(unsigned i = 0; i < fill; i++) {
			struct inner *child = root->children[i];
			unsigned childlevel = level - 1;
			if(!check_recurse(child, childlevel))
				return false;

			size_t spansum;
			if(childlevel == 1) {
				struct leaf *leaf = (struct leaf *)child;
				size_t fill = leaf_fill(leaf, 0);
				spansum = leaf_sum(leaf, fill);
			} else {
				size_t fill = inner_fill(child, 0);
				spansum = inner_sum(child, fill);
			}
			if(spansum != root->spans[i]) {
				st_dbg("child span violation in slot %u of ", i);
				print_node(level, root);
				st_dbg("with child sum: %zd span %zd\n",spansum,root->spans[i]);
				return false;
			}
		}
		return true;
	}
}

bool st_check_invariants(SliceTable *st)
{
	return check_recurse(st->root, st->height);
}

bool st_dump(SliceTable *st, const char *path);
#if 0
int main(void)
{
	st_print_struct_sizes();
	SliceTable *st = st_new_from_file("test.xml");
	st_dbg("original size: %ld\n", st_size(st));
	for(int i = 0; i < 100; i++) {
		long n = 34+i*59;
		st_delete(st, n, 5);
		st_insert(st, n, "thang", 5);
	}
	assert(st_check_invariants(st));
	st_dbg("size: %ld\n", st_size(st));
	//st_to_dot(st, "t.dot");
	FILE *test = fopen("test", "w");
	st_dump(st, test);
	fclose(test);
}
#endif

static void print_node(unsigned level, const struct inner *node)
{
	char out[256], *it = out;

	if(level == 1) {
		struct leaf *leaf = (struct leaf *)node;
		it += sprintf(it, "[k: ");
		for(unsigned i = 0; i < BL; i++) {
			size_t key = leaf->spans[i];
			if(key != ULONG_MAX)
				it += sprintf(it, "%lu|", key);
			else
				it += sprintf(it, "NUL|");
		}
		it--;
		it += sprintf(it, " p: ");
		for(unsigned i = 0; i < BL; i++) {
			struct slice *val = LVAL(leaf, i);
			it += sprintf(it, "%lu|", val ? val->offset : 0);
		}
		it--;
		it += sprintf(it, "]");
	} else {
		it += sprintf(it, "[");
		for(unsigned i = 0; i < B; i++) {
			size_t key = node->spans[i];
			if(key != ULONG_MAX)
				it += sprintf(it, "%lu|", key);
			else
				it += sprintf(it, "0|");
		}
		it--;
		it += sprintf(it, "]");
	}
	printf("%s ", out);
	fflush(stdout);
}

/* global queue */

struct q {
	unsigned level;
	struct inner *node;
};

#define QSIZE 100000
struct q queue[QSIZE]; // a ring buffer
unsigned int tail = 0, head = 0;

static void enqueue(struct q q) {
	assert(head == tail || head % QSIZE != tail % QSIZE);
	queue[head++ % QSIZE] = q;
}

static struct q *dequeue(void) {
	return (tail == head) ? NULL : &queue[tail++ % QSIZE];
}

void st_pprint(SliceTable *st)
{
	enqueue((struct q){ st->height, st->root });
	struct q *next;
	unsigned lastlevel = 1;
	while(next = dequeue()) {
		if(lastlevel != next->level)
			puts("");
		print_node(next->level, next->node);
		if(next->level > 1)
			for(unsigned i = 0; i < inner_fill(next->node, 0); i++)
				enqueue((struct q){ next->level-1, next->node->children[i] });
		lastlevel = next->level;
	}
	puts("");
}

bool st_dump(SliceTable *st, const char *path)
{
	// TODO handle ugly error cases here
	FILE *file = fopen(path, "w");
	if(!file)
		return false;

	enqueue((struct q){ st->height, st->root });
	struct q *next;
	while(next = dequeue()) {
		if(next->level > 1)
			for(unsigned i = 0; i < inner_fill(next->node, 0); i++)
				enqueue((struct q){ next->level-1, next->node->children[i] });
		else { // start dumping
			struct leaf *leaf = (struct leaf *)next->node;
			for(unsigned i = 0; i < leaf_fill(leaf, 0); i++) {
				fprintf(file, "%.*s", (int)leaf->spans[i], // FIXME use write()
						leaf->slices[i].blk->data + leaf->slices[i].offset);
			}
		}

	}
	return true;
}

/* dot output */

#include "dot.h"

static void slice_to_dot(FILE *file, const struct slice *slice)
{
	char *tmp = NULL, *port = NULL;

	if(!slice) {
		graph_table_entry(file, NULL, NULL);
		return;
	}
	FSTR(tmp, "offset: %lu", slice->offset);
	FSTR(port, "%ld", (long)slice);
	graph_table_entry(file, tmp, port);

	free(tmp);
	free(port);
}

static void leaf_to_dot(FILE *file, const struct leaf *leaf)
{
	char *tmp = NULL, *port = NULL;
	graph_table_begin(file, leaf, "aquamarine3");

	for(unsigned i = 0; i < BL; i++) {
		size_t key = leaf->spans[i];
		if(key != ULONG_MAX)
			FSTR(tmp, "%lu", key);
		else
			tmp = NULL;
		graph_table_entry(file, tmp, NULL);
	}
	for(unsigned i = 0; i < BL; i++)
		slice_to_dot(file, LVAL(leaf, i));
	graph_table_end(file);
	// output blocks
	for(unsigned i = 0; i < leaf_fill(leaf, 0); i++) {
		const struct slice *slice = LVAL(leaf, i);
		graph_table_begin(file, slice->blk, "darkgreen");
		FSTR(tmp, "%.*s", (int)slice->blk->len, (char *)slice->blk->data);
		graph_table_entry(file, tmp, NULL);
		graph_table_end(file);
		FSTR(port, "%ld", (long)slice);
		graph_link(file, leaf, port, slice->blk, "body");
	}
	free(tmp);
}

static void inner_to_dot(FILE *file, const struct inner *root, unsigned height)
{
	if(!root)
		return;
	if(height == 1)
		return leaf_to_dot(file, (struct leaf *)root);

	char *tmp = NULL, *port = NULL;
	graph_table_begin(file, root, NULL);

	for(unsigned i = 0; i < B; i++) {
		size_t key = root->spans[i];
		if(key != ULONG_MAX) {
			FSTR(tmp, "%lu", key);
			FSTR(port, "%u", i);
		} else
			tmp = port = NULL;
		graph_table_entry(file, tmp, port);
	}
	graph_table_end(file);

	for(unsigned i = 0; i < B; i++) {
		struct inner *child = root->children[i];
		if(!child)
			break;
		FSTR(tmp, "%d", i);
		graph_link(file, root, tmp, child, "body");
		inner_to_dot(file, child, height - 1);
	}
	free(tmp);
	free(port);
}

bool st_to_dot(SliceTable *st, const char *path)
{
	char *tmp = NULL;
	FILE *file = fopen(path, "w");
	if(!file)
		goto fail;
	graph_begin(file);

	graph_table_begin(file, st, NULL);
	FSTR(tmp, "height: %u", st->height);
	graph_table_entry(file, tmp, NULL);
	graph_table_entry(file, "root", "root");
	graph_table_end(file);

	graph_link(file, st, "root", st->root, "body");
	if(st->root)
		inner_to_dot(file, st->root, st->height);

	graph_end(file);
	free(tmp);
	if(fclose(file))
		goto fail;
	return true;

fail:
	perror("st_to_dot");
	return false;
}
