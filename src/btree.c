/*
 * persistent b+tree slice sequence
 */

#include <assert.h>
#include <limits.h>
#include <stdatomic.h>
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

#define HIGH_WATER (1<<10)

enum blktype { HEAP, MMAP };
struct block {
	// atomic counter of references to this block
	atomic_int refc;
	// packed with int above. LARGE_MMAP indicates file mmap
	enum blktype type;
	// owned by the block, which lives as long as the slicetable, same as the
	// leaves that immutably point into it. So this is safe, but how in rust?
	char *data;
	// length of the block, used for mmap
	size_t len;
	// singly linked list for freeing later
	struct block *next;
};

#define NODESIZE (256 - sizeof(atomic_int)) // close enough
#define PER_B (sizeof(size_t) + sizeof(void *))
#define B ((int)(NODESIZE / PER_B))
struct node {
	atomic_int refc;
	size_t spans[B];
	void *child[B]; // in leaves (level 1), these are data pointers
};

struct slicetable {
	// tree root
	struct node *root;
	// singly linked list of blocks
	struct block *blocks;
	// used for recursion. we could tag pointers instead, but that's a hack
	// and we need to track blocks anyways
	int levels;
};

/* blocks */

static size_t count_lfs(const char *s, size_t len) {
	size_t count = 0;
	for(size_t i = 0; i < len; i++)
		if(*s++ == '\n')
			count++;
	return count;
}

static void free_block(struct block *block)
{
	switch(block->type) {
		case MMAP: munmap(block->data, block->len); break;
		case HEAP: free(block->data);
	}
	free(block);
}

static void drop_block(struct block *block)
{
	if(atomic_fetch_sub_explicit(&block->refc,1,memory_order_release) == 1) {
		atomic_thread_fence(memory_order_acquire);
		if(block->next)
			drop_block(block->next);
		free_block(block);
	}
}

static void block_insert(char *block, size_t off, const char *data, size_t len)
{
	assert(off <= HIGH_WATER);
	memmove(block + off + len, block + off, HIGH_WATER - off - len);
	memcpy(block + off, data, len);
}

static void block_delete(char *block, size_t off, size_t len)
{
	assert(off + len <= HIGH_WATER);
	memmove(block + off, block + off + len, HIGH_WATER - off - len);
}

/* tree utilities */

static void print_node(const struct node *node, int level);
bool st_check_invariants(const SliceTable *st);

static void node_clrslots(struct node *node, int from, int to)
{
	assert(to <= B);
	for(int i = from; i < to; i++)
		node->spans[i] = ULONG_MAX;

	memset(&node->child[from], 0, (to - from) * sizeof(void *));
}

static struct node *new_node(void)
{
	struct node *node = malloc(sizeof *node);
	node_clrslots(node, 0, B);
	atomic_store_explicit(&node->refc, 1, memory_order_relaxed);
	return node;
}

// sums the spans of entries in node, up to fill
static size_t node_sum(const struct node *node, int fill)
{
	size_t sum = 0;
	for(int i = 0; i < fill; i++)
		sum += node->spans[i];
	return sum;
}

// returns index of the first key spanning the search key in node
// key contains the offset at the end
static int node_offset(const struct node *node, size_t *key)
{
	int i = 0;
	while(*key > node->spans[i])
		*key -= node->spans[i++];
	return i;
}

// count the number of live entries in node counting up from START
static int node_fill(const struct node *node, int start)
{
	int i;
	for(i = start; i < B; i++)
		if(!node->child[i])
			break;
	return i;
}

void drop_node(struct node *root, int level)
{
	if(level == 1) {
		if(atomic_fetch_sub_explicit(&root->refc,1,memory_order_release)==1) {
			atomic_thread_fence(memory_order_acquire);
			for(int i = 0; i < node_fill(root, 0); i++)
				if(root->spans[i] <= HIGH_WATER)
					free(root->child[i]); // free small allocations
			free(root);
		}
	} else // node node
		if(atomic_fetch_sub_explicit(&root->refc,1,memory_order_release)==1) {
			atomic_thread_fence(memory_order_acquire);
			for(int i = 0; i < node_fill(root, 0); i++)
				drop_node(root->child[i], level - 1);
			free(root);
		}
}


static void incref(atomic_int *refc)
{
	// Should be safe as long as this increment is made visible to another
	// thread T in passing the object to T, avoiding a data race when T reads
	// refc == 1 and proceeds to modify the object inplace whilst our original
	// thread is accessing it.
	// I'm trusting the boost atomics documentation here
	atomic_fetch_add_explicit(refc, 1, memory_order_relaxed);
}

static void ensure_node_editable(struct node **nodeptr, int level)
{
	struct node *node = *nodeptr;
	if(atomic_load_explicit(&node->refc, memory_order_acquire) != 1) {
		struct node *copy = malloc(sizeof *copy);
		memcpy(copy, node, sizeof *copy);
		atomic_store_explicit(&copy->refc, 1, memory_order_relaxed);
		// in a leaf, copy small data blocks as we modify them inplace
		int fill = node_fill(node, 0);
		if(level == 1) {
			for(int i = 0; i < fill; i++)
				if(node->spans[i] <= HIGH_WATER) {
					char *copy = malloc(HIGH_WATER);
					memcpy(copy, node->child[i], node->spans[i]);
					node->child[i] = copy;
				}
		} else
			for(int i = 0; i < fill; i++)
				incref(&((struct node *)node->child[i])->refc);

		drop_node(node, level);
		*nodeptr = copy;
	}
}

/* simple */

int st_depth(const SliceTable *st) { return st->levels - 1; }

size_t st_size(const SliceTable *st)
{
	return node_sum(st->root, node_fill(st->root, 0));
}

SliceTable *st_new(void)
{
	SliceTable *st = malloc(sizeof *st);
	st->root = new_node();
	st->blocks = NULL;
	st->levels = 1;
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
		// TODO
		if(read(fd, data, len) != len) {
			free(data);
			return NULL;
		}
		type = HEAP;
	} else {
		data = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
		close(fd);
		if(data == MAP_FAILED)
			return NULL;
		type = MMAP;
	}

	SliceTable *st = malloc(sizeof *st);
	struct block *init = malloc(sizeof(struct block));
	*init = (struct block){
		.type = type, .refc = 1, .data = data, .len = len, .next = NULL
	};

	struct node *leaf = new_node();
	leaf->spans[0] = len;
	leaf->child[0] = data;
	st->root = (struct node *)leaf;
	st->blocks = init;
	st_dbg("allocating st->blocks %p\n", st->blocks);
	st->levels = 1;
	return st;
}

void st_free(SliceTable *st)
{
	drop_node(st->root, st->levels);
	st_dbg("freeing st->blocks %p\n", st->blocks);
	if(st->blocks)
		drop_block(st->blocks);
	free(st);
}

SliceTable *st_clone(const SliceTable *st)
{
	SliceTable *clone = malloc(sizeof *clone);
	clone->levels = st->levels;
	clone->root = st->root;
	clone->blocks = st->blocks;
	incref(&st->root->refc);
	incref(&st->blocks->refc);
	return clone;
}

/* utilities */

// may *ONLY* be used for inserting in small slices
char *slice_insert(SliceTable *st, char *target, size_t offset,
				const char *data, size_t len, size_t *tspan)
{
	if(*tspan + len > HIGH_WATER) {
		struct block *new = malloc(sizeof *new);
		new->len = *tspan + len;
		target = realloc(target, new->len);
		new->data = target;
		memmove(target + offset + len, &target[offset], *tspan - offset);
		memcpy(target + offset, data, len);
		new->type = HEAP;
		// we have exclusive access here
		atomic_store_explicit(&new->refc, 1, memory_order_relaxed);
		// no change to reference count, as we're still pointing to it
		new->next = st->blocks;
		st->blocks = new;
	} else
		block_insert(target, offset, data, len);
	*tspan += len;
	return target;
}

int merge_slices(SliceTable *st, size_t spans[static 5], char *data[static 5],
				int fill)
{
	int i = 1;
	while(i < fill) {
		if(spans[i] > HIGH_WATER)
			i += 2; // X|L__ to XL_|_
		else if(spans[i-1] <= HIGH_WATER) {
			// both are smaller S|S, i doesn't move
			data[i-1] = slice_insert(st, data[i-1], spans[i-1],
									data[i], spans[i], &spans[i-1]);
			free(data[i]);
			memmove(&spans[i], &spans[i+1], (fill - (i+1)) * sizeof(size_t));
			memmove(&data[i], &data[i+1], (fill - (i+1)) * sizeof(char *));
			fill--;
		} else
			i++; // L|S_ -> LS|_
	}
	return fill;
}

static struct node *split_node(struct node *node, int offset)
{
	struct node *split = new_node();
	int count = B - offset;
	memcpy(&split->spans[0], &node->spans[offset], count * sizeof(size_t));
	memcpy(&split->child[0], &node->child[offset], count * sizeof(void *));
	node_clrslots(node, offset, B);
	return split;
}

// steals slots from j into i, returning the total size of slots moved
size_t rebalance_node(struct node * restrict i, struct node * restrict j,
					int ifill, int jfill, bool i_on_left)
{
	size_t delta = 0;
	int count = (ifill + jfill <= B) ? jfill : (B/2 + (B&1) - ifill);
	if(i_on_left) {
		for(int c = 0; c < count; c++) {
			i->spans[ifill+c] = j->spans[c];
			i->child[ifill+c] = j->child[c];
			delta += i->spans[ifill+c];
		}
		memmove(&j->spans[0], &j->spans[count], (jfill-count)*sizeof(size_t));
		memmove(&j->child[0], &j->child[count], (jfill-count)*sizeof(void *));
		node_clrslots(j, jfill - count, jfill);
	} else {
		memmove(&i->spans[count], &i->spans[0], ifill * sizeof(size_t));
		memmove(&i->child[count], &i->child[0], ifill * sizeof(void *));
		for(int c = 0; c < count; c++) {
			i->spans[c] = j->spans[jfill-count+c];
			i->child[c] = j->child[jfill-count+c];
			delta += i->spans[c];
		}
		node_clrslots(j, jfill - count, jfill);
	}
	return delta;
}

size_t merge_boundary(SliceTable *st, struct node **lptr, int lfill)
{
	struct node *l = lptr[0];
	struct node *r = lptr[1];
	// merge the boundary slices if they're both small
	if(l->spans[lfill-1] <= HIGH_WATER && r->spans[0] <= HIGH_WATER) {
		size_t delta = l->spans[lfill-1];
		r->child[0] = slice_insert(st, r->child[0], 0, l->child[lfill-1],
								delta, &r->spans[0]);
		free(l->child[lfill-1]);
		node_clrslots(l, lfill - 1, lfill);
		return delta;
	}
	return 0;
}

// removes the jth slot of root
// root(j) **MUST** be editable and its slices must have been moved already
void node_remove(struct node *root, int fill, int j)
{
	free(root->child[j]); // slices shifted over
	size_t count = fill - (j+1);
	memmove(&root->spans[j], &root->spans[j+1], count * sizeof(size_t));
	memmove(&root->child[j], &root->child[j+1], count * sizeof(void *));
	node_clrslots(root, fill - 1, fill);
}

/* the complex stuff */

typedef long (*leaf_case)(SliceTable *st, struct node *leaf,
						size_t pos, long *span,
						struct node **split, size_t *splitsize, void *ctx);

static long edit_recurse(SliceTable *st, int level, struct node *root,
						size_t pos, long *span,
						leaf_case base_case, void *ctx,
						struct node **split, size_t *splitsize)
{
	if(level == 1)
		return base_case(st,(void *)root,pos,span,(void *)split,splitsize,ctx);
	else { // level > 1: node node recursion
		struct node *childsplit = NULL;
		size_t childsize = 0;
		int i = node_offset(root, &pos);

		ensure_node_editable((struct node **)&root->child[i], level - 1);
		long delta = edit_recurse(st, level - 1, root->child[i], pos, span,
								base_case, ctx, &childsplit, &childsize);
		st_dbg("applying upwards delta at level %d: %ld\n", level, delta);
		root->spans[i] += delta;
		// reset delta
		delta = *span;

		if(childsize) {
			if(childsplit) { // overflow: attempt to insert childsplit at i+1
				i++;
				int fill = node_fill(root, i);
				if(fill == B) {
					fill = B/2 + (i > B/2);
					*split = split_node(root, fill);
					*splitsize = node_sum(*split, B - fill);
					delta -= *splitsize;
					if(i > B/2) {
						delta -= childsize;
						*splitsize += childsize;
						root = *split;
						i -= fill;
					}
				}
				size_t *start = &root->spans[i];
				struct node **cstart = (struct node **)&root->child[i];
				memmove(start + 1, start, (fill - i) * sizeof(size_t));
				memmove(cstart + 1, cstart, (fill - i) * sizeof(void*));
				root->spans[i] = childsize;
				root->child[i] = childsplit;
			} else { // children[i] underflowed
				st_dbg("handling underflow at %d, level %d\n", i, level);
				int j = i > 0 ? i-1 : i+1;
				int fill = node_fill(root, i);
				long shifted = 0;
				if(childsize == ULONG_MAX)
					root->spans[j = i] = 0; // mark j = i as deleted
				else {
					int jfill = node_fill((void *)root->child[j], 0);
					ensure_node_editable((void *)&root->child[j], level - 1);
					if(level-1 == 1) {
						size_t res;
						if(i < j) {
							if(res = merge_boundary(st, (void*)&root->child[i],
													childsize))
								childsize--, shifted -= res;
						} else // j < i
							if(res = merge_boundary(st, (void*)&root->child[j],
													jfill))
								jfill--, shifted += res;
					}
					// transfer some slots from j to i
					shifted += rebalance_node((void *)root->child[i],
											(void *)root->child[j],
											childsize, jfill, i < j);
				}
				root->spans[i] += shifted;
				root->spans[j] -= shifted;
				// j was merged into oblivion
				if(root->spans[j] == 0) {
					node_remove(root, fill, j); // propagate underflow up
					if(fill - 1 < B/2 + (B&1))
						*splitsize = fill - 1;
				}
			}
		}
		return delta;
	}
}

/* insertion */

// handle insertion within LARGE slices
static long insert_within_slice(SliceTable *st, struct node *leaf, int fill,
							int i, size_t off, char *new, size_t newlen,
							struct node **split, size_t *splitsize)
{
	assert(leaf->spans[i] > HIGH_WATER);
	size_t *left_span = &leaf->spans[i];
	char **left = (char **)&leaf->child[i];
	size_t right_span = *left_span - off;
	char *right;
	// maintain block uniqueness
	if(right_span <= HIGH_WATER) {
		right = malloc(HIGH_WATER);
		memcpy(right, *left + off, right_span);
	} else
		right = *left + off;

	assert(off > 0); // should be handled by general case
	// demote left fragment if necessary
	if(off <= HIGH_WATER) {
		char *new = malloc(HIGH_WATER);
		memcpy(new, *left, off);
		*left = new;
	} // then truncate
	*left_span = off;
	// fill tmp
	size_t tmpspans[5];
	char *tmp[5];
	int tmpfill = 0;
	if(i > 0) {
		tmpspans[tmpfill] = leaf->spans[i-1];
		tmp[tmpfill++] = leaf->child[i-1];
	}
	tmpspans[tmpfill] = *left_span;
	tmp[tmpfill++] = *left;
	tmpspans[tmpfill] = newlen;
	tmp[tmpfill++] = new;
	tmpspans[tmpfill] = right_span;
	tmp[tmpfill++] = right;

	if(i+1 < fill) {
		tmpspans[tmpfill] = leaf->spans[i+1];
		tmp[tmpfill++] = leaf->child[i+1];
	}
	int newfill = merge_slices(st, tmpspans, tmp, tmpfill);
	int delta = tmpfill - newfill;
	assert(delta <= 3); // [S][S1|Si|S2][S] -> [L][S], S1+S2 > HIGH_WATER
	st_dbg("merged %d nodes\n", delta);
	if(i > 0) {
		i--, left_span--, left--; // see above
	}
	int realfill = fill - (delta-2);
	if(realfill <= B) {
		size_t count = fill - (i + (tmpfill-2));
		memmove(left_span + newfill, left_span + (tmpfill-2),
				count * sizeof(size_t));
		memmove(left + newfill, left + (tmpfill-2), count * sizeof(char *));
		// when delta == 0, newfill exceeds tmpfill-2 and may overwrite
		// old slots, so we copy afterwards
		memcpy(left_span, tmpspans, newfill * sizeof(size_t));
		memcpy(left, tmp, newfill * sizeof(char *));
		if(delta > 2)
			node_clrslots(leaf, realfill, fill);
		if(realfill < B/2 + (B&1))
			*splitsize = realfill; // indicate underflow
		return newlen;
	} else { // realfill > B: leaf split, we have at most 2 new slices
		size_t spans[B + 2];
		char *blocks[B + 2];
		// copy all data to temporary buffers and distribute
		memcpy(spans, leaf->spans, i * sizeof(size_t));
		memcpy(blocks, leaf->child, i * sizeof(char *));
		memcpy(&spans[i], tmpspans, newfill * sizeof(size_t));
		memcpy(&blocks[i], tmp, newfill * sizeof(char *));
		int count = fill - (i + (tmpfill-2));
		memcpy(&spans[i+newfill], &leaf->spans[i+tmpfill-2],
				count * sizeof(size_t));
		memcpy(&blocks[i+newfill], &leaf->child[i+tmpfill-2],
				count * sizeof(char *));
		struct node *right_split = new_node();
		// n.b. we must compute delta directly since merging moves the insert
		size_t oldsum = node_sum(leaf, fill) + right_span;
		size_t new_node_fill = B/2 + 1; // B=5 6,7 -> 3,4 in right
		size_t right_fill = realfill - (B/2 + 1); // B=4 5,6 -> 2,3 in right
		memcpy(leaf->spans, spans, new_node_fill * sizeof(size_t));
		memcpy(leaf->child, blocks, new_node_fill * sizeof(char *));
		memcpy(right_split->spans, &spans[new_node_fill],
				right_fill * sizeof(size_t));
		memcpy(right_split->child, &blocks[new_node_fill],
				right_fill * sizeof(char *));
		node_clrslots(leaf, new_node_fill, fill);
		node_clrslots(right_split, right_fill, B);
		size_t newsum = node_sum(leaf, new_node_fill);
		*splitsize = node_sum(right_split, right_fill);
		*split = right_split;
		return newsum - oldsum;
	}
}

struct insert_ctx {
	size_t lfs;
	const char *data;
};

static long insert_leaf(SliceTable *st, struct node *leaf,
						size_t pos, long *span,
						struct node **split, size_t *splitsize, void *ctx)
{
	int i = node_offset(leaf, &pos);
	int fill = node_fill(leaf, i);
	st_dbg("insertion: found slot %d, offset %zu target fill %d\n",
			i, pos, fill);
	size_t len = *span;
	long delta = len;
	bool at_bound = (pos == leaf->spans[i]);
	const char *data = ((struct insert_ctx *)ctx)->data;
	// we scan the data here for better locality
	((struct insert_ctx *)ctx)->lfs = count_lfs(data, len);
	// if we are inserting at 0, pos will be 0
	if(pos == 0 && leaf->spans[0] <= HIGH_WATER) {
		assert(i == 0);
		leaf->child[0] = slice_insert(st, (char *)leaf->child[0], 0,
									data, len, &leaf->spans[0]);
	}
	else if(leaf->spans[i] <= HIGH_WATER) {
		leaf->child[i] = slice_insert(st, (char *)leaf->child[i], pos,
									data, len, &leaf->spans[i]);
	} // try start of i+1
	else if(at_bound && (i < fill-1) && leaf->spans[i+1] <= HIGH_WATER) {
		leaf->child[i+1] = slice_insert(st, (char *)leaf->child[i+1], 0,
										data, len, &leaf->spans[i+1]);
	} else { // make a modifiable copy
		char *copy;
		if(len > HIGH_WATER) {
			copy = malloc(len);
			struct block *new = malloc(sizeof *new);
			new->data = copy;
			new->type = HEAP;
			new->len = len;
			atomic_store_explicit(&new->refc, 1, memory_order_relaxed);
			new->next = st->blocks;
			st->blocks->next = new; // still pointing, no refc update
		} else {
			copy = malloc(HIGH_WATER);
		}
		memcpy(copy, data, len);
		// insertion on boundary [L]|[L], no merging possible
		if(at_bound || pos == 0) {
			i += at_bound; // if at_bound, we are inserting at index i+1
			if(fill == B) {
				fill = B/2 + (i > B/2);
				*split = split_node(leaf, fill);
				*splitsize = node_sum((struct node *)*split, B - fill);
				delta -= *splitsize;
				if(i > B/2) {
					delta -= len;
					*splitsize += len;
					leaf = (struct node *)*split;
					i -= fill;
				}
			}
			memmove(&leaf->spans[i+1],&leaf->spans[i],(fill-i)*sizeof(size_t));
			memmove(&leaf->child[i+1],&leaf->child[i],(fill-i)*sizeof(char *));
			leaf->spans[i] = len;
			leaf->child[i] = copy;
		} else
			return insert_within_slice(st, leaf, fill, i, pos, copy, len,
									split, splitsize);
	}
	return delta;
}

size_t st_insert(SliceTable *st, size_t pos, const char *data, size_t len)
{
	if(len == 0)
		return 0;

	st_dbg("st_insert at pos %zd of len %zd\n", pos, len);
	struct node *split = NULL;
	size_t splitsize;
	long span = (long)len;
	struct insert_ctx ctx = { .data = data };

	ensure_node_editable(&st->root, st->levels);
	edit_recurse(st, st->levels, st->root, pos, &span, &insert_leaf, &ctx,
				&split, &splitsize);
	// handle root underflow
	if(st->levels > 1 && node_fill(st->root, 0) == 1) {
		st_dbg("handling root underflow\n");
		st->root = st->root->child[0];
		st->levels--;
	}
	// handle root split
	if(split) {
		st_dbg("allocating new root\n");
		struct node *newroot = new_node();
		newroot->spans[0] = st_size(st);
		newroot->child[0] = st->root; // we only switched the pointer
		newroot->spans[1] = splitsize;
		newroot->child[1] = split;
		st->root = newroot;
		st->levels++;
	}
	return ctx.lfs;
}

/* deletion */

static int delete_within_slice(SliceTable *st, struct node *leaf, int fill,
								int i, size_t new_right_span, char *new_right)
{
	size_t *slice_span = &leaf->spans[i];
	char **data = (char **)&leaf->child[i];
	size_t tmpspans[5];
	char *tmp[5];
	int tmpfill = 0;
	if(i > 0) {
		tmpspans[tmpfill] = leaf->spans[i-1];
		tmp[tmpfill++] = leaf->child[i-1];
	}
	tmpspans[tmpfill] = *slice_span;
	tmp[tmpfill++] = *data;
	tmpspans[tmpfill] = new_right_span;
	tmp[tmpfill++] = new_right;

	if(i+1 < fill) {
		tmpspans[tmpfill] = leaf->spans[i+1];
		tmp[tmpfill++] = leaf->child[i+1];
	}
	// clearly we can create at most one extra slice
	// unmergeable [L]*[L] -> [L]*[X]|[L] <=> full leaf +1 overflow
	// delta == 0 means +1 for new_right being inserted
	int newfill = merge_slices(st, tmpspans, tmp, tmpfill);
	int delta = tmpfill - newfill;
	assert(delta <= 3); // [S][S|S][S] -> [S]
	int realfill = fill - (delta-1);

	if(realfill > B)
		return B + 1;
	st_dbg("merged %d nodes\n", delta);
	if(i > 0) {
		i--, slice_span--, data--; // see above
	}
	int count = fill - (i + (tmpfill-1)); // exclude new_right
	memmove(slice_span + newfill, slice_span + (tmpfill-1),
			count * sizeof(size_t));
	memmove(data + newfill, data + (tmpfill-1), count * sizeof(char *));
	memcpy(slice_span, tmpspans, newfill * sizeof(size_t));
	memcpy(data, tmp, newfill * sizeof(char *));
	if(delta > 0)
		node_clrslots(leaf, realfill, fill);
	return realfill;
}

// span is negative to indicate deltas for partial deletions
static long delete_leaf(SliceTable *st, struct node *leaf,
						size_t pos, long *span,
						struct node **split, size_t *splitsize, void *ctx)
{
	int i = node_offset(leaf, &pos);
	int fill = node_fill(leaf, i);
	// we search for pos + 1 as we assume our next chunk is in this leaf
	pos--;
	st_dbg("deletion: found slot %d, offset %zd, target fill %d\n",
			i, pos, fill);
	size_t len = -*span;

	if(pos > 0 && pos + len < leaf->spans[i]) {
		size_t oldspan = leaf->spans[i];
		char *olddata = leaf->child[i];
		size_t delta = -len;
		*(size_t *)ctx = count_lfs(leaf->child[i] + pos, len);
		// inplace deletion, no splitting/underflowing here
		if(oldspan <= HIGH_WATER) {
			block_delete(olddata, pos, len);
			leaf->spans[i] -= len;
			return delta;
		}
		size_t right_span = oldspan - pos - len;
		char *right;
		// copy data for right fragment
		if(right_span <= HIGH_WATER) {
			right = malloc(HIGH_WATER);
			memcpy(right, olddata + pos + len, right_span);
		} else
			right = olddata + pos + len;

		leaf->spans[i] = pos; // truncate slice
		// truncation might have resulted in a small block
		if(leaf->spans[i] <= HIGH_WATER) {
			char *new = malloc(HIGH_WATER);
			memcpy(new, olddata, pos);
			leaf->child[i] = new;
		}
		int newfill = delete_within_slice(st, leaf, fill, i, right_span, right);
		if(newfill > B) {
			assert(newfill == B+1);
			st_dbg("deletion within piece: overflow\n");
			i++;
			// fill == B, we must split
			fill = B/2 + (i > B/2);
			*split = split_node(leaf, fill);
			*splitsize = node_sum((struct node *)*split, B - fill);
			delta -= *splitsize; // = -(len + *splitsize)
			if(i > B/2) {
				delta -= right_span;
				*splitsize += right_span;
				leaf = (struct node *)*split;
				i -= fill;
			}
			size_t n = fill - i;
			memmove(&leaf->spans[i+1], &leaf->spans[i], n * sizeof(size_t));
			memmove(&leaf->child[i+1], &leaf->child[i], n * sizeof(char *));
			leaf->spans[i] = right_span;
			leaf->child[i] = right;
		}
		else if(newfill < B/2 + (B&1)) // underflow
			*splitsize = newfill;
		return delta;
	} else { // pos + len >= leaf->spans[i]
		size_t lfs = 0;
		int start = i;
		if(pos > 0) {
			len -= leaf->spans[i] - pos; // no. deleted characters remaining
			char **si = (char **)&leaf->child[i];
			lfs += count_lfs(*si + pos, leaf->spans[i] - pos);
			// if the span was large, consider reallocating after truncation
			if(leaf->spans[i] > HIGH_WATER && pos <= HIGH_WATER) {
				char *new = malloc(HIGH_WATER);
				memcpy(new, leaf->child[i], pos);
				leaf->child[i] = new;
			}
			leaf->spans[i] = pos; // truncate si, fine for small blocks
			start++;
		}
		int end = start;
		while(end < fill && len >= leaf->spans[end]) {
			char **se = (char **)&leaf->child[end];
			lfs += count_lfs(*se, leaf->spans[end]);
			// free small blocks
			if(leaf->spans[end] <= HIGH_WATER) {
				free(*se);
			}
			len -= leaf->spans[end];
			end++;
		}
		if(end < fill) { // if len == 0, st=end nothing happens. that's fine
			char **se = (char **)&leaf->child[end];
			lfs += count_lfs(*se, len);
			// delete prefix of end
			if(leaf->spans[end] <= HIGH_WATER) {
				block_delete(*se, 0, len);
				leaf->spans[end] -= len;
			} else { // cannot become 0 as the loop would've continued
				leaf->spans[end] -= len;
				// was large, now small
				if(leaf->spans[end] <= HIGH_WATER) {
					char *new = malloc(HIGH_WATER);
					;
					memcpy(new, leaf->child[end], leaf->spans[end]);
					leaf->child[end] = new;
				} else
					*se += len;
			}
			len = 0;
		}
		memmove(&leaf->spans[start], &leaf->spans[end],
				(fill - end) * sizeof(size_t));
		memmove(&leaf->child[start], &leaf->child[end],
				(fill - end) * sizeof(char *));
		int oldfill = fill;
		fill = start + fill-end;
		size_t tmpspans[5];
		char *tmp[5];
		// it's this simple! n.b. start may be truncated. Thus use start - 2
		start = MAX(0, start - 2);
		int tmpfill = MIN(fill - start, 4); // [][s|][|e][]
		memcpy(tmpspans, &leaf->spans[start], tmpfill * sizeof(size_t));
		memcpy(tmp, &leaf->child[start], tmpfill * sizeof(char *));
		// merge and copy in
		int newfill = merge_slices(st, tmpspans, tmp, tmpfill);
		st_dbg("merged %d nodes\n", tmpfill - newfill);
		fill -= tmpfill - newfill;
		memcpy(&leaf->spans[start], tmpspans, newfill * sizeof(size_t));
		memcpy(&leaf->child[start], tmp, newfill * sizeof(char *));
		// move old entries down
		memmove(&leaf->spans[start+newfill], &leaf->spans[start+tmpfill],
				(oldfill - (start + tmpfill)) * sizeof(size_t));
		memmove(&leaf->child[start+newfill], &leaf->child[start+tmpfill],
				(oldfill - (start + tmpfill)) * sizeof(char *));
		node_clrslots(leaf, fill, oldfill);

		if(fill < B/2 + (B&1))
			*splitsize = fill ? fill : ULONG_MAX; // ULONG_MAX indicates 0 size
		*(size_t *)ctx = lfs;
		return *span += len; // |requested-deleted| = |change|
	}
}

size_t st_delete(SliceTable *st, size_t pos, size_t len)
{
	len = MIN(len, st_size(st) - pos);
	if(len == 0)
		return 0;

	st_dbg("st_delete at pos %zd of len %zd\n", pos, len);
	struct node *split = NULL;
	size_t splitsize;
	// we only need to ensure root uniqueness once
	ensure_node_editable(&st->root, st->levels);

	size_t lfs = 0;
	do {
		long remaining = -len;
		// n.b. remaining is bytes left to delete.
		st_dbg("deleting... %ld bytes remaining\n", remaining);
		// search for pos + 1 (see above)
		// n.b. we never search for st_size+1 since that entails len = 0
		edit_recurse(st, st->levels, st->root, pos+1, &remaining,
					&delete_leaf, &lfs, &split, &splitsize);
		len += remaining; // adjusted to byte delta (e.g. -3)
		// handle underflow
		if(st->levels > 1 && node_fill(st->root, 0) == 1) {
			st_dbg("handling root underflow\n");
			st->root = st->root->child[0];
			st->levels--;
		}
		// handle root split
		if(split) {
			st_dbg("allocating new root\n");
			struct node *newroot = new_node();
			newroot->spans[0] = st_size(st);
			newroot->child[0] = st->root;
			newroot->spans[1] = splitsize;
			newroot->child[1] = split;
			st->root = newroot;
			st->levels++;
		}
		assert(st_check_invariants(st));
	} while(len > 0);

	return lfs;
}

/* iterator */

struct stackentry {
	struct node *node;
	int idx;
};

#define STACKSIZE 3
struct sliceiter {
	char *data;
	size_t off; // offset into slice
	struct node *leaf;
	int node_offset;
	struct stackentry stack[STACKSIZE];
	SliceTable *st;
	size_t pos; // absolute position
};

SliceIter *st_iter_to(SliceIter *it, size_t pos)
{
	it->pos = pos;
	// TODO having 2 exceptions is quite ugly
	size_t size = st_size(it->st);
	bool off_end = (pos == size);
	if(pos > 0)
		pos -= off_end;

	struct node *node = it->st->root;
	int level = it->st->levels;
	while(level > 1) {
		int i = 0;
		while(pos && pos >= node->spans[i])
			pos -= node->spans[i++];
		st_dbg("iter_to: found i: %d at level %d\n", i, level);
		int stackidx = level - 2; // level 2 goes at stack[0], etc.
		if(stackidx < STACKSIZE)
			it->stack[stackidx] = (struct stackentry){ node, i };

		node = node->child[i];
		level--;
	}
	struct node *leaf = (struct node *)node;
	it->leaf = leaf;
	// find position within leaf
	int i = 0;
	while(pos && pos >= leaf->spans[i])
		pos -= leaf->spans[i++];

	it->node_offset = i;
	it->off = pos;
	st_dbg("iter_to at leaf: i: %d, pos %zd\n", i, pos);

	if(size > 0) {
		it->data = (char *)leaf->child[i] + pos;
		// we searched for pos - 1
		if(off_end) {
			it->data++;
			it->off++;
		}
	}
	return it;
}

SliceIter *st_iter_new(SliceTable *st, size_t pos)
{
	SliceIter *it = malloc(sizeof *it);
	it->st = st;
	return st_iter_to(it, pos);
}

int iter_stacksize(SliceIter *it)
{
	return MIN(it->st->levels - 1, STACKSIZE);
}

void st_iter_free(SliceIter *it)
{
	// We shouldn't have to manage reference counting of nodes given the
	// invalidation upon freeing/modification of the corresponding slicetable.
	free(it);
}

SliceTable *st_iter_st(const SliceIter *it) { return it->st; }
size_t st_iter_pos(const SliceIter *it) { return it->pos; }

static bool iter_off_end(const SliceIter *it)
{
	return it->off == it->leaf->spans[it->node_offset];
}

bool st_iter_next_chunk(SliceIter *it)
{
	int i = it->node_offset;
	struct node *leaf = it->leaf;
	it->pos += leaf->spans[i] - it->off;
	// fast path: same leaf
	if(i < B-1 && leaf->spans[i+1] != ULONG_MAX) {
		it->node_offset++;
		it->off = 0;
		it->data = leaf->child[i+1];
		return true;
	}
	// traverse upwards until we find one
	int si = 0;
	struct stackentry s = it->stack[si];
	while(si < iter_stacksize(it) &&
			!(s.idx < B-1 && s.node->spans[s.idx+1] != ULONG_MAX))
		s = it->stack[++si];

	if(si != iter_stacksize(it)) {
		it->stack[si].idx++;
		while(--si > 0) {
			it->stack[si].node = it->stack[si+1].node->child[0];
			it->stack[si].idx = 0;
		}
		int leaf_idx = it->stack[0].idx;
		it->leaf = (struct node *)it->stack[0].node->child[leaf_idx];
		it->node_offset = 0;
		it->off = 0;
		it->data = it->leaf->child[0];
		return true;
	} else { // if the stack was insufficient, search from the root
		st_dbg("gave up. scanning from root for %zd\n", it->pos);
		st_iter_to(it, it->pos);
		return !iter_off_end(it);
	}
}

bool st_iter_prev_chunk(SliceIter *it)
{
	int i = it->node_offset;
	struct node *leaf = it->leaf;
	it->pos -= it->off + 1;
	// fast path: same leaf
	if(i > 0) {
		it->node_offset--;
		it->off = it->leaf->spans[i-1] - 1;
		it->data = (char *)leaf->child[i-1] + it->off;
		return true;
	}
	int si = 0;
	struct stackentry s = it->stack[si];
	while(si < iter_stacksize(it) && s.idx == 0)
		s = it->stack[++si];

	if(si != iter_stacksize(it)) {
		it->stack[si].idx--;
		while(--si > 0) {
			struct node *parent = it->stack[si+1].node;
			int parentfill = node_fill(parent, 0);
			it->stack[si].node = parent->child[parentfill-1];
			int fill = node_fill(it->stack[si].node, 0);
			it->stack[si].idx = fill - 1;
		}
		int leaf_i = it->stack[0].idx;
		struct node *leaf = (struct node *)it->stack[0].node->child[leaf_i];
		int fill = node_fill(leaf, 0);
		it->leaf = leaf;
		it->node_offset = fill - 1;
		it->off = leaf->spans[it->node_offset] - 1;
		it->data = (char *)leaf->child[fill-1] + it->off;
		return true;
	} else { // if stack was insufficient, reinitialize
		st_iter_to(it, MAX(0, it->pos - it->off - 1));
		return it->pos > 0;
	}
	return true;
}

char *st_iter_chunk(const SliceIter *it, size_t *len)
{
	*len = it->leaf->spans[it->node_offset];
	return it->data - it->off;
}

char st_iter_byte(const SliceIter *it)
{
	return iter_off_end(it) ? -1 : it->data[0];
}

char st_iter_next_byte(SliceIter *it, size_t count)
{
	if(iter_off_end(it))
		return -1;

	size_t left = it->leaf->spans[it->node_offset] - it->off;
	if(count < left) {
		it->off += count;
		it->data += count;
		it->pos += count;
		return *it->data;
	} // cursor ends up off end if no next chunk
	st_dbg("iter_next_byte: wanted %zd, had %zd\n", count, left);
	st_iter_next_chunk(it);
	return st_iter_next_byte(it, count - left);
}

char st_iter_prev_byte(SliceIter *it, size_t count)
{
	if(it->pos == 0)
		return -1;

	size_t left = it->off;
	if(count <= left) {
		it->off -= count;
		it->data -= count;
		it->pos -= count;
		return *it->data;
	}
	st_dbg("iter_prev_byte: wanted %zd, had %zd\n", count, left);
	st_iter_prev_chunk(it);
	return st_iter_prev_byte(it, count - left);
}

#define MBERR ((size_t)-1)
#define MBPART ((size_t)-2)

long st_iter_cp(const SliceIter *it);
long st_iter_next_cp(SliceIter *it, size_t count);
long st_iter_prev_cp(SliceIter *it, size_t count);

bool st_iter_next_line(SliceIter *it, size_t count);
bool st_iter_prev_line(SliceIter *it, size_t count);

size_t st_iter_visual_col(const SliceIter *it);

/* debugging */

void st_print_struct_sizes(void)
{
	printf(
		"Implementation: \e[38;5;1mpersistent btree\e[0m with B=%u\n"
		"sizeof(struct node): %zd\n"
		"sizeof(PieceTable): %zd\n",
		B, sizeof(struct node), sizeof(SliceTable)
	);
}

static void print_node(const struct node *node, int level)
{
	char out[256], *it = out;

	it += sprintf(it, "[");
	if(level == 1) {
		for(int i = 0; i < B; i++) {
			size_t key = node->spans[i];
			if(key != ULONG_MAX)
				it += sprintf(it, "\e[38;5;%dm%lu|",
							node->spans[i] <= HIGH_WATER ? 2 : 1, key);
			else
				it += sprintf(it, "\e[0mNUL|");
		}
	} else {
		for(int i = 0; i < B; i++) {
			size_t key = node->spans[i];
			it += sprintf(it, (key == ULONG_MAX) ? "NUL|" : "%lu|", key);
		}
	}
	it--;
	sprintf(it, "]");
	fprintf(stderr, "%s ", out);
}

static bool check_recurse(struct node *root, int height, int level)
{
	int fill = node_fill(root, 0);
	if(level == 1) {
		bool fillcheck = (height == 1) || fill >= B/2 + (B&1);
		if(!fillcheck) {
			st_dbg("leaf fill violation in ");
			print_node(root, 1);
			return false;
		}

		bool last_issmall = false, issmall;
		for(int i = 0; i < fill; i++) {
			size_t span = root->spans[i];
			if(span == 0) {
				st_dbg("zero span in ");
				print_node(root, 1);
				return false;
			}
			issmall = (span <= HIGH_WATER);
			if(last_issmall && issmall) {
				st_dbg("adjacent slice size violation in slot %d of ", i);
				print_node(root, 1);
				return false;
			}
			last_issmall = issmall;
		}
		return true;
	} else {
		bool fillcheck = fill >= (level == height ? 2 : B/2 + (B&1));
		if(!fillcheck) {
			st_dbg("node fill violation in ");
			print_node(root, 2);
			return false;
		}

		for(int i = 0; i < fill; i++) {
			struct node *child = root->child[i];
			int childlevel = level - 1;
			if(!check_recurse(child, height, childlevel))
				return false;

			size_t spansum;
			if(childlevel == 1) {
				size_t fill = node_fill(child, 0);
				spansum = node_sum(child, fill);
			} else {
				size_t fill = node_fill(child, 0);
				spansum = node_sum(child, fill);
			}
			if(spansum != root->spans[i]) {
				st_dbg("child span violation in slot %d of ", i);
				print_node(root, 2);
				st_dbg("with child sum: %zd span %zd\n",spansum,root->spans[i]);
				return false;
			}
		}
		return true;
	}
}

bool st_check_invariants(const SliceTable *st)
{
	return check_recurse(st->root, st->levels, st->levels);
}

/* global queue */

struct q {
	int level;
	struct node *node;
};

#define QSIZE 100000
struct q queue[QSIZE]; // a ring buffer
int tail = 0, head = 0;

static void enqueue(struct q q) {
	assert(head == tail || head % QSIZE != tail % QSIZE);
	queue[head++ % QSIZE] = q;
}

static struct q *dequeue(void) {
	return (tail == head) ? NULL : &queue[tail++ % QSIZE];
}

void st_pprint(const SliceTable *st)
{
	enqueue((struct q){ st->levels, st->root });
	struct q *next;
	int lastlevel = 1;
	while((next = dequeue())) {
		if(lastlevel != next->level)
			puts("");
		print_node(next->node, next->level);
		if(next->level > 1)
			for(int i = 0; i < node_fill(next->node, 0); i++)
				enqueue((struct q){ next->level-1, next->node->child[i] });
		lastlevel = next->level;
	}
	puts("");
}

void st_dump(const SliceTable *st, FILE *file)
{
	enqueue((struct q){ st->levels, st->root });
	struct q *next;
	while((next = dequeue()))
		if(next->level > 1)
			for(int i = 0; i < node_fill(next->node, 0); i++)
				enqueue((struct q){ next->level-1, next->node->child[i] });
		else // start dumping
			for(int i = 0; i < node_fill(next->node, 0); i++)
				fprintf(file, "%.*s", (int)next->node->spans[i], // TODO write
						(char *)next->node->child[i]);
}

/* dot output */

#include "dot.h"

static void leaf_to_dot(FILE *file, const struct node *leaf)
{
	char *tmp = NULL, *port = NULL;
	graph_table_begin(file, leaf, "aquamarine3");

	for(int i = 0; i < B; i++) {
		size_t key = leaf->spans[i];
		if(key != ULONG_MAX) {
			FSTR(tmp, "%lu", key);
			graph_table_entry(file, tmp, NULL);
		} else
			graph_table_entry(file, NULL, NULL);
	}
	for(int i = 0; i < B; i++) {
		if(leaf->child[i]) {
			FSTR(tmp, "%.*s", (int)leaf->spans[i], (char *)leaf->child[i]);
			graph_table_entry(file, tmp, NULL);
		} else
			graph_table_entry(file, NULL, NULL);
	}
	graph_table_end(file);
	free(tmp);
	free(port);
}

static void node_to_dot(FILE *file, const struct node *root, int height)
{
	if(!root)
		return;
	if(height == 1)
		return leaf_to_dot(file, (struct node *)root);

	char *tmp = NULL, *port = NULL;
	graph_table_begin(file, root, NULL);

	for(int i = 0; i < B; i++) {
		size_t key = root->spans[i];
		if(key != ULONG_MAX) {
			FSTR(tmp, "%lu", key);
			FSTR(port, "%u", i);
		} else
			tmp = port = NULL;
		graph_table_entry(file, tmp, port);
	}
	graph_table_end(file);

	for(int i = 0; i < B; i++) {
		struct node *child = root->child[i];
		if(!child)
			break;
		FSTR(tmp, "%d", i);
		graph_link(file, root, tmp, child, "body");
		node_to_dot(file, child, height - 1);
	}
	free(tmp);
	free(port);
}

bool st_to_dot(const SliceTable *st, const char *path)
{
	char *tmp = NULL;
	FILE *file = fopen(path, "w");
	if(!file)
		goto fail;
	graph_begin(file);

	graph_table_begin(file, st, NULL);
	FSTR(tmp, "height: %u", st->levels);
	graph_table_entry(file, tmp, NULL);
	graph_table_entry(file, "root", "root");
	graph_table_end(file);

	graph_link(file, st, "root", st->root, "body");
	if(st->root)
		node_to_dot(file, st->root, st->levels);

	graph_end(file);
	free(tmp);
	if(fclose(file))
		goto fail;
	return true;

fail:
	perror("st_to_dot");
	return false;
}
