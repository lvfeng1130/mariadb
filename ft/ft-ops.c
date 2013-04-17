/* -*- mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
#ident "Copyright (c) 2007-2010 Tokutek Inc.  All rights reserved."
#ident "The technology is licensed by the Massachusetts Institute of Technology, Rutgers State University of New Jersey, and the Research Foundation of State University of New York at Stony Brook under United States of America Serial No. 11/760379 and to the patents and/or patent applications resulting from it."

/*

Managing the tree shape:  How insertion, deletion, and querying work

When we insert a message into the FT_HANDLE, here's what happens.

to insert a message at the root

    - find the root node
    - capture the next msn of the root node and assign it to the message
    - split the root if it needs to be split
    - insert the message into the root buffer
    - if the root is too full, then flush_some_child() of the root on a flusher thread

flusher functions use an advice struct with provides some functions to
call that tell it what to do based on the context of the flush. see ft-flusher.h

to flush some child, given a parent and some advice
    - pick the child using advice->pick_child()
    - remove that childs buffer from the parent
    - flush the buffer to the child
    - if the child has stable reactivity and
      advice->should_recursively_flush() is true, then
      flush_some_child() of the child
    - otherwise split the child if it needs to be split
    - otherwise maybe merge the child if it needs to be merged

flusher threads:

    flusher threads are created on demand as the result of internal nodes
    becoming gorged by insertions. this allows flushing to be done somewhere
    other than the client thread. these work items are enqueued onto
    the cachetable kibbutz and are done in a first in first out order.

cleaner threads:

    the cleaner thread wakes up every so often (say, 1 second) and chooses
    a small number (say, 5) of nodes as candidates for a flush. the one
    with the largest cache pressure is chosen to be flushed. cache pressure
    is a function of the size of the node in the cachetable plus the work done.
    the cleaner thread need not actually do a flush when awoken, so only
    nodes that have sufficient cache pressure are flushed.

checkpointing:

    the checkpoint thread wakes up every minute to checkpoint dirty nodes
    to disk. at the time of this writing, nodes during checkpoint are
    locked and cannot be queried or flushed to. a design in which nodes
    are copied before checkpoint is being considered as a way to reduce
    the performance variability caused by a checkpoint locking too
    many nodes and preventing other threads from traversing down the tree,
    for a query or otherwise.

To shrink a file: Let X be the size of the reachable data.
    We define an acceptable bloat constant of C.  For example we set C=2 if we are willing to allow the file to be as much as 2X in size.
    The goal is to find the smallest amount of stuff we can move to get the file down to size CX.
    That seems like a difficult problem, so we use the following heuristics:
       If we can relocate the last block to an lower location, then do so immediately.        (The file gets smaller right away, so even though the new location
         may even not be in the first CX bytes, we are making the file smaller.)
       Otherwise all of the earlier blocks are smaller than the last block (of size L).         So find the smallest region that has L free bytes in it.
         (This can be computed in one pass)
         Move the first allocated block in that region to some location not in the interior of the region.
               (Outside of the region is OK, and reallocating the block at the edge of the region is OK).
            This has the effect of creating a smaller region with at least L free bytes in it.
         Go back to the top (because by now some other block may have been allocated or freed).
    Claim: if there are no other allocations going on concurrently, then this algorithm will shrink the file reasonably efficiently.  By this I mean that
       each block of shrinkage does the smallest amount of work possible.  That doesn't mean that the work overall is minimized.
    Note: If there are other allocations and deallocations going on concurrently, we might never get enough space to move the last block.  But it takes a lot
      of allocations and deallocations to make that happen, and it's probably reasonable for the file not to shrink in this case.

To split or merge a child of a node:
Split_or_merge (node, childnum) {
  If the child needs to be split (it's a leaf with too much stuff or a nonleaf with too much fanout)
    fetch the node and the child into main memory.
    split the child, producing two nodes A and B, and also a pivot.   Don't worry if the resulting child is still too big or too small.         Fix it on the next pass.
    fixup node to point at the two new children.  Don't worry about the node getting too much fanout.
    return;
  If the child needs to be merged (it's a leaf with too little stuff (less than 1/4 full) or a nonleaf with too little fanout (less than 1/4)
    fetch node, the child  and a sibling of the child into main memory.
    move all messages from the node to the two children (so that the FIFOs are empty)
    If the two siblings together fit into one node then
      merge the two siblings.
      fixup the node to point at one child
    Otherwise
      load balance the content of the two nodes
    Don't worry about the resulting children having too many messages or otherwise being too big or too small.        Fix it on the next pass.
  }
}

Here's how querying works:

lookups:
    - As of Dr. No, we don't do any tree shaping on lookup.
    - We don't promote eagerly or use aggressive promotion or passive-aggressive
    promotion.        We just push messages down according to the traditional FT_HANDLE
    algorithm on insertions.
    - when a node is brought into memory, we apply ancestor messages above it.

basement nodes, bulk fetch,  and partial fetch:
    - leaf nodes are comprised of N basement nodes, each of nominal size. when
    a query hits a leaf node. it may require one or more basement nodes to be in memory.
    - for point queries, we do not read the entire node into memory. instead,
      we only read in the required basement node
    - for range queries, cursors may return cursor continue in their callback
      to take a the shortcut path until the end of the basement node.
    - for range queries, cursors may prelock a range of keys (with or without a txn).
      the fractal tree will prefetch nodes aggressively until the end of the range.
    - without a prelocked range, range queries behave like successive point queries.

*/

#include "includes.h"
#include "checkpoint.h"
#include "mempool.h"
// Access to nested transaction logic
#include "ule.h"
#include "xids.h"
#include "sub_block.h"
#include "sort.h"
#include <ft-cachetable-wrappers.h>
#include <ft-flusher.h>
#include <valgrind/helgrind.h>

#if defined(HAVE_CILK)
#include <cilk/cilk.h>
#define cilk_worker_count (__cilkrts_get_nworkers())
#else
#define cilk_spawn
#define cilk_sync
#define cilk_for for
#define cilk_worker_count 1
#endif

static const uint32_t this_version = FT_LAYOUT_VERSION;

/* Status is intended for display to humans to help understand system behavior.
 * It does not need to be perfectly thread-safe.
 */
static volatile FT_STATUS_S ft_status;

#define STATUS_INIT(k,t,l) {                            \
        ft_status.status[k].keyname = #k;              \
        ft_status.status[k].type    = t;               \
        ft_status.status[k].legend  = "brt: " l;       \
    }

static toku_mutex_t ft_open_close_lock;

static void
status_init(void)
{
    // Note, this function initializes the keyname, type, and legend fields.
    // Value fields are initialized to zero by compiler.
    STATUS_INIT(FT_UPDATES,                                UINT64, "dictionary updates");
    STATUS_INIT(FT_UPDATES_BROADCAST,                      UINT64, "dictionary broadcast updates");
    STATUS_INIT(FT_DESCRIPTOR_SET,                         UINT64, "descriptor set");
    STATUS_INIT(FT_PARTIAL_EVICTIONS_NONLEAF,              UINT64, "nonleaf node partial evictions");
    STATUS_INIT(FT_PARTIAL_EVICTIONS_LEAF,                 UINT64, "leaf node partial evictions");
    STATUS_INIT(FT_MSN_DISCARDS,                           UINT64, "messages ignored by leaf due to msn");
    STATUS_INIT(FT_MAX_WORKDONE,                           UINT64, "max workdone over all buffers");
    STATUS_INIT(FT_TOTAL_RETRIES,                          UINT64, "total search retries due to TRY_AGAIN");
    STATUS_INIT(FT_MAX_SEARCH_EXCESS_RETRIES,              UINT64, "max excess search retries (retries - tree height) due to TRY_AGAIN");
    STATUS_INIT(FT_SEARCH_TRIES_GT_HEIGHT,                 UINT64, "searches requiring more tries than the height of the tree");
    STATUS_INIT(FT_SEARCH_TRIES_GT_HEIGHTPLUS3,            UINT64, "searches requiring more tries than the height of the tree plus three");
    STATUS_INIT(FT_DISK_FLUSH_LEAF,                        UINT64, "leaf nodes flushed to disk (not for checkpoint)");
    STATUS_INIT(FT_DISK_FLUSH_NONLEAF,                     UINT64, "nonleaf nodes flushed to disk (not for checkpoint)");
    STATUS_INIT(FT_DISK_FLUSH_LEAF_FOR_CHECKPOINT,         UINT64, "leaf nodes flushed to disk (for checkpoint)");
    STATUS_INIT(FT_DISK_FLUSH_NONLEAF_FOR_CHECKPOINT,      UINT64, "nonleaf nodes flushed to disk (for checkpoint)");
    STATUS_INIT(FT_CREATE_LEAF,                            UINT64, "leaf nodes created");
    STATUS_INIT(FT_CREATE_NONLEAF,                         UINT64, "nonleaf nodes created");
    STATUS_INIT(FT_DESTROY_LEAF,                           UINT64, "leaf nodes destroyed");
    STATUS_INIT(FT_DESTROY_NONLEAF,                        UINT64, "nonleaf nodes destroyed");
    STATUS_INIT(FT_MSG_BYTES_IN,                           UINT64, "bytes of messages injected at root (all trees)");
    STATUS_INIT(FT_MSG_BYTES_OUT,                          UINT64, "bytes of messages flushed from h1 nodes to leaves");
    STATUS_INIT(FT_MSG_BYTES_CURR,                         UINT64, "bytes of messages currently in trees (estimate)");
    STATUS_INIT(FT_MSG_BYTES_MAX,                          UINT64, "max bytes of messages ever in trees (estimate)");
    STATUS_INIT(FT_MSG_NUM,                                UINT64, "messages injected at root");
    STATUS_INIT(FT_MSG_NUM_BROADCAST,                      UINT64, "broadcast messages injected at root");
    STATUS_INIT(FT_NUM_BASEMENTS_DECOMPRESSED_NORMAL,      UINT64, "basements decompressed as a target of a query");
    STATUS_INIT(FT_NUM_BASEMENTS_DECOMPRESSED_AGGRESSIVE,  UINT64, "basements decompressed for prelocked range");
    STATUS_INIT(FT_NUM_BASEMENTS_DECOMPRESSED_PREFETCH,    UINT64, "basements decompressed for prefetch");
    STATUS_INIT(FT_NUM_BASEMENTS_DECOMPRESSED_WRITE,       UINT64, "basements decompressed for write");
    STATUS_INIT(FT_NUM_MSG_BUFFER_DECOMPRESSED_NORMAL,     UINT64, "buffers decompressed as a target of a query");
    STATUS_INIT(FT_NUM_MSG_BUFFER_DECOMPRESSED_AGGRESSIVE, UINT64, "buffers decompressed for prelocked range");
    STATUS_INIT(FT_NUM_MSG_BUFFER_DECOMPRESSED_PREFETCH,   UINT64, "buffers decompressed for prefetch");
    STATUS_INIT(FT_NUM_MSG_BUFFER_DECOMPRESSED_WRITE,      UINT64, "buffers decompressed for write");
    STATUS_INIT(FT_NUM_PIVOTS_FETCHED_QUERY,               UINT64, "pivots fetched for query");
    STATUS_INIT(FT_NUM_PIVOTS_FETCHED_PREFETCH,            UINT64, "pivots fetched for prefetch");
    STATUS_INIT(FT_NUM_PIVOTS_FETCHED_WRITE,               UINT64, "pivots fetched for write");
    STATUS_INIT(FT_NUM_BASEMENTS_FETCHED_NORMAL,           UINT64, "basements fetched as a target of a query");
    STATUS_INIT(FT_NUM_BASEMENTS_FETCHED_AGGRESSIVE,       UINT64, "basements fetched for prelocked range");
    STATUS_INIT(FT_NUM_BASEMENTS_FETCHED_PREFETCH,         UINT64, "basements fetched for prefetch");
    STATUS_INIT(FT_NUM_BASEMENTS_FETCHED_WRITE,            UINT64, "basements fetched for write");
    STATUS_INIT(FT_NUM_MSG_BUFFER_FETCHED_NORMAL,          UINT64, "buffers fetched as a target of a query");
    STATUS_INIT(FT_NUM_MSG_BUFFER_FETCHED_AGGRESSIVE,      UINT64, "buffers fetched for prelocked range");
    STATUS_INIT(FT_NUM_MSG_BUFFER_FETCHED_PREFETCH,        UINT64, "buffers fetched for prefetch");
    STATUS_INIT(FT_NUM_MSG_BUFFER_FETCHED_WRITE,           UINT64, "buffers fetched for write");

    ft_status.initialized = true;
}
#undef STATUS_INIT

void
toku_ft_get_status(FT_STATUS s) {
    if (!ft_status.initialized) {
        status_init();
    }
    *s = ft_status;
}

#define STATUS_VALUE(x) ft_status.status[x].value.num

bool is_entire_node_in_memory(FTNODE node) {
    for (int i = 0; i < node->n_children; i++) {
        if(BP_STATE(node,i) != PT_AVAIL) {
            return false;
        }
    }
    return true;
}

void
toku_assert_entire_node_in_memory(FTNODE node) {
    assert(is_entire_node_in_memory(node));
}

static u_int32_t
get_leaf_num_entries(FTNODE node) {
    u_int32_t result = 0;
    int i;
    toku_assert_entire_node_in_memory(node);
    for ( i = 0; i < node->n_children; i++) {
        result += toku_omt_size(BLB_BUFFER(node, i));
    }
    return result;
}

static enum reactivity
get_leaf_reactivity (FTNODE node) {
    enum reactivity re = RE_STABLE;
    toku_assert_entire_node_in_memory(node);
    assert(node->height==0);
    unsigned int size = toku_serialize_ftnode_size(node);
    if (size > node->nodesize && get_leaf_num_entries(node) > 1) {
        re = RE_FISSIBLE;
    }
    else if ((size*4) < node->nodesize && !BLB_SEQINSERT(node, node->n_children-1)) {
        re = RE_FUSIBLE;
    }
    return re;
}

enum reactivity
get_nonleaf_reactivity (FTNODE node) {
    assert(node->height>0);
    int n_children = node->n_children;
    if (n_children > TREE_FANOUT) return RE_FISSIBLE;
    if (n_children*4 < TREE_FANOUT) return RE_FUSIBLE;
    return RE_STABLE;
}

enum reactivity
get_node_reactivity (FTNODE node) {
    toku_assert_entire_node_in_memory(node);
    if (node->height==0)
        return get_leaf_reactivity(node);
    else
        return get_nonleaf_reactivity(node);
}

unsigned int
toku_bnc_nbytesinbuf(NONLEAF_CHILDINFO bnc)
{
    return toku_fifo_buffer_size_in_use(bnc->buffer);
}

// return TRUE if the size of the buffers plus the amount of work done is large enough.   (But return false if there is nothing to be flushed (the buffers empty)).
bool
toku_ft_nonleaf_is_gorged (FTNODE node) {
    u_int64_t size = toku_serialize_ftnode_size(node);

    bool buffers_are_empty = TRUE;
    toku_assert_entire_node_in_memory(node);
    //
    // the nonleaf node is gorged if the following holds true:
    //  - the buffers are non-empty
    //  - the total workdone by the buffers PLUS the size of the buffers
    //     is greater than node->nodesize (which as of Maxwell should be
    //     4MB)
    //
    assert(node->height > 0);
    for (int child = 0; child < node->n_children; ++child) {
        size += BP_WORKDONE(node, child);
    }
    for (int child = 0; child < node->n_children; ++child) {
        if (toku_bnc_nbytesinbuf(BNC(node, child)) > 0) {
            buffers_are_empty = FALSE;
            break;
        }
    }
    return ((size > node->nodesize)
            &&
            (!buffers_are_empty));
}

static void ft_verify_flags(FT ft, FTNODE node) {
    assert(ft->h->flags == node->flags);
}

int toku_ft_debug_mode = 0;

u_int32_t compute_child_fullhash (CACHEFILE cf, FTNODE node, int childnum) {
    assert(node->height>0 && childnum<node->n_children);
    return toku_cachetable_hash(cf, BP_BLOCKNUM(node, childnum));
}

// TODO: (Zardosht) look into this and possibly fix and use
static void __attribute__((__unused__))
ft_leaf_check_leaf_stats (FTNODE node)
{
    assert(node);
    assert(FALSE);
    // static int count=0; count++;
    // if (node->height>0) return;
    // struct subtree_estimates e = calc_leaf_stats(node);
    // assert(e.ndata == node->u.l.leaf_stats.ndata);
    // assert(e.nkeys == node->u.l.leaf_stats.nkeys);
    // assert(e.dsize == node->u.l.leaf_stats.dsize);
    // assert(node->u.l.leaf_stats.exact);
}

int
toku_bnc_n_entries(NONLEAF_CHILDINFO bnc)
{
    return toku_fifo_n_entries(bnc->buffer);
}

static const DBT *prepivotkey (FTNODE node, int childnum, const DBT * const lower_bound_exclusive) {
    if (childnum==0)
        return lower_bound_exclusive;
    else {
        return &node->childkeys[childnum-1];
    }
}

static const DBT *postpivotkey (FTNODE node, int childnum, const DBT * const upper_bound_inclusive) {
    if (childnum+1 == node->n_children)
        return upper_bound_inclusive;
    else {
        return &node->childkeys[childnum];
    }
}
static struct pivot_bounds next_pivot_keys (FTNODE node, int childnum, struct pivot_bounds const * const old_pb) {
    struct pivot_bounds pb = {.lower_bound_exclusive = prepivotkey(node, childnum, old_pb->lower_bound_exclusive),
                              .upper_bound_inclusive = postpivotkey(node, childnum, old_pb->upper_bound_inclusive)};
    return pb;
}

// how much memory does this child buffer consume?
long
toku_bnc_memory_size(NONLEAF_CHILDINFO bnc)
{
    return (sizeof(*bnc) +
            toku_fifo_memory_footprint(bnc->buffer) +
            toku_omt_memory_size(bnc->fresh_message_tree) +
            toku_omt_memory_size(bnc->stale_message_tree) +
            toku_omt_memory_size(bnc->broadcast_list));
}

// how much memory in this child buffer holds useful data?
// originally created solely for use by test program(s).
long
toku_bnc_memory_used(NONLEAF_CHILDINFO bnc)
{
    return (sizeof(*bnc) +
            toku_fifo_memory_size_in_use(bnc->buffer) +
            toku_omt_memory_size(bnc->fresh_message_tree) +
            toku_omt_memory_size(bnc->stale_message_tree) +
            toku_omt_memory_size(bnc->broadcast_list));
}

static long
get_avail_internal_node_partition_size(FTNODE node, int i)
{
    assert(node->height > 0);
    return toku_bnc_memory_size(BNC(node, i));
}


static long
ftnode_cachepressure_size(FTNODE node)
{
    long retval = 0;
    bool totally_empty = true;
    if (node->height == 0) {
        goto exit;
    }
    else {
        for (int i = 0; i < node->n_children; i++) {
            if (BP_STATE(node,i) == PT_INVALID || BP_STATE(node,i) == PT_ON_DISK) {
                continue;
            }
            else if (BP_STATE(node,i) == PT_COMPRESSED) {
                SUB_BLOCK sb = BSB(node, i);
                totally_empty = false;
                retval += sb->compressed_size;
            }
            else if (BP_STATE(node,i) == PT_AVAIL) {
                totally_empty = totally_empty && (toku_bnc_n_entries(BNC(node, i)) == 0);
                retval += get_avail_internal_node_partition_size(node, i);
                retval += BP_WORKDONE(node, i);
            }
            else {
                assert(FALSE);
            }
        }
    }
exit:
    if (totally_empty) {
        return 0;
    }
    return retval;
}

long
ftnode_memory_size (FTNODE node)
// Effect: Estimate how much main memory a node requires.
{
    long retval = 0;
    int n_children = node->n_children;
    retval += sizeof(*node);
    retval += (n_children)*(sizeof(node->bp[0]));
    retval += node->totalchildkeylens;

    // now calculate the sizes of the partitions
    for (int i = 0; i < n_children; i++) {
        if (BP_STATE(node,i) == PT_INVALID || BP_STATE(node,i) == PT_ON_DISK) {
            continue;
        }
        else if (BP_STATE(node,i) == PT_COMPRESSED) {
            SUB_BLOCK sb = BSB(node, i);
            retval += sizeof(*sb);
            retval += sb->compressed_size;
        }
        else if (BP_STATE(node,i) == PT_AVAIL) {
            if (node->height > 0) {
                retval += get_avail_internal_node_partition_size(node, i);
            }
            else {
                BASEMENTNODE bn = BLB(node, i);
                retval += sizeof(*bn);
                {
                    // include fragmentation overhead but do not include space in the
                    // mempool that has not yet been allocated for leaf entries
                    size_t poolsize = toku_mempool_footprint(&bn->buffer_mempool);
                    invariant (poolsize >= BLB_NBYTESINBUF(node,i));
                    retval += poolsize;
                }
                OMT curr_omt = BLB_BUFFER(node, i);
                retval += (toku_omt_memory_size(curr_omt));
            }
        }
        else {
            assert(FALSE);
        }
    }
    return retval;
}

PAIR_ATTR make_ftnode_pair_attr(FTNODE node) {
    long size = ftnode_memory_size(node);
    long cachepressure_size = ftnode_cachepressure_size(node);
    PAIR_ATTR result={
        .size = size,
        .nonleaf_size = (node->height > 0) ? size : 0,
        .leaf_size = (node->height > 0) ? 0 : size,
        .rollback_size = 0,
        .cache_pressure_size = cachepressure_size,
        .is_valid = TRUE
    };
    return result;
}

PAIR_ATTR make_invalid_pair_attr(void) {
    PAIR_ATTR result={
        .size = 0,
        .nonleaf_size = 0,
        .leaf_size = 0,
        .rollback_size = 0,
        .cache_pressure_size = 0,
        .is_valid = FALSE
    };
    return result;
}


// assign unique dictionary id
static uint64_t dict_id_serial = 1;
static DICTIONARY_ID
next_dict_id(void) {
    uint64_t i = __sync_fetch_and_add(&dict_id_serial, 1);
    assert(i);        // guarantee unique dictionary id by asserting 64-bit counter never wraps
    DICTIONARY_ID d = {.dictid = i};
    return d;
}

//
// Given a bfe and a childnum, returns whether the query that constructed the bfe
// wants the child available.
// Requires: bfe->child_to_read to have been set
//
bool
toku_bfe_wants_child_available (struct ftnode_fetch_extra* bfe, int childnum)
{
    if (bfe->type == ftnode_fetch_all ||
        (bfe->type == ftnode_fetch_subset && bfe->child_to_read == childnum))
    {
        return true;
    }
    else {
        return false;
    }
}

int
toku_bfe_leftmost_child_wanted(struct ftnode_fetch_extra *bfe, FTNODE node)
{
    lazy_assert(bfe->type == ftnode_fetch_subset || bfe->type == ftnode_fetch_prefetch);
    if (bfe->left_is_neg_infty) {
        return 0;
    } else if (bfe->range_lock_left_key == NULL) {
        return -1;
    } else {
        return toku_ftnode_which_child(node, bfe->range_lock_left_key, &bfe->h->cmp_descriptor, bfe->h->compare_fun);
    }
}

int
toku_bfe_rightmost_child_wanted(struct ftnode_fetch_extra *bfe, FTNODE node)
{
    lazy_assert(bfe->type == ftnode_fetch_subset || bfe->type == ftnode_fetch_prefetch);
    if (bfe->right_is_pos_infty) {
        return node->n_children - 1;
    } else if (bfe->range_lock_right_key == NULL) {
        return -1;
    } else {
        return toku_ftnode_which_child(node, bfe->range_lock_right_key, &bfe->h->cmp_descriptor, bfe->h->compare_fun);
    }
}

static int
ft_cursor_rightmost_child_wanted(FT_CURSOR cursor, FT_HANDLE brt, FTNODE node)
{
    if (cursor->right_is_pos_infty) {
        return node->n_children - 1;
    } else if (cursor->range_lock_right_key.data == NULL) {
        return -1;
    } else {
        return toku_ftnode_which_child(node, &cursor->range_lock_right_key, &brt->ft->cmp_descriptor, brt->ft->compare_fun);
    }
}

STAT64INFO_S
toku_get_and_clear_basement_stats(FTNODE leafnode) {
    invariant(leafnode->height == 0);
    STAT64INFO_S deltas = ZEROSTATS;
    for (int i = 0; i < leafnode->n_children; i++) {
        BASEMENTNODE bn = BLB(leafnode, i);
        invariant(BP_STATE(leafnode,i) == PT_AVAIL);
        deltas.numrows  += bn->stat64_delta.numrows;
        deltas.numbytes += bn->stat64_delta.numbytes;
        bn->stat64_delta = ZEROSTATS;
    }
    return deltas;
}

static void ft_status_update_flush_reason(FTNODE node, BOOL for_checkpoint) {
    if (node->height == 0) {
        if (for_checkpoint) {
            __sync_fetch_and_add(&STATUS_VALUE(FT_DISK_FLUSH_LEAF_FOR_CHECKPOINT), 1);
        }
        else {
            __sync_fetch_and_add(&STATUS_VALUE(FT_DISK_FLUSH_LEAF), 1);
        }
    }
    else {
        if (for_checkpoint) {
            __sync_fetch_and_add(&STATUS_VALUE(FT_DISK_FLUSH_NONLEAF_FOR_CHECKPOINT), 1);
        }
        else {
            __sync_fetch_and_add(&STATUS_VALUE(FT_DISK_FLUSH_NONLEAF), 1);
        }
    }
}

static void ftnode_update_disk_stats(
    FTNODE ftnode,
    FT ft,
    BOOL for_checkpoint
    )
{
    STAT64INFO_S deltas = ZEROSTATS;
    // capture deltas before rebalancing basements for serialization
    deltas = toku_get_and_clear_basement_stats(ftnode);
    // locking not necessary here with respect to checkpointing
    // in Clayface (because of the pending lock and cachetable lock
    // in toku_cachetable_begin_checkpoint)
    // essentially, if we are dealing with a for_checkpoint 
    // parameter in a function that is called by the flush_callback,
    // then the cachetable needs to ensure that this is called in a safe
    // manner that does not interfere with the beginning
    // of a checkpoint, which it does with the cachetable lock
    // and pending lock
    toku_ft_update_stats(&ft->h->on_disk_stats, deltas);
    if (for_checkpoint) {
        toku_ft_update_stats(&ft->checkpoint_header->on_disk_stats, deltas);
    }
}

static void ftnode_clone_partitions(FTNODE node, FTNODE cloned_node) {
    for (int i = 0; i < node->n_children; i++) {
        BP_BLOCKNUM(cloned_node,i) = BP_BLOCKNUM(node,i);
        assert(BP_STATE(node,i) == PT_AVAIL);
        BP_STATE(cloned_node,i) = PT_AVAIL;
        BP_WORKDONE(cloned_node, i) = BP_WORKDONE(node, i);
        if (node->height == 0) {
            set_BLB(cloned_node, i, toku_clone_bn(BLB(node,i)));
        }
        else {
            set_BNC(cloned_node, i, toku_clone_nl(BNC(node,i)));
        }
    }
}

void toku_ftnode_clone_callback(
    void* value_data,
    void** cloned_value_data,
    PAIR_ATTR* new_attr,
    BOOL for_checkpoint,
    void* write_extraargs
    )
{
    FTNODE node = value_data;
    toku_assert_entire_node_in_memory(node);
    FT ft = write_extraargs;
    FTNODE XMALLOC(cloned_node);
    //FTNODE cloned_node = (FTNODE)toku_xmalloc(sizeof(*FTNODE));
    memset(cloned_node, 0, sizeof(*cloned_node));
    if (node->height == 0) {
        // set header stats, must be done before rebalancing
        ftnode_update_disk_stats(node, ft, for_checkpoint);
        // rebalance the leaf node
        rebalance_ftnode_leaf(node, ft->h->basementnodesize);
    }

    cloned_node->max_msn_applied_to_node_on_disk = node->max_msn_applied_to_node_on_disk;
    cloned_node->nodesize = node->nodesize;
    cloned_node->flags = node->flags;
    cloned_node->thisnodename = node->thisnodename;
    cloned_node->layout_version = node->layout_version;
    cloned_node->layout_version_original = node->layout_version_original;
    cloned_node->layout_version_read_from_disk = node->layout_version_read_from_disk;
    cloned_node->build_id = node->build_id;
    cloned_node->height = node->height;
    cloned_node->dirty = node->dirty;
    cloned_node->fullhash = node->fullhash;
    cloned_node->n_children = node->n_children;
    cloned_node->totalchildkeylens = node->totalchildkeylens;

    XMALLOC_N(node->n_children-1, cloned_node->childkeys);
    XMALLOC_N(node->n_children, cloned_node->bp);
    // clone pivots
    for (int i = 0; i < node->n_children-1; i++) {
        toku_clone_dbt(&cloned_node->childkeys[i], node->childkeys[i]);
    }
    // clone partition
    ftnode_clone_partitions(node, cloned_node);

    // clear dirty bit
    node->dirty = 0;
    cloned_node->dirty = 0;
    node->layout_version_read_from_disk = FT_LAYOUT_VERSION;
    // set new pair attr if necessary
    if (node->height == 0) {
        *new_attr = make_ftnode_pair_attr(node);
    }
    else {
        new_attr->is_valid = FALSE;
    }
    *cloned_value_data = cloned_node;
}


void toku_ftnode_flush_callback (
    CACHEFILE cachefile,
    int fd,
    BLOCKNUM nodename,
    void *ftnode_v,
    void** disk_data,
    void *extraargs,
    PAIR_ATTR size __attribute__((unused)),
    PAIR_ATTR* new_size,
    BOOL write_me,
    BOOL keep_me,
    BOOL for_checkpoint,
    BOOL is_clone
    )
{
    FT h = extraargs;
    FTNODE ftnode = ftnode_v;
    FTNODE_DISK_DATA* ndd = (FTNODE_DISK_DATA*)disk_data;
    assert(ftnode->thisnodename.b==nodename.b);
    int height = ftnode->height;
    if (write_me) {
        if (height == 0 && !is_clone) {
            ftnode_update_disk_stats(ftnode, h, for_checkpoint);
        }
        if (!h->panic) { // if the brt panicked, stop writing, otherwise try to write it.
            toku_assert_entire_node_in_memory(ftnode);
            int n_workitems, n_threads;
            toku_cachefile_get_workqueue_load(cachefile, &n_workitems, &n_threads);
            int r = toku_serialize_ftnode_to(fd, ftnode->thisnodename, ftnode, ndd, !is_clone, h, n_workitems, n_threads, for_checkpoint);
            assert_zero(r);
            ftnode->layout_version_read_from_disk = FT_LAYOUT_VERSION;
        }
        ft_status_update_flush_reason(ftnode, for_checkpoint);
    }
    if (!keep_me) {
        if (!is_clone) {
            toku_free(*disk_data);
        }
        else {
            if (ftnode->height == 0) {
                for (int i = 0; i < ftnode->n_children; i++) {
                    if (BP_STATE(ftnode,i) == PT_AVAIL) {
                        BASEMENTNODE bn = BLB(ftnode, i);
                        toku_ft_decrease_stats(&h->in_memory_stats, bn->stat64_delta);
                    }
                }
            }
        }
        toku_ftnode_free(&ftnode);
    }
    else {
        *new_size = make_ftnode_pair_attr(ftnode);
    }
}

void
toku_ft_status_update_pivot_fetch_reason(struct ftnode_fetch_extra *bfe)
{
    if (bfe->type == ftnode_fetch_prefetch) {
        STATUS_VALUE(FT_NUM_PIVOTS_FETCHED_PREFETCH)++;
    } else if (bfe->type == ftnode_fetch_all) {
        STATUS_VALUE(FT_NUM_PIVOTS_FETCHED_WRITE)++;
    } else if (bfe->type == ftnode_fetch_subset) {
        STATUS_VALUE(FT_NUM_PIVOTS_FETCHED_QUERY)++;
    }
}

int toku_ftnode_fetch_callback (CACHEFILE UU(cachefile), int fd, BLOCKNUM nodename, u_int32_t fullhash,
                                 void **ftnode_pv,  void** disk_data, PAIR_ATTR *sizep, int *dirtyp, void *extraargs) {
    assert(extraargs);
    assert(*ftnode_pv == NULL);
    FTNODE_DISK_DATA* ndd = (FTNODE_DISK_DATA*)disk_data;
    struct ftnode_fetch_extra *bfe = (struct ftnode_fetch_extra *)extraargs;
    FTNODE *node=(FTNODE*)ftnode_pv;
    // deserialize the node, must pass the bfe in because we cannot
    // evaluate what piece of the the node is necessary until we get it at
    // least partially into memory
    int r = toku_deserialize_ftnode_from(fd, nodename, fullhash, node, ndd, bfe);
    if (r != 0) {
        if (r == TOKUDB_BAD_CHECKSUM) {
            fprintf(stderr,
                    "Checksum failure while reading node in file %s.\n",
                    toku_cachefile_fname_in_env(cachefile));
        } else {
            fprintf(stderr, "Error deserializing node, errno = %d", r);
        }
        // make absolutely sure we crash before doing anything else.
        assert(false);
    }

    if (r == 0) {
        *sizep = make_ftnode_pair_attr(*node);
        *dirtyp = (*node)->dirty;  // deserialize could mark the node as dirty (presumably for upgrade)
    }
    return r;
}

void toku_ftnode_pe_est_callback(
    void* ftnode_pv,
    void* disk_data,
    long* bytes_freed_estimate,
    enum partial_eviction_cost *cost,
    void* UU(write_extraargs)
    )
{
    assert(ftnode_pv != NULL);
    long bytes_to_free = 0;
    FTNODE node = (FTNODE)ftnode_pv;
    if (node->dirty || node->height == 0 ||
        node->layout_version_read_from_disk < FT_FIRST_LAYOUT_VERSION_WITH_BASEMENT_NODES) {
        *bytes_freed_estimate = 0;
        *cost = PE_CHEAP;
        goto exit;
    }

    //
    // we are dealing with a clean internal node
    //
    *cost = PE_EXPENSIVE;
    // now lets get an estimate for how much data we can free up
    // we estimate the compressed size of data to be how large
    // the compressed data is on disk
    for (int i = 0; i < node->n_children; i++) {
        if (BP_STATE(node,i) == PT_AVAIL && BP_SHOULD_EVICT(node,i)) {
            // calculate how much data would be freed if
            // we compress this node and add it to
            // bytes_to_free

            // first get an estimate for how much space will be taken
            // after compression, it is simply the size of compressed
            // data on disk plus the size of the struct that holds it
            FTNODE_DISK_DATA ndd = disk_data;
            u_int32_t compressed_data_size = BP_SIZE(ndd, i);
            compressed_data_size += sizeof(struct sub_block);

            // now get the space taken now
            u_int32_t decompressed_data_size = get_avail_internal_node_partition_size(node,i);
            bytes_to_free += (decompressed_data_size - compressed_data_size);
        }
    }

    *bytes_freed_estimate = bytes_to_free;
exit:
    return;
}

static void
compress_internal_node_partition(FTNODE node, int i, enum toku_compression_method compression_method)
{
    // if we should evict, compress the
    // message buffer into a sub_block
    assert(BP_STATE(node, i) == PT_AVAIL);
    assert(node->height > 0);
    SUB_BLOCK sb = NULL;
    sb = toku_xmalloc(sizeof(struct sub_block));
    sub_block_init(sb);
    toku_create_compressed_partition_from_available(node, i, compression_method, sb);

    // now free the old partition and replace it with this
    destroy_nonleaf_childinfo(BNC(node,i));
    set_BSB(node, i, sb);
    BP_STATE(node,i) = PT_COMPRESSED;
}

void toku_evict_bn_from_memory(FTNODE node, int childnum, FT h) {
    // free the basement node
    assert(!node->dirty);
    BASEMENTNODE bn = BLB(node, childnum);
    toku_ft_decrease_stats(&h->in_memory_stats, bn->stat64_delta);
    struct mempool * mp = &bn->buffer_mempool;
    toku_mempool_destroy(mp);
    destroy_basement_node(bn);
    set_BNULL(node, childnum);
    BP_STATE(node, childnum) = PT_ON_DISK;
}

// callback for partially evicting a node
int toku_ftnode_pe_callback (void *ftnode_pv, PAIR_ATTR UU(old_attr), PAIR_ATTR* new_attr, void* extraargs) {
    FTNODE node = (FTNODE)ftnode_pv;
    FT ft = extraargs;
    // Don't partially evict dirty nodes
    if (node->dirty) {
        goto exit;
    }
    // Don't partially evict nodes whose partitions can't be read back
    // from disk individually
    if (node->layout_version_read_from_disk < FT_FIRST_LAYOUT_VERSION_WITH_BASEMENT_NODES) {
        goto exit;
    }
    //
    // partial eviction for nonleaf nodes
    //
    if (node->height > 0) {
        for (int i = 0; i < node->n_children; i++) {
            if (BP_STATE(node,i) == PT_AVAIL) {
                if (BP_SHOULD_EVICT(node,i)) {
                    STATUS_VALUE(FT_PARTIAL_EVICTIONS_NONLEAF)++;
                    cilk_spawn compress_internal_node_partition(node, i, ft->h->compression_method);
                }
                else {
                    BP_SWEEP_CLOCK(node,i);
                }
            }
            else {
                continue;
            }
        }
        cilk_sync;
    }
    //
    // partial eviction strategy for basement nodes:
    //  if the bn is compressed, evict it
    //  else: check if it requires eviction, if it does, evict it, if not, sweep the clock count
    //
    else {
        for (int i = 0; i < node->n_children; i++) {
            // Get rid of compressed stuff no matter what.
            if (BP_STATE(node,i) == PT_COMPRESSED) {
                STATUS_VALUE(FT_PARTIAL_EVICTIONS_LEAF)++;
                SUB_BLOCK sb = BSB(node, i);
                toku_free(sb->compressed_ptr);
                toku_free(sb);
                set_BNULL(node, i);
                BP_STATE(node,i) = PT_ON_DISK;
            }
            else if (BP_STATE(node,i) == PT_AVAIL) {
                if (BP_SHOULD_EVICT(node,i)) {
                    STATUS_VALUE(FT_PARTIAL_EVICTIONS_LEAF)++;
                    toku_evict_bn_from_memory(node, i, ft);
                }
                else {
                    BP_SWEEP_CLOCK(node,i);
                }
            }
            else if (BP_STATE(node,i) == PT_ON_DISK) {
                continue;
            }
            else {
                assert(FALSE);
            }
        }
    }

exit:
    *new_attr = make_ftnode_pair_attr(node);
    return 0;
}


// Callback that states if a partial fetch of the node is necessary
// Currently, this function is responsible for the following things:
//  - reporting to the cachetable whether a partial fetch is required (as required by the contract of the callback)
//  - A couple of things that are NOT required by the callback, but we do for efficiency and simplicity reasons:
//   - for queries, set the value of bfe->child_to_read so that the query that called this can proceed with the query
//      as opposed to having to evaluate toku_ft_search_which_child again. This is done to make the in-memory query faster
//   - touch the necessary partition's clock. The reason we do it here is so that there is one central place it is done, and not done
//      by all the various callers
//
BOOL toku_ftnode_pf_req_callback(void* ftnode_pv, void* read_extraargs) {
    // placeholder for now
    BOOL retval = FALSE;
    FTNODE node = ftnode_pv;
    struct ftnode_fetch_extra *bfe = read_extraargs;
    //
    // The three types of fetches that the brt layer may request are:
    //  - ftnode_fetch_none: no partitions are necessary (example use: stat64)
    //  - ftnode_fetch_subset: some subset is necessary (example use: toku_ft_search)
    //  - ftnode_fetch_all: entire node is necessary (example use: flush, split, merge)
    // The code below checks if the necessary partitions are already in memory,
    // and if they are, return FALSE, and if not, return TRUE
    //
    if (bfe->type == ftnode_fetch_none) {
        retval = FALSE;
    }
    else if (bfe->type == ftnode_fetch_all) {
        retval = FALSE;
        for (int i = 0; i < node->n_children; i++) {
            BP_TOUCH_CLOCK(node,i);
            // if we find a partition that is not available,
            // then a partial fetch is required because
            // the entire node must be made available
            if (BP_STATE(node,i) != PT_AVAIL) {
                retval = TRUE;
            }
        }
    }
    else if (bfe->type == ftnode_fetch_subset) {
        // we do not take into account prefetching yet
        // as of now, if we need a subset, the only thing
        // we can possibly require is a single basement node
        // we find out what basement node the query cares about
        // and check if it is available
        assert(bfe->h->compare_fun);
        assert(bfe->search);
        bfe->child_to_read = toku_ft_search_which_child(
            &bfe->h->cmp_descriptor,
            bfe->h->compare_fun,
            node,
            bfe->search
            );
        BP_TOUCH_CLOCK(node,bfe->child_to_read);
        // child we want to read is not available, must set retval to TRUE
        retval = (BP_STATE(node, bfe->child_to_read) != PT_AVAIL);
    }
    else if (bfe->type == ftnode_fetch_prefetch) {
        // makes no sense to have prefetching disabled
        // and still call this function
        assert(!bfe->disable_prefetching);
        int lc = toku_bfe_leftmost_child_wanted(bfe, node);
        int rc = toku_bfe_rightmost_child_wanted(bfe, node);
        for (int i = lc; i <= rc; ++i) {
            if (BP_STATE(node, i) != PT_AVAIL) {
                retval = TRUE;
            }
        }
    }
    else {
        // we have a bug. The type should be known
        assert(FALSE);
    }
    return retval;
}

static void
ft_status_update_partial_fetch_reason(
    struct ftnode_fetch_extra* UU(bfe),
    int UU(i),
    int UU(state),
    BOOL UU(is_leaf)
    )
{
    invariant(state == PT_COMPRESSED || state == PT_ON_DISK);
    if (is_leaf) {
        if (bfe->type == ftnode_fetch_prefetch) {
            if (state == PT_COMPRESSED) {
                STATUS_VALUE(FT_NUM_BASEMENTS_DECOMPRESSED_PREFETCH)++;
            } else {
                STATUS_VALUE(FT_NUM_BASEMENTS_FETCHED_PREFETCH)++;
            }
        } else if (bfe->type == ftnode_fetch_all) {
            if (state == PT_COMPRESSED) {
                STATUS_VALUE(FT_NUM_BASEMENTS_DECOMPRESSED_WRITE)++;
            } else {
                STATUS_VALUE(FT_NUM_BASEMENTS_FETCHED_WRITE)++;
            }
        } else if (i == bfe->child_to_read) {
            if (state == PT_COMPRESSED) {
                STATUS_VALUE(FT_NUM_BASEMENTS_DECOMPRESSED_NORMAL)++;
            } else {
                STATUS_VALUE(FT_NUM_BASEMENTS_FETCHED_NORMAL)++;
            }
        } else {
            if (state == PT_COMPRESSED) {
                STATUS_VALUE(FT_NUM_BASEMENTS_DECOMPRESSED_AGGRESSIVE)++;
            } else {
                STATUS_VALUE(FT_NUM_BASEMENTS_FETCHED_AGGRESSIVE)++;
            }
        }
    }
    else {
        if (bfe->type == ftnode_fetch_prefetch) {
            if (state == PT_COMPRESSED) {
                STATUS_VALUE(FT_NUM_MSG_BUFFER_DECOMPRESSED_PREFETCH)++;
            } else {
                STATUS_VALUE(FT_NUM_MSG_BUFFER_FETCHED_PREFETCH)++;
            }
        } else if (bfe->type == ftnode_fetch_all) {
            if (state == PT_COMPRESSED) {
                STATUS_VALUE(FT_NUM_MSG_BUFFER_DECOMPRESSED_WRITE)++;
            } else {
                STATUS_VALUE(FT_NUM_MSG_BUFFER_FETCHED_WRITE)++;
            }
        } else if (i == bfe->child_to_read) {
            if (state == PT_COMPRESSED) {
                STATUS_VALUE(FT_NUM_MSG_BUFFER_DECOMPRESSED_NORMAL)++;
            } else {
                STATUS_VALUE(FT_NUM_MSG_BUFFER_FETCHED_NORMAL)++;
            }
        } else {
            if (state == PT_COMPRESSED) {
                STATUS_VALUE(FT_NUM_MSG_BUFFER_DECOMPRESSED_AGGRESSIVE)++;
            } else {
                STATUS_VALUE(FT_NUM_MSG_BUFFER_FETCHED_AGGRESSIVE)++;
            }
        }
    }
}

// callback for partially reading a node
// could have just used toku_ftnode_fetch_callback, but wanted to separate the two cases to separate functions
int toku_ftnode_pf_callback(void* ftnode_pv, void* disk_data, void* read_extraargs, int fd, PAIR_ATTR* sizep) {
    int r = 0;
    FTNODE node = ftnode_pv;
    FTNODE_DISK_DATA ndd = disk_data;
    struct ftnode_fetch_extra *bfe = read_extraargs;
    // there must be a reason this is being called. If we get a garbage type or the type is ftnode_fetch_none,
    // then something went wrong
    assert((bfe->type == ftnode_fetch_subset) || (bfe->type == ftnode_fetch_all) || (bfe->type == ftnode_fetch_prefetch));
    // determine the range to prefetch
    int lc, rc;
    if (!bfe->disable_prefetching &&
        (bfe->type == ftnode_fetch_subset || bfe->type == ftnode_fetch_prefetch)
        )
    {
        lc = toku_bfe_leftmost_child_wanted(bfe, node);
        rc = toku_bfe_rightmost_child_wanted(bfe, node);
    } else {
        lc = -1;
        rc = -1;
    }
    // TODO: possibly cilkify expensive operations in this loop
    // TODO: review this with others to see if it can be made faster
    for (int i = 0; i < node->n_children; i++) {
        if (BP_STATE(node,i) == PT_AVAIL) {
            continue;
        }
        if ((lc <= i && i <= rc) || toku_bfe_wants_child_available(bfe, i)) {
            ft_status_update_partial_fetch_reason(bfe, i, BP_STATE(node, i), (node->height == 0));
            if (BP_STATE(node,i) == PT_COMPRESSED) {
                r = toku_deserialize_bp_from_compressed(node, i, &bfe->h->cmp_descriptor, bfe->h->compare_fun);
            }
            else if (BP_STATE(node,i) == PT_ON_DISK) {
                r = toku_deserialize_bp_from_disk(node, ndd, i, fd, bfe);
            }
            else {
                assert(FALSE);
            }
        }

        if (r != 0) {
            if (r == TOKUDB_BAD_CHECKSUM) {
                fprintf(stderr,
                        "Checksum failure while reading node partition in file %s.\n",
                        toku_cachefile_fname_in_env(bfe->h->cf));
            } else {
                fprintf(stderr,
                        "Error while reading node partition %d\n",
                        errno);
            }
            assert(false);
        }
    }

    *sizep = make_ftnode_pair_attr(node);

    return 0;
}

static int
leafval_heaviside_le (u_int32_t klen, void *kval,
                      struct cmd_leafval_heaviside_extra *be) {
    DBT dbt;
    DBT const * const key = be->key;
    FAKE_DB(db, be->desc);
    return be->compare_fun(&db,
                           toku_fill_dbt(&dbt, kval, klen),
                           key);
}

//TODO: #1125 optimize
int
toku_cmd_leafval_heaviside (OMTVALUE lev, void *extra) {
    LEAFENTRY le=lev;
    struct cmd_leafval_heaviside_extra *be = extra;
    u_int32_t keylen;
    void*     key = le_key_and_len(le, &keylen);
    return leafval_heaviside_le(keylen, key,
                                be);
}

static int
ft_compare_pivot(DESCRIPTOR desc, ft_compare_func cmp, const DBT *key, const DBT *pivot)
{
    int r;
    FAKE_DB(db, desc);
    r = cmp(&db, key, pivot);
    return r;
}


// destroys the internals of the ftnode, but it does not free the values
// that are stored
// this is common functionality for toku_ftnode_free and rebalance_ftnode_leaf
// MUST NOT do anything besides free the structures that have been allocated
void toku_destroy_ftnode_internals(FTNODE node)
{
    for (int i=0; i<node->n_children-1; i++) {
        toku_free(node->childkeys[i].data);
    }
    toku_free(node->childkeys);
    node->childkeys = NULL;

    for (int i=0; i < node->n_children; i++) {
        if (BP_STATE(node,i) == PT_AVAIL) {
            if (node->height > 0) {
                destroy_nonleaf_childinfo(BNC(node,i));
            } else {
                destroy_basement_node(BLB(node, i));
            }
        } else if (BP_STATE(node,i) == PT_COMPRESSED) {
            SUB_BLOCK sb = BSB(node,i);
            toku_free(sb->compressed_ptr);
            toku_free(sb);
        } else {
            assert(is_BNULL(node, i));
        }
        set_BNULL(node, i);
    }
    toku_free(node->bp);
    node->bp = NULL;

}


/* Frees a node, including all the stuff in the hash table. */
void toku_ftnode_free (FTNODE *nodep) {

    //TODO: #1378 Take omt lock (via ftnode) around call to toku_omt_destroy().

    FTNODE node=*nodep;
    if (node->height == 0) {
        for (int i = 0; i < node->n_children; i++) {
            if (BP_STATE(node,i) == PT_AVAIL) {
                struct mempool * mp = &(BLB_BUFFER_MEMPOOL(node, i));
                toku_mempool_destroy(mp);
            }
        }
        STATUS_VALUE(FT_DESTROY_LEAF)++;
    } else {
        STATUS_VALUE(FT_DESTROY_NONLEAF)++;
    }
    toku_destroy_ftnode_internals(node);
    toku_free(node);
    *nodep=0;
}

void
toku_initialize_empty_ftnode (FTNODE n, BLOCKNUM nodename, int height, int num_children, int layout_version, unsigned int nodesize, unsigned int flags)
// Effect: Fill in N as an empty ftnode.
{
    assert(layout_version != 0);
    assert(height >= 0);

    if (height == 0)
        STATUS_VALUE(FT_CREATE_LEAF)++;
    else
        STATUS_VALUE(FT_CREATE_NONLEAF)++;

    n->max_msn_applied_to_node_on_disk = ZERO_MSN;    // correct value for root node, harmless for others
    n->nodesize = nodesize;
    n->flags = flags;
    n->thisnodename = nodename;
    n->layout_version               = layout_version;
    n->layout_version_original = layout_version;
    n->layout_version_read_from_disk = layout_version;
    n->height = height;
    n->totalchildkeylens = 0;
    n->childkeys = 0;
    n->bp = 0;
    n->n_children = num_children;

    if (num_children > 0) {
        XMALLOC_N(num_children-1, n->childkeys);
        XMALLOC_N(num_children, n->bp);
        for (int i = 0; i < num_children; i++) {
            BP_BLOCKNUM(n,i).b=0;
            BP_STATE(n,i) = PT_INVALID;
            BP_WORKDONE(n,i) = 0;
            BP_INIT_TOUCHED_CLOCK(n, i);
            set_BNULL(n,i);
            if (height > 0) {
                set_BNC(n, i, toku_create_empty_nl());
            } else {
                set_BLB(n, i, toku_create_empty_bn());
            }
        }
    }
    n->dirty = 1;  // special case exception, it's okay to mark as dirty because the basements are empty
}

static void
ft_init_new_root(FT ft, FTNODE nodea, FTNODE nodeb, DBT splitk, CACHEKEY *rootp, FTNODE *newrootp)
// Effect:  Create a new root node whose two children are NODEA and NODEB, and the pivotkey is SPLITK.
//  Store the new root's identity in *ROOTP, and the node in *NEWROOTP.
//  Unpin nodea and nodeb.
//  Leave the new root pinned.
{
    FTNODE XMALLOC(newroot);
    int new_height = nodea->height+1;
    BLOCKNUM newroot_diskoff;
    toku_allocate_blocknum(ft->blocktable, &newroot_diskoff, ft);
    assert(newroot);
    *rootp=newroot_diskoff;
    assert(new_height > 0);
    toku_initialize_empty_ftnode (newroot, newroot_diskoff, new_height, 2, ft->h->layout_version, ft->h->nodesize, ft->h->flags);
    //printf("new_root %lld %d %lld %lld\n", newroot_diskoff, newroot->height, nodea->thisnodename, nodeb->thisnodename);
    //printf("%s:%d Splitkey=%p %s\n", __FILE__, __LINE__, splitkey, splitkey);
    toku_copyref_dbt(&newroot->childkeys[0], splitk);
    newroot->totalchildkeylens=splitk.size;
    BP_BLOCKNUM(newroot,0)=nodea->thisnodename;
    BP_BLOCKNUM(newroot,1)=nodeb->thisnodename;
    {
        MSN msna = nodea->max_msn_applied_to_node_on_disk;
        MSN msnb = nodeb->max_msn_applied_to_node_on_disk;
        invariant(msna.msn == msnb.msn);
        newroot->max_msn_applied_to_node_on_disk = msna;
    }
    BP_STATE(newroot,0) = PT_AVAIL;
    BP_STATE(newroot,1) = PT_AVAIL;
    newroot->dirty = 1;
    //printf("%s:%d put %lld\n", __FILE__, __LINE__, newroot_diskoff);
    u_int32_t fullhash = toku_cachetable_hash(ft->cf, newroot_diskoff);
    newroot->fullhash = fullhash;
    toku_cachetable_put(ft->cf, newroot_diskoff, fullhash, newroot, make_ftnode_pair_attr(newroot), get_write_callbacks_for_node(ft));

    //at this point, newroot is associated with newroot_diskoff, nodea is associated with root_blocknum
    // make newroot_diskoff point to nodea
    // make root_blocknum point to newroot
    // also modify the blocknum and fullhash of nodea and newroot
    // before doing this, assert(nodea->blocknum == ft->root_blocknum)
    
    toku_unpin_ftnode(ft, nodea);
    toku_unpin_ftnode(ft, nodeb);
    *newrootp = newroot;
}

static void
init_childinfo(FTNODE node, int childnum, FTNODE child) {
    BP_BLOCKNUM(node,childnum) = child->thisnodename;
    BP_STATE(node,childnum) = PT_AVAIL;
    BP_WORKDONE(node, childnum)   = 0;
    set_BNC(node, childnum, toku_create_empty_nl());
}

static void
init_childkey(FTNODE node, int childnum, const DBT *pivotkey) {
    toku_copyref_dbt(&node->childkeys[childnum], *pivotkey);
    node->totalchildkeylens += pivotkey->size;
}

// Used only by test programs: append a child node to a parent node
void
toku_ft_nonleaf_append_child(FTNODE node, FTNODE child, const DBT *pivotkey) {
    int childnum = node->n_children;
    node->n_children++;
    XREALLOC_N(node->n_children, node->bp);
    init_childinfo(node, childnum, child);
    XREALLOC_N(node->n_children-1, node->childkeys);
    if (pivotkey) {
        invariant(childnum > 0);
        init_childkey(node, childnum-1, pivotkey);
    }
    node->dirty = 1;
}

static void
ft_leaf_delete_leafentry (
    BASEMENTNODE bn,
    u_int32_t idx,
    LEAFENTRY le
    )
// Effect: Delete leafentry
//   idx is the location where it is
//   le is the leafentry to be deleted
{
    // Figure out if one of the other keys is the same key

    {
        int r = toku_omt_delete_at(bn->buffer, idx);
        assert(r==0);
    }

    bn->n_bytes_in_buffer -= leafentry_disksize(le);

    toku_mempool_mfree(&bn->buffer_mempool, 0, leafentry_memsize(le)); // Must pass 0, since le is no good any more.
}

void
toku_ft_bn_apply_cmd_once (
    BASEMENTNODE bn,
    const FT_MSG cmd,
    u_int32_t idx,
    LEAFENTRY le,
    uint64_t *workdone,
    STAT64INFO stats_to_update
    )
// Effect: Apply cmd to leafentry (msn is ignored)
//         Calculate work done by message on leafentry and add it to caller's workdone counter.
//   idx is the location where it goes
//   le is old leafentry
{
    // ft_leaf_check_leaf_stats(node);

    size_t newsize=0, oldsize=0, workdone_this_le=0;
    LEAFENTRY new_le=0;
    void *maybe_free = 0;
    int64_t numbytes_delta = 0;  // how many bytes of user data (not including overhead) were added or deleted from this row
    int64_t numrows_delta = 0;   // will be +1 or -1 or 0 (if row was added or deleted or not)

    if (le)
        oldsize = leafentry_memsize(le);

    // apply_msg_to_leafentry() may call mempool_malloc_from_omt() to allocate more space.
    // That means le is guaranteed to not cause a sigsegv but it may point to a mempool that is
    // no longer in use.  We'll have to release the old mempool later.
    {
        int r = apply_msg_to_leafentry(cmd, le, &newsize, &new_le, bn->buffer, &bn->buffer_mempool, &maybe_free, &numbytes_delta);
        invariant(r==0);
    }

    if (new_le) assert(newsize == leafentry_disksize(new_le));
    if (le && new_le) {
        bn->n_bytes_in_buffer -= oldsize;
        bn->n_bytes_in_buffer += newsize;

        // This mfree must occur after the mempool_malloc so that when
        // the mempool is compressed everything is accounted for.  But
        // we must compute the size before doing the mempool mfree
        // because otherwise the le pointer is no good.
        toku_mempool_mfree(&bn->buffer_mempool, 0, oldsize); // Must pass 0, since le may be no good any more.

        {
            int r = toku_omt_set_at(bn->buffer, new_le, idx);
            invariant(r==0);
        }

        workdone_this_le = (oldsize > newsize ? oldsize : newsize);  // work done is max of le size before and after message application

    } else {           // we did not just replace a row, so ...
        if (le) {
            //            ... we just deleted a row ...
            // It was there, note that it's gone and remove it from the mempool
            ft_leaf_delete_leafentry (bn, idx, le);
            workdone_this_le = oldsize;
            numrows_delta = -1;
        }
        if (new_le) {
            //            ... or we just added a row
            int r = toku_omt_insert_at(bn->buffer, new_le, idx);
            invariant(r==0);
            bn->n_bytes_in_buffer += newsize;
            workdone_this_le = newsize;
            numrows_delta = 1;
        }
    }
    if (workdone) {  // test programs may call with NULL
        *workdone += workdone_this_le;
        if (*workdone > STATUS_VALUE(FT_MAX_WORKDONE))
            STATUS_VALUE(FT_MAX_WORKDONE) = *workdone;
    }

    // if we created a new mempool buffer, free the old one
    if (maybe_free) toku_free(maybe_free);

    // now update stat64 statistics
    bn->stat64_delta.numrows  += numrows_delta;
    bn->stat64_delta.numbytes += numbytes_delta;
    // the only reason stats_to_update may be null is for tests
    if (stats_to_update) {
        stats_to_update->numrows += numrows_delta;
        stats_to_update->numbytes += numbytes_delta;
    }

}

static const uint32_t setval_tag = 0xee0ccb99; // this was gotten by doing "cat /dev/random|head -c4|od -x" to get a random number.  We want to make sure that the user actually passes us the setval_extra_s that we passed in.
struct setval_extra_s {
    u_int32_t  tag;
    BOOL did_set_val;
    int         setval_r;    // any error code that setval_fun wants to return goes here.
    // need arguments for toku_ft_bn_apply_cmd_once
    BASEMENTNODE bn;
    MSN msn;              // captured from original message, not currently used
    XIDS xids;
    const DBT *key;
    u_int32_t idx;
    LEAFENTRY le;
    uint64_t * workdone;  // set by toku_ft_bn_apply_cmd_once()
    STAT64INFO stats_to_update;
};

/*
 * If new_val == NULL, we send a delete message instead of an insert.
 * This happens here instead of in do_delete() for consistency.
 * setval_fun() is called from handlerton, passing in svextra_v
 * from setval_extra_s input arg to brt->update_fun().
 */
static void setval_fun (const DBT *new_val, void *svextra_v) {
    struct setval_extra_s *svextra = svextra_v;
    assert(svextra->tag==setval_tag);
    assert(!svextra->did_set_val);
    svextra->did_set_val = TRUE;

    {
        // can't leave scope until toku_ft_bn_apply_cmd_once if
        // this is a delete
        DBT val;
        FT_MSG_S msg = { FT_NONE, svextra->msn, svextra->xids,
                          .u.id={svextra->key, NULL} };
        if (new_val) {
            msg.type = FT_INSERT;
            msg.u.id.val = new_val;
        } else {
            msg.type = FT_DELETE_ANY;
            toku_init_dbt(&val);
            msg.u.id.val = &val;
        }
        toku_ft_bn_apply_cmd_once(svextra->bn, &msg,
                                   svextra->idx, svextra->le,
                                   svextra->workdone, svextra->stats_to_update);
        svextra->setval_r = 0;
    }
}

// We are already past the msn filter (in toku_ft_bn_apply_cmd(), which calls do_update()),
// so capturing the msn in the setval_extra_s is not strictly required.         The alternative
// would be to put a dummy msn in the messages created by setval_fun(), but preserving
// the original msn seems cleaner and it preserves accountability at a lower layer.
static int do_update(ft_update_func update_fun, DESCRIPTOR desc, BASEMENTNODE bn, FT_MSG cmd, int idx,
                     LEAFENTRY le,
                     uint64_t * workdone,
                     STAT64INFO stats_to_update) {
    LEAFENTRY le_for_update;
    DBT key;
    const DBT *keyp;
    const DBT *update_function_extra;
    DBT vdbt;
    const DBT *vdbtp;

    // the location of data depends whether this is a regular or
    // broadcast update
    if (cmd->type == FT_UPDATE) {
        // key is passed in with command (should be same as from le)
        // update function extra is passed in with command
        STATUS_VALUE(FT_UPDATES)++;
        keyp = cmd->u.id.key;
        update_function_extra = cmd->u.id.val;
    } else if (cmd->type == FT_UPDATE_BROADCAST_ALL) {
        // key is not passed in with broadcast, it comes from le
        // update function extra is passed in with command
        assert(le);  // for broadcast updates, we just hit all leafentries
                     // so this cannot be null
        assert(cmd->u.id.key->size == 0);
        STATUS_VALUE(FT_UPDATES_BROADCAST)++;
        keyp = toku_fill_dbt(&key, le_key(le), le_keylen(le));
        update_function_extra = cmd->u.id.val;
    } else {
        assert(FALSE);
    }

    if (le && !le_latest_is_del(le)) {
        // if the latest val exists, use it, and we'll use the leafentry later
        u_int32_t vallen;
        void *valp = le_latest_val_and_len(le, &vallen);
        vdbtp = toku_fill_dbt(&vdbt, valp, vallen);
        le_for_update = le;
    } else {
        // otherwise, the val and leafentry are both going to be null
        vdbtp = NULL;
        le_for_update = NULL;
    }

    struct setval_extra_s setval_extra = {setval_tag, FALSE, 0, bn, cmd->msn, cmd->xids,
                                          keyp, idx, le_for_update, workdone, stats_to_update};
    // call handlerton's brt->update_fun(), which passes setval_extra to setval_fun()
    FAKE_DB(db, desc);
    int r = update_fun(
        &db,
        keyp,
        vdbtp,
        update_function_extra,
        setval_fun, &setval_extra
        );

    if (r == 0) { r = setval_extra.setval_r; }
    return r;
}

// Should be renamed as something like "apply_cmd_to_basement()."
void
toku_ft_bn_apply_cmd (
    ft_compare_func compare_fun,
    ft_update_func update_fun,
    DESCRIPTOR desc,
    BASEMENTNODE bn,
    FT_MSG cmd,
    uint64_t *workdone,
    STAT64INFO stats_to_update
    )
// Effect:
//   Put a cmd into a leaf.
//   Calculate work done by message on leafnode and add it to caller's workdone counter.
// The leaf could end up "too big" or "too small".  The caller must fix that up.
{
    LEAFENTRY storeddata;
    OMTVALUE storeddatav=NULL;

    u_int32_t omt_size;
    int r;
    struct cmd_leafval_heaviside_extra be = {compare_fun, desc, cmd->u.id.key};

    unsigned int doing_seqinsert = bn->seqinsert;
    bn->seqinsert = 0;

    switch (cmd->type) {
    case FT_INSERT_NO_OVERWRITE:
    case FT_INSERT: {
        u_int32_t idx;
        if (doing_seqinsert) {
            idx = toku_omt_size(bn->buffer);
            r = toku_omt_fetch(bn->buffer, idx-1, &storeddatav);
            if (r != 0) goto fz;
            storeddata = storeddatav;
            int cmp = toku_cmd_leafval_heaviside(storeddata, &be);
            if (cmp >= 0) goto fz;
            r = DB_NOTFOUND;
        } else {
        fz:
            r = toku_omt_find_zero(bn->buffer, toku_cmd_leafval_heaviside, &be,
                                   &storeddatav, &idx);
        }
        if (r==DB_NOTFOUND) {
            storeddata = 0;
        } else {
            assert(r==0);
            storeddata=storeddatav;
        }
        toku_ft_bn_apply_cmd_once(bn, cmd, idx, storeddata, workdone, stats_to_update);

        // if the insertion point is within a window of the right edge of
        // the leaf then it is sequential
        // window = min(32, number of leaf entries/16)
        {
            u_int32_t s = toku_omt_size(bn->buffer);
            u_int32_t w = s / 16;
            if (w == 0) w = 1;
            if (w > 32) w = 32;

            // within the window?
            if (s - idx <= w)
                bn->seqinsert = doing_seqinsert + 1;
        }
        break;
    }
    case FT_DELETE_ANY:
    case FT_ABORT_ANY:
    case FT_COMMIT_ANY: {
        u_int32_t idx;
        // Apply to all the matches

        r = toku_omt_find_zero(bn->buffer, toku_cmd_leafval_heaviside, &be,
                               &storeddatav, &idx);
        if (r == DB_NOTFOUND) break;
        assert(r==0);
        storeddata=storeddatav;

        while (1) {
            u_int32_t num_leafentries_before = toku_omt_size(bn->buffer);

            toku_ft_bn_apply_cmd_once(bn, cmd, idx, storeddata, workdone, stats_to_update);

            {
                // Now we must find the next leafentry.
                u_int32_t num_leafentries_after = toku_omt_size(bn->buffer);
                //idx is the index of the leafentry we just modified.
                //If the leafentry was deleted, we will have one less leafentry in
                //the omt than we started with and the next leafentry will be at the
                //same index as the deleted one. Otherwise, the next leafentry will
                //be at the next index (+1).
                assert(num_leafentries_before        == num_leafentries_after ||
                       num_leafentries_before-1 == num_leafentries_after);
                if (num_leafentries_after==num_leafentries_before) idx++; //Not deleted, advance index.

                assert(idx <= num_leafentries_after);
                if (idx == num_leafentries_after) break; //Reached the end of the leaf
                r = toku_omt_fetch(bn->buffer, idx, &storeddatav);
                assert_zero(r);
            }
            storeddata=storeddatav;
            {        // Continue only if the next record that we found has the same key.
                DBT adbt;
                u_int32_t keylen;
                void *keyp = le_key_and_len(storeddata, &keylen);
                FAKE_DB(db, desc);
                if (compare_fun(&db,
                                toku_fill_dbt(&adbt, keyp, keylen),
                                cmd->u.id.key) != 0)
                    break;
            }
        }

        break;
    }
    case FT_OPTIMIZE_FOR_UPGRADE:
        // fall through so that optimize_for_upgrade performs rest of the optimize logic
    case FT_COMMIT_BROADCAST_ALL:
    case FT_OPTIMIZE:
        // Apply to all leafentries
        omt_size = toku_omt_size(bn->buffer);
        for (u_int32_t idx = 0; idx < omt_size; ) {
            r = toku_omt_fetch(bn->buffer, idx, &storeddatav);
            assert_zero(r);
            storeddata=storeddatav;
            int deleted = 0;
            if (!le_is_clean(storeddata)) { //If already clean, nothing to do.
                toku_ft_bn_apply_cmd_once(bn, cmd, idx, storeddata, workdone, stats_to_update);
                u_int32_t new_omt_size = toku_omt_size(bn->buffer);
                if (new_omt_size != omt_size) {
                    assert(new_omt_size+1 == omt_size);
                    //Item was deleted.
                    deleted = 1;
                }
            }
            if (deleted)
                omt_size--;
            else
                idx++;
        }
        assert(toku_omt_size(bn->buffer) == omt_size);

        break;
    case FT_COMMIT_BROADCAST_TXN:
    case FT_ABORT_BROADCAST_TXN:
        // Apply to all leafentries if txn is represented
        omt_size = toku_omt_size(bn->buffer);
        for (u_int32_t idx = 0; idx < omt_size; ) {
            r = toku_omt_fetch(bn->buffer, idx, &storeddatav);
            assert_zero(r);
            storeddata=storeddatav;
            int deleted = 0;
            if (le_has_xids(storeddata, cmd->xids)) {
                toku_ft_bn_apply_cmd_once(bn, cmd, idx, storeddata, workdone, stats_to_update);
                u_int32_t new_omt_size = toku_omt_size(bn->buffer);
                if (new_omt_size != omt_size) {
                    assert(new_omt_size+1 == omt_size);
                    //Item was deleted.
                    deleted = 1;
                }
            }
            if (deleted)
                omt_size--;
            else
                idx++;
        }
        assert(toku_omt_size(bn->buffer) == omt_size);

        break;
    case FT_UPDATE: {
        u_int32_t idx;
        r = toku_omt_find_zero(bn->buffer, toku_cmd_leafval_heaviside, &be,
                               &storeddatav, &idx);
        if (r==DB_NOTFOUND) {
            r = do_update(update_fun, desc, bn, cmd, idx, NULL, workdone, stats_to_update);
        } else if (r==0) {
            storeddata=storeddatav;
            r = do_update(update_fun, desc, bn, cmd, idx, storeddata, workdone, stats_to_update);
        } // otherwise, a worse error, just return it
        break;
    }
    case FT_UPDATE_BROADCAST_ALL: {
        // apply to all leafentries.
        u_int32_t idx = 0;
        u_int32_t num_leafentries_before;
        while (idx < (num_leafentries_before = toku_omt_size(bn->buffer))) {
            r = toku_omt_fetch(bn->buffer, idx, &storeddatav);
            assert(r==0);
            storeddata=storeddatav;
            r = do_update(update_fun, desc, bn, cmd, idx, storeddata, workdone, stats_to_update);
            // TODO(leif): This early return means get_leaf_reactivity()
            // and VERIFY_NODE() never get called.  Is this a problem?
            assert(r==0);

            if (num_leafentries_before == toku_omt_size(bn->buffer)) {
                // we didn't delete something, so increment the index.
                idx++;
            }
        }
        break;
    }
    case FT_NONE: break; // don't do anything
    }

    return;
}

static inline int
key_msn_cmp(const DBT *a, const DBT *b, const MSN amsn, const MSN bmsn,
            DESCRIPTOR descriptor, ft_compare_func key_cmp)
{
    FAKE_DB(db, descriptor);
    int r = key_cmp(&db, a, b);
    if (r == 0) {
        if (amsn.msn > bmsn.msn) {
            r = +1;
        } else if (amsn.msn < bmsn.msn) {
            r = -1;
        } else {
            r = 0;
        }
    }
    return r;
}

int
toku_fifo_entry_key_msn_heaviside(OMTVALUE v, void *extrap)
{
    const struct toku_fifo_entry_key_msn_heaviside_extra *extra = extrap;
    const long offset = (long) v;
    const struct fifo_entry *query = toku_fifo_get_entry(extra->fifo, offset);
    DBT qdbt;
    const DBT *query_key = fill_dbt_for_fifo_entry(&qdbt, query);
    const DBT *target_key = extra->key;
    return key_msn_cmp(query_key, target_key, query->msn, extra->msn,
                       extra->desc, extra->cmp);
}

int
toku_fifo_entry_key_msn_cmp(void *extrap, const void *ap, const void *bp)
{
    const struct toku_fifo_entry_key_msn_cmp_extra *extra = extrap;
    const long ao = *(long *) ap;
    const long bo = *(long *) bp;
    const struct fifo_entry *a = toku_fifo_get_entry(extra->fifo, ao);
    const struct fifo_entry *b = toku_fifo_get_entry(extra->fifo, bo);
    DBT adbt, bdbt;
    const DBT *akey = fill_dbt_for_fifo_entry(&adbt, a);
    const DBT *bkey = fill_dbt_for_fifo_entry(&bdbt, b);
    return key_msn_cmp(akey, bkey, a->msn, b->msn,
                       extra->desc, extra->cmp);
}

int
toku_bnc_insert_msg(NONLEAF_CHILDINFO bnc, const void *key, ITEMLEN keylen, const void *data, ITEMLEN datalen, enum ft_msg_type type, MSN msn, XIDS xids, bool is_fresh, DESCRIPTOR desc, ft_compare_func cmp)
// Effect: Enqueue the message represented by the parameters into the
//   bnc's buffer, and put it in either the fresh or stale message tree,
//   or the broadcast list.
//
// This is only exported for tests.
{
    long offset;
    int r = toku_fifo_enq(bnc->buffer, key, keylen, data, datalen, type, msn, xids, is_fresh, &offset);
    assert_zero(r);
    if (ft_msg_type_applies_once(type)) {
        DBT keydbt;
        struct toku_fifo_entry_key_msn_heaviside_extra extra = { .desc = desc, .cmp = cmp, .fifo = bnc->buffer, .key = toku_fill_dbt(&keydbt, key, keylen), .msn = msn };
        if (is_fresh) {
            r = toku_omt_insert(bnc->fresh_message_tree, (OMTVALUE) offset, toku_fifo_entry_key_msn_heaviside, &extra, NULL);
            assert_zero(r);
        } else {
            r = toku_omt_insert(bnc->stale_message_tree, (OMTVALUE) offset, toku_fifo_entry_key_msn_heaviside, &extra, NULL);
            assert_zero(r);
        }
    } else if (ft_msg_type_applies_all(type) || ft_msg_type_does_nothing(type)) {
        u_int32_t idx = toku_omt_size(bnc->broadcast_list);
        r = toku_omt_insert_at(bnc->broadcast_list, (OMTVALUE) offset, idx);
        assert_zero(r);
    } else {
        assert(FALSE);
    }
    return r;
}

// append a cmd to a nonleaf node's child buffer
// should be static, but used by test programs
void
toku_ft_append_to_child_buffer(ft_compare_func compare_fun, DESCRIPTOR desc, FTNODE node, int childnum, enum ft_msg_type type, MSN msn, XIDS xids, bool is_fresh, const DBT *key, const DBT *val) {
    assert(BP_STATE(node,childnum) == PT_AVAIL);
    int r = toku_bnc_insert_msg(BNC(node, childnum), key->data, key->size, val->data, val->size, type, msn, xids, is_fresh, desc, compare_fun);
    invariant_zero(r);
    node->dirty = 1;
}

static void ft_nonleaf_cmd_once_to_child (ft_compare_func compare_fun, DESCRIPTOR desc,  FTNODE node, unsigned int childnum, FT_MSG cmd, bool is_fresh)
// Previously we had passive aggressive promotion, but that causes a lot of I/O a the checkpoint.  So now we are just putting it in the buffer here.
// Also we don't worry about the node getting overfull here.  It's the caller's problem.
{
    toku_ft_append_to_child_buffer(compare_fun, desc, node, childnum, cmd->type, cmd->msn, cmd->xids, is_fresh, cmd->u.id.key, cmd->u.id.val);
}

/* Find the leftmost child that may contain the key.
 * If the key exists it will be in the child whose number
 * is the return value of this function.
 */
unsigned int toku_ftnode_which_child(FTNODE node, const DBT *k,
                                      DESCRIPTOR desc, ft_compare_func cmp) {
#define DO_PIVOT_SEARCH_LR 0
#if DO_PIVOT_SEARCH_LR
    int i;
    for (i=0; i<node->n_children-1; i++) {
        int c = ft_compare_pivot(desc, cmp, k, d, &node->childkeys[i]);
        if (c > 0) continue;
        if (c < 0) return i;
        return i;
    }
    return node->n_children-1;
#else
#endif
#define DO_PIVOT_SEARCH_RL 0
#if DO_PIVOT_SEARCH_RL
    // give preference for appending to the dictionary.         no change for
    // random keys
    int i;
    for (i = node->n_children-2; i >= 0; i--) {
        int c = ft_compare_pivot(desc, cmp, k, d, &node->childkeys[i]);
        if (c > 0) return i+1;
    }
    return 0;
#endif
#define DO_PIVOT_BIN_SEARCH 1
#if DO_PIVOT_BIN_SEARCH
    // a funny case of no pivots
    if (node->n_children <= 1) return 0;

    // check the last key to optimize seq insertions
    int n = node->n_children-1;
    int c = ft_compare_pivot(desc, cmp, k, &node->childkeys[n-1]);
    if (c > 0) return n;

    // binary search the pivots
    int lo = 0;
    int hi = n-1; // skip the last one, we checked it above
    int mi;
    while (lo < hi) {
        mi = (lo + hi) / 2;
        c = ft_compare_pivot(desc, cmp, k, &node->childkeys[mi]);
        if (c > 0) {
            lo = mi+1;
            continue;
        }
        if (c < 0) {
            hi = mi;
            continue;
        }
        return mi;
    }
    return lo;
#endif
}

// Used for HOT.
unsigned int
toku_ftnode_hot_next_child(FTNODE node,
                            const DBT *k,
                            DESCRIPTOR desc,
                            ft_compare_func cmp) {
    int low = 0;
    int hi = node->n_children - 1;
    int mi;
    while (low < hi) {
        mi = (low + hi) / 2;
        int r = ft_compare_pivot(desc, cmp, k, &node->childkeys[mi]);
        if (r > 0) {
            low = mi + 1;
        } else if (r < 0) {
            hi = mi;
        } else {
            // if they were exactly equal, then we want the sub-tree under
            // the next pivot.
            return mi + 1;
        }
    }
    invariant(low == hi);
    return low;
}

// TODO Use this function to clean up other places where bits of messages are passed around
//      such as toku_bnc_insert_msg() and the call stack above it.
static size_t
ft_msg_size(FT_MSG msg) {
    size_t keylen = msg->u.id.key->size;
    size_t vallen = msg->u.id.val->size;
    size_t xids_size = xids_get_serialize_size(msg->xids);
    size_t rval = keylen + vallen + KEY_VALUE_OVERHEAD + FT_CMD_OVERHEAD + xids_size;
    return rval;
}


static void ft_nonleaf_cmd_once(ft_compare_func compare_fun, DESCRIPTOR desc, FTNODE node, FT_MSG cmd, bool is_fresh)
// Effect: Insert a message into a nonleaf.  We may put it into a child, possibly causing the child to become reactive.
//  We don't do the splitting and merging.  That's up to the caller after doing all the puts it wants to do.
//  The re_array[i] gets set to reactivity of any modified child.
{
    /* find the right subtree */
    //TODO: accesses key, val directly
    unsigned int childnum = toku_ftnode_which_child(node, cmd->u.id.key, desc, compare_fun);

    ft_nonleaf_cmd_once_to_child (compare_fun, desc, node, childnum, cmd, is_fresh);
}

static void
ft_nonleaf_cmd_all (ft_compare_func compare_fun, DESCRIPTOR desc, FTNODE node, FT_MSG cmd, bool is_fresh)
// Effect: Put the cmd into a nonleaf node.  We put it into all children, possibly causing the children to become reactive.
//  We don't do the splitting and merging.  That's up to the caller after doing all the puts it wants to do.
//  The re_array[i] gets set to the reactivity of any modified child i.         (And there may be several such children.)
{
    int i;
    for (i = 0; i < node->n_children; i++) {
        ft_nonleaf_cmd_once_to_child(compare_fun, desc, node, i, cmd, is_fresh);
    }
}

static BOOL
ft_msg_applies_once(FT_MSG cmd)
{
    return ft_msg_type_applies_once(cmd->type);
}

static BOOL
ft_msg_applies_all(FT_MSG cmd)
{
    return ft_msg_type_applies_all(cmd->type);
}

static BOOL
ft_msg_does_nothing(FT_MSG cmd)
{
    return ft_msg_type_does_nothing(cmd->type);
}

static void
ft_nonleaf_put_cmd (ft_compare_func compare_fun, DESCRIPTOR desc, FTNODE node, FT_MSG cmd, bool is_fresh)
// Effect: Put the cmd into a nonleaf node.  We may put it into a child, possibly causing the child to become reactive.
//  We don't do the splitting and merging.  That's up to the caller after doing all the puts it wants to do.
//  The re_array[i] gets set to the reactivity of any modified child i.         (And there may be several such children.)
//
{

    //
    // see comments in toku_ft_leaf_apply_cmd
    // to understand why we handle setting
    // node->max_msn_applied_to_node_on_disk here,
    // and don't do it in toku_ft_node_put_cmd
    //
    MSN cmd_msn = cmd->msn;
    invariant(cmd_msn.msn > node->max_msn_applied_to_node_on_disk.msn);
    node->max_msn_applied_to_node_on_disk = cmd_msn;

    //TODO: Accessing type directly
    switch (cmd->type) {
    case FT_INSERT_NO_OVERWRITE:
    case FT_INSERT:
    case FT_DELETE_ANY:
    case FT_ABORT_ANY:
    case FT_COMMIT_ANY:
    case FT_UPDATE:
        ft_nonleaf_cmd_once(compare_fun, desc, node, cmd, is_fresh);
        return;
    case FT_COMMIT_BROADCAST_ALL:
    case FT_COMMIT_BROADCAST_TXN:
    case FT_ABORT_BROADCAST_TXN:
    case FT_OPTIMIZE:
    case FT_OPTIMIZE_FOR_UPGRADE:
    case FT_UPDATE_BROADCAST_ALL:
        ft_nonleaf_cmd_all (compare_fun, desc, node, cmd, is_fresh);  // send message to all children
        return;
    case FT_NONE:
        return;
    }
    abort(); // cannot happen
}


// return TRUE if root changed, FALSE otherwise
static BOOL
ft_process_maybe_reactive_root (FT ft, CACHEKEY *rootp, FTNODE *nodep) {
    FTNODE node = *nodep;
    toku_assert_entire_node_in_memory(node);
    enum reactivity re = get_node_reactivity(node);
    switch (re) {
    case RE_STABLE:
        return FALSE;
    case RE_FISSIBLE:
    {
        // The root node should split, so make a new root.
        FTNODE nodea,nodeb;
        DBT splitk;
        assert(ft->h->nodesize>=node->nodesize); /* otherwise we might be in trouble because the nodesize shrank. */
        //
        // This happens on the client thread with the ydb lock, so it is safe to
        // not pass in dependent nodes. Although if we wanted to, we could pass
        // in just node. That would be correct.
        //
        if (node->height==0) {
            ftleaf_split(ft, node, &nodea, &nodeb, &splitk, TRUE, 0, NULL);
        } else {
            ft_nonleaf_split(ft, node, &nodea, &nodeb, &splitk, 0, NULL);
        }
        ft_init_new_root(ft, nodea, nodeb, splitk, rootp, nodep);
        return TRUE;
    }
    case RE_FUSIBLE:
        return FALSE; // Cannot merge anything at the root, so return happy.
    }
    abort(); // cannot happen
}


// Garbage collect one leaf entry.
static void
ft_basement_node_gc_once(BASEMENTNODE bn,
                          u_int32_t index,
                          LEAFENTRY leaf_entry,
                          OMT snapshot_xids,
                          OMT referenced_xids,
                          OMT live_root_txns,
                          STAT64INFO_S * delta)
{
    assert(leaf_entry);

    // There is no point in running GC if there is only one committed
    // leaf entry.
    if (leaf_entry->type != LE_MVCC || leaf_entry->u.mvcc.num_cxrs <= 1) { // MAKE ACCESSOR
        goto exit;
    }

    size_t oldsize = 0, newsize = 0;
    LEAFENTRY new_leaf_entry = NULL;

    // The mempool doesn't free itself.  When it allocates new memory,
    // this pointer will be set to the older memory that must now be
    // freed.
    void * maybe_free = NULL;

    // Cache the size of the leaf entry.
    oldsize = leafentry_memsize(leaf_entry);
    garbage_collect_leafentry(leaf_entry,
                              &new_leaf_entry,
                              &newsize,
                              bn->buffer,
                              &bn->buffer_mempool,
                              &maybe_free,
                              snapshot_xids,
                              referenced_xids,
                              live_root_txns);

    // These will represent the number of bytes and rows changed as
    // part of the garbage collection.
    int64_t numbytes_delta = newsize - oldsize;
    int64_t numrows_delta = 0;
    if (new_leaf_entry) {
        // If we have a new leaf entry, we must update the size of the
        // memory object.
        bn->n_bytes_in_buffer -= oldsize;
        bn->n_bytes_in_buffer += newsize;
        toku_mempool_mfree(&bn->buffer_mempool, 0, oldsize);
        toku_omt_set_at(bn->buffer, new_leaf_entry, index);
        numrows_delta = 0;
    } else {
        // Our garbage collection removed the leaf entry so we must
        // remove it from the mempool.
        ft_leaf_delete_leafentry (bn, index, leaf_entry);
        numrows_delta = -1;
    }

    // If we created a new mempool buffer we must free the
    // old/original buffer.
    if (maybe_free) {
        toku_free(maybe_free);
    }

    // Update stats.
    bn->stat64_delta.numrows += numrows_delta;
    bn->stat64_delta.numbytes += numbytes_delta;
    delta->numrows += numrows_delta;
    delta->numbytes += numbytes_delta;

exit:
    return;
}

// Garbage collect all leaf entries for a given basement node.
static void
basement_node_gc_all_les(BASEMENTNODE bn,
                         OMT snapshot_xids,
                         OMT referenced_xids,
                         OMT live_root_txns,
                         STAT64INFO_S * delta)
{
    int r = 0;
    u_int32_t index = 0;
    u_int32_t num_leafentries_before;
    while (index < (num_leafentries_before = toku_omt_size(bn->buffer))) {
        OMTVALUE storedatav = NULL;
        LEAFENTRY leaf_entry;
        r = toku_omt_fetch(bn->buffer, index, &storedatav);
        assert(r == 0);
        leaf_entry = storedatav;
        ft_basement_node_gc_once(bn, index, leaf_entry, snapshot_xids, referenced_xids, live_root_txns, delta);
        // Check if the leaf entry was deleted or not.
        if (num_leafentries_before == toku_omt_size(bn->buffer)) {
            ++index;
        }
    }
}

// Garbage collect all leaf entires.
static void
ft_leaf_gc_all_les(FTNODE node,
                    FT h,
                    OMT snapshot_xids,
                    OMT referenced_xids,
                    OMT live_root_txns)
{
    toku_assert_entire_node_in_memory(node);
    assert(node->height == 0);
    // Loop through each leaf entry, garbage collecting as we go.
    for (int i = 0; i < node->n_children; ++i) {
        // Perform the garbage collection.
        BASEMENTNODE bn = BLB(node, i);
        STAT64INFO_S delta;
        delta.numrows = 0;
        delta.numbytes = 0;
        basement_node_gc_all_les(bn, snapshot_xids, referenced_xids, live_root_txns, &delta);
        toku_ft_update_stats(&h->in_memory_stats, delta);
    }
}

int
toku_bnc_flush_to_child(
    FT h,
    NONLEAF_CHILDINFO bnc,
    FTNODE child
    )
{
    assert(bnc);
    assert(toku_fifo_n_entries(bnc->buffer)>0);
    STAT64INFO_S stats_delta = {0,0};
    FIFO_ITERATE(
        bnc->buffer, key, keylen, val, vallen, type, msn, xids, is_fresh,
        ({
            DBT hk,hv;
            FT_MSG_S ftcmd = { type, msn, xids, .u.id= {toku_fill_dbt(&hk, key, keylen),
                                                          toku_fill_dbt(&hv, val, vallen)} };
            toku_ft_node_put_cmd(
                h->compare_fun,
                h->update_fun,
                &h->cmp_descriptor,
                child,
                &ftcmd,
                is_fresh,
                &stats_delta
                );
        }));
    if (stats_delta.numbytes || stats_delta.numrows) {
        toku_ft_update_stats(&h->in_memory_stats, stats_delta);
    }
    // Run garbage collection, if we are a leaf entry.
    TOKULOGGER logger = toku_cachefile_logger(h->cf);
    if (child->height == 0 && logger) {
        OMT snapshot_txnids = NULL;
        OMT referenced_xids = NULL;
        OMT live_root_txns = NULL;
        toku_txn_manager_clone_state_for_gc(
            logger->txn_manager,
            &snapshot_txnids,
            &referenced_xids,
            &live_root_txns
            );
        size_t buffsize = toku_fifo_buffer_size_in_use(bnc->buffer);
        STATUS_VALUE(FT_MSG_BYTES_OUT) += buffsize;
        // may be misleading if there's a broadcast message in there
        STATUS_VALUE(FT_MSG_BYTES_CURR) -= buffsize;
        // Perform the garbage collection.
        ft_leaf_gc_all_les(child, h, snapshot_txnids, referenced_xids, live_root_txns);

        // Free the OMT's we used for garbage collecting.
        toku_omt_destroy(&snapshot_txnids);
        toku_omt_destroy(&live_root_txns);
        toku_omt_free_items_pool(referenced_xids);
        toku_omt_destroy(&referenced_xids);
    }

    return 0;
}

void bring_node_fully_into_memory(FTNODE node, FT h)
{
    if (!is_entire_node_in_memory(node)) {
        struct ftnode_fetch_extra bfe;
        fill_bfe_for_full_read(&bfe, h);
        toku_cachetable_pf_pinned_pair(
            node,
            toku_ftnode_pf_callback,
            &bfe,
            h->cf,
            node->thisnodename,
            toku_cachetable_hash(h->cf, node->thisnodename)
            );
    }
}

void
toku_ft_node_put_cmd (
    ft_compare_func compare_fun,
    ft_update_func update_fun,
    DESCRIPTOR desc,
    FTNODE node,
    FT_MSG cmd,
    bool is_fresh,
    STAT64INFO stats_to_update
    )
// Effect: Push CMD into the subtree rooted at NODE.
//   If NODE is a leaf, then
//   put CMD into leaf, applying it to the leafentries
//   If NODE is a nonleaf, then push the cmd into the FIFO(s) of the relevent child(ren).
//   The node may become overfull.  That's not our problem.
{
    toku_assert_entire_node_in_memory(node);
    //
    // see comments in toku_ft_leaf_apply_cmd
    // to understand why we don't handle setting
    // node->max_msn_applied_to_node_on_disk here,
    // and instead defer to these functions
    //
    if (node->height==0) {
        uint64_t workdone = 0;
        toku_ft_leaf_apply_cmd(
            compare_fun,
            update_fun,
            desc,
            node,
            cmd,
            &workdone,
            stats_to_update
            );
    } else {
        ft_nonleaf_put_cmd(compare_fun, desc, node, cmd, is_fresh);
    }
}

static const struct pivot_bounds infinite_bounds = {.lower_bound_exclusive=NULL,
                                                    .upper_bound_inclusive=NULL};


// Effect: applies the cmd to the leaf if the appropriate basement node is in memory.
//           This function is called during message injection and/or flushing, so the entire
//           node MUST be in memory.
void toku_ft_leaf_apply_cmd(
    ft_compare_func compare_fun,
    ft_update_func update_fun,
    DESCRIPTOR desc,
    FTNODE node,
    FT_MSG cmd,
    uint64_t *workdone,
    STAT64INFO stats_to_update
    )
{
    VERIFY_NODE(t, node);
    toku_assert_entire_node_in_memory(node);

    //
    // Because toku_ft_leaf_apply_cmd is called with the intent of permanently
    // applying a message to a leaf node (meaning the message is permanently applied
    // and will be purged from the system after this call, as opposed to
    // maybe_apply_ancestors_messages_to_node, which applies a message
    // for a query, but the message may still reside in the system and
    // be reapplied later), we mark the node as dirty and
    // take the opportunity to update node->max_msn_applied_to_node_on_disk.
    //
    node->dirty = 1;

    //
    // we cannot blindly update node->max_msn_applied_to_node_on_disk,
    // we must check to see if the msn is greater that the one already stored,
    // because the cmd may have already been applied earlier (via
    // maybe_apply_ancestors_messages_to_node) to answer a query
    //
    // This is why we handle node->max_msn_applied_to_node_on_disk both here
    // and in ft_nonleaf_put_cmd, as opposed to in one location, toku_ft_node_put_cmd.
    //
    MSN cmd_msn = cmd->msn;
    if (cmd_msn.msn > node->max_msn_applied_to_node_on_disk.msn) {
        node->max_msn_applied_to_node_on_disk = cmd_msn;
    }

    if (ft_msg_applies_once(cmd)) {
        unsigned int childnum = toku_ftnode_which_child(node, cmd->u.id.key, desc, compare_fun);
        if (cmd->msn.msn > BLB(node, childnum)->max_msn_applied.msn) {
            BLB(node, childnum)->max_msn_applied = cmd->msn;
            toku_ft_bn_apply_cmd(compare_fun,
                                  update_fun,
                                  desc,
                                  BLB(node, childnum),
                                  cmd,
                                  workdone,
                                  stats_to_update);
        } else {
            STATUS_VALUE(FT_MSN_DISCARDS)++;
        }
    }
    else if (ft_msg_applies_all(cmd)) {
        for (int childnum=0; childnum<node->n_children; childnum++) {
            if (cmd->msn.msn > BLB(node, childnum)->max_msn_applied.msn) {
                BLB(node, childnum)->max_msn_applied = cmd->msn;
                toku_ft_bn_apply_cmd(compare_fun,
                                      update_fun,
                                      desc,
                                      BLB(node, childnum),
                                      cmd,
                                      workdone,
                                      stats_to_update);
            } else {
                STATUS_VALUE(FT_MSN_DISCARDS)++;
            }
        }
    }
    else if (!ft_msg_does_nothing(cmd)) {
        assert(FALSE);
    }
    VERIFY_NODE(t, node);
}

static void push_something_at_root (FT h, FTNODE *nodep, FT_MSG cmd)
// Effect:  Put CMD into brt's root node, and update
// the value of root's max_msn_applied_to_node_on_disk
{
    FTNODE node = *nodep;
    toku_assert_entire_node_in_memory(node);
    MSN cmd_msn = cmd->msn;
    invariant(cmd_msn.msn > node->max_msn_applied_to_node_on_disk.msn);
    STAT64INFO_S stats_delta = {0,0};
    toku_ft_node_put_cmd(
        h->compare_fun,
        h->update_fun,
        &h->cmp_descriptor,
        node,
        cmd,
        true,
        &stats_delta
        );
    if (stats_delta.numbytes || stats_delta.numrows) {
        toku_ft_update_stats(&h->in_memory_stats, stats_delta);
    }
    //
    // assumption is that toku_ft_node_put_cmd will
    // mark the node as dirty.
    // enforcing invariant here.
    //
    invariant(node->dirty != 0);

    // update some status variables
    if (node->height != 0) {
        uint64_t msgsize = ft_msg_size(cmd);
        STATUS_VALUE(FT_MSG_BYTES_IN) += msgsize;
        STATUS_VALUE(FT_MSG_BYTES_CURR) += msgsize;
        if (STATUS_VALUE(FT_MSG_BYTES_CURR) > STATUS_VALUE(FT_MSG_BYTES_MAX)) {
            STATUS_VALUE(FT_MSG_BYTES_MAX) = STATUS_VALUE(FT_MSG_BYTES_CURR);
        }
        STATUS_VALUE(FT_MSG_NUM)++;
        if (ft_msg_applies_all(cmd)) {
            STATUS_VALUE(FT_MSG_NUM_BROADCAST)++;
        }
    }
}

int
toku_ft_root_put_cmd (FT h, FT_MSG_S * cmd)
// Effect:
//  - assign msn to cmd
//  - push the cmd into the brt
//  - cmd will set new msn in tree
{
    FTNODE node;
    CACHEKEY root_key;
    //assert(0==toku_cachetable_assert_all_unpinned(brt->cachetable));
    assert(h);
    //
    // As of Dr. Noga, the following code is currently protected by two locks:
    //  - the ydb lock
    //  - header's tree lock
    //
    // We hold the header's tree lock to stop other threads from
    // descending down the tree while the root node may change.
    // The root node may change when ft_process_maybe_reactive_root is called.
    // Other threads (such as the cleaner thread or hot optimize) that want to
    // descend down the tree must grab the header's tree lock, so they are
    // ensured that what they think is the root's blocknum is actually
    // the root's blocknum.
    //
    // We also hold the ydb lock for a number of reasons, but an important
    // one is to make sure that a begin_checkpoint may not start while
    // this code is executing. A begin_checkpoint does (at least) two things
    // that can interfere with the operations here:
    //  - copies the header to a checkpoint header. Because we may change
    //    the root blocknum below, we don't want the header to be copied in
    //    the middle of these operations.
    //  - Takes note of the log's LSN. Because this put operation has
    //     already been logged, this message injection must be included
    //     in any checkpoint that contains this put's logentry.
    //     Holding the ydb lock throughout this function ensures that fact.
    //  As of Dr. Noga, I (Zardosht) THINK these are the only reasons why
    //  the ydb lock must be held for this function, but there may be
    //  others
    //
    {
        toku_ft_grab_treelock(h);

        u_int32_t fullhash;
        toku_calculate_root_offset_pointer(h, &root_key, &fullhash);

        // get the root node
        struct ftnode_fetch_extra bfe;
        fill_bfe_for_full_read(&bfe, h);
        toku_pin_ftnode_off_client_thread(
            h,
            root_key,
            fullhash,
            &bfe,
            TRUE, // may_modify_node
            0,
            NULL,
            &node
            );
        toku_assert_entire_node_in_memory(node);

        cmd->msn.msn = node->max_msn_applied_to_node_on_disk.msn + 1;
        // Note, the lower level function that filters messages based on
        // msn, (toku_ft_bn_apply_cmd() or ft_nonleaf_put_cmd()) will capture
        // the msn and store it in the relevant node, including the root
        // node. This is how the new msn is set in the root.

        //VERIFY_NODE(brt, node);
        assert(node->fullhash==fullhash);
        ft_verify_flags(h, node);

        // first handle a reactive root, then put in the message
        CACHEKEY new_root_key;
        BOOL root_changed = ft_process_maybe_reactive_root(h, &new_root_key, &node);
        if (root_changed) {
            toku_ft_set_new_root_blocknum(h, new_root_key);
        }

        toku_ft_release_treelock(h);
    }
    push_something_at_root(h, &node, cmd);
    // verify that msn of latest message was captured in root node (push_something_at_root() did not release ydb lock)
    invariant(cmd->msn.msn == node->max_msn_applied_to_node_on_disk.msn);

    // if we call flush_some_child, then that function unpins the root
    // otherwise, we unpin ourselves
    if (node->height > 0 && toku_ft_nonleaf_is_gorged(node)) {
        flush_node_on_background_thread(h, node);
    }
    else {
        toku_unpin_ftnode(h, node);  // unpin root
    }

    return 0;
}

// Effect: Insert the key-val pair into brt.
int toku_ft_insert (FT_HANDLE brt, DBT *key, DBT *val, TOKUTXN txn) {
    return toku_ft_maybe_insert(brt, key, val, txn, FALSE, ZERO_LSN, TRUE, FT_INSERT);
}

int
toku_ft_load_recovery(TOKUTXN txn, FILENUM old_filenum, char const * new_iname, int do_fsync, int do_log, LSN *load_lsn) {
    int r = 0;
    assert(txn);
    toku_txn_force_fsync_on_commit(txn);  //If the txn commits, the commit MUST be in the log
                                          //before the (old) file is actually unlinked
    TOKULOGGER logger = toku_txn_logger(txn);

    BYTESTRING new_iname_bs = {.len=strlen(new_iname), .data=(char*)new_iname};
    r = toku_logger_save_rollback_load(txn, old_filenum, &new_iname_bs);
    if (r==0 && do_log && logger) {
        TXNID xid = toku_txn_get_txnid(txn);
        r = toku_log_load(logger, load_lsn, do_fsync, txn, xid, old_filenum, new_iname_bs);
    }
    return r;
}

// 2954
// this function handles the tasks needed to be recoverable
//  - write to rollback log
//  - write to recovery log
int
toku_ft_hot_index_recovery(TOKUTXN txn, FILENUMS filenums, int do_fsync, int do_log, LSN *hot_index_lsn)
{
    int r = 0;
    assert(txn);
    TOKULOGGER logger = toku_txn_logger(txn);

    // write to the rollback log
    r = toku_logger_save_rollback_hot_index(txn, &filenums);
    if ( r==0 && do_log && logger) {
        TXNID xid = toku_txn_get_txnid(txn);
        // write to the recovery log
        r = toku_log_hot_index(logger, hot_index_lsn, do_fsync, txn, xid, filenums);
    }
    return r;
}

// Effect: Optimize the brt.
int
toku_ft_optimize (FT_HANDLE brt) {
    int r = 0;

    TOKULOGGER logger = toku_cachefile_logger(brt->ft->cf);
    if (logger) {
        TXNID oldest = toku_txn_manager_get_oldest_living_xid(logger->txn_manager);

        XIDS root_xids = xids_get_root_xids();
        XIDS message_xids;
        if (oldest == TXNID_NONE_LIVING) {
            message_xids = root_xids;
        }
        else {
            r = xids_create_child(root_xids, &message_xids, oldest);
            invariant(r==0);
        }

        DBT key;
        DBT val;
        toku_init_dbt(&key);
        toku_init_dbt(&val);
        FT_MSG_S ftcmd = { FT_OPTIMIZE, ZERO_MSN, message_xids, .u.id={&key,&val}};
        r = toku_ft_root_put_cmd(brt->ft, &ftcmd);
        xids_destroy(&message_xids);
    }
    return r;
}

int
toku_ft_load(FT_HANDLE brt, TOKUTXN txn, char const * new_iname, int do_fsync, LSN *load_lsn) {
    int r = 0;
    FILENUM old_filenum = toku_cachefile_filenum(brt->ft->cf);
    int do_log = 1;
    r = toku_ft_load_recovery(txn, old_filenum, new_iname, do_fsync, do_log, load_lsn);
    return r;
}

// 2954
// brt actions for logging hot index filenums
int
toku_ft_hot_index(FT_HANDLE brt __attribute__ ((unused)), TOKUTXN txn, FILENUMS filenums, int do_fsync, LSN *lsn) {
    int r = 0;
    int do_log = 1;
    r = toku_ft_hot_index_recovery(txn, filenums, do_fsync, do_log, lsn);
    return r;
}

int
toku_ft_log_put (TOKUTXN txn, FT_HANDLE brt, const DBT *key, const DBT *val) {
    int r = 0;
    TOKULOGGER logger = toku_txn_logger(txn);
    if (logger && brt->ft->txnid_that_suppressed_recovery_logs == TXNID_NONE) {
        BYTESTRING keybs = {.len=key->size, .data=key->data};
        BYTESTRING valbs = {.len=val->size, .data=val->data};
        TXNID xid = toku_txn_get_txnid(txn);
        // if (type == FT_INSERT)
            r = toku_log_enq_insert(logger, (LSN*)0, 0, txn, toku_cachefile_filenum(brt->ft->cf), xid, keybs, valbs);
        // else
            // r = toku_log_enq_insert_no_overwrite(logger, (LSN*)0, 0, toku_cachefile_filenum(brt->ft->cf), xid, keybs, valbs);
    }
    return r;
}

int
toku_ft_log_put_multiple (TOKUTXN txn, FT_HANDLE src_ft, FT_HANDLE *brts, int num_fts, const DBT *key, const DBT *val) {
    int r = 0;
    assert(txn);
    assert(num_fts > 0);
    TOKULOGGER logger = toku_txn_logger(txn);
    if (logger) {
        FILENUM         fnums[num_fts];
        int i;
        int num_unsuppressed_fts = 0;
        for (i = 0; i < num_fts; i++) {
            if (brts[i]->ft->txnid_that_suppressed_recovery_logs == TXNID_NONE) {
                //Logging not suppressed for this brt.
                fnums[num_unsuppressed_fts++] = toku_cachefile_filenum(brts[i]->ft->cf);
            }
        }
        if (num_unsuppressed_fts) {
            FILENUMS filenums = {.num = num_unsuppressed_fts, .filenums = fnums};
            BYTESTRING keybs = {.len=key->size, .data=key->data};
            BYTESTRING valbs = {.len=val->size, .data=val->data};
            TXNID xid = toku_txn_get_txnid(txn);
            FILENUM src_filenum = src_ft ? toku_cachefile_filenum(src_ft->ft->cf) : FILENUM_NONE;
            r = toku_log_enq_insert_multiple(logger, (LSN*)0, 0, txn, src_filenum, filenums, xid, keybs, valbs);
        }
    }
    return r;
}

int
toku_ft_maybe_insert (FT_HANDLE ft_h, DBT *key, DBT *val, TOKUTXN txn, BOOL oplsn_valid, LSN oplsn, BOOL do_logging, enum ft_msg_type type) {
    assert(type==FT_INSERT || type==FT_INSERT_NO_OVERWRITE);
    int r = 0;
    XIDS message_xids = xids_get_root_xids(); //By default use committed messages
    TXNID xid = toku_txn_get_txnid(txn);
    if (txn) {
        if (ft_h->ft->txnid_that_created_or_locked_when_empty != xid) {
            BYTESTRING keybs  = {key->size, key->data};
            r = toku_logger_save_rollback_cmdinsert(txn, toku_cachefile_filenum(ft_h->ft->cf), &keybs);
            if (r!=0) return r;
            toku_txn_maybe_note_ft(txn, ft_h->ft);
            //We have transactions, and this is not 2440.  We must send the full root-to-leaf-path
            message_xids = toku_txn_get_xids(txn);
        }
        else if (txn->ancestor_txnid64 != ft_h->ft->h->root_xid_that_created) {
            //We have transactions, and this is 2440, however the txn doing 2440 did not create the dictionary.         We must send the full root-to-leaf-path
            message_xids = toku_txn_get_xids(txn);
        }
    }
    TOKULOGGER logger = toku_txn_logger(txn);
    if (do_logging && logger &&
        ft_h->ft->txnid_that_suppressed_recovery_logs == TXNID_NONE) {
        BYTESTRING keybs = {.len=key->size, .data=key->data};
        BYTESTRING valbs = {.len=val->size, .data=val->data};
        if (type == FT_INSERT) {
            r = toku_log_enq_insert(logger, (LSN*)0, 0, txn, toku_cachefile_filenum(ft_h->ft->cf), xid, keybs, valbs);
        }
        else {
            r = toku_log_enq_insert_no_overwrite(logger, (LSN*)0, 0, txn, toku_cachefile_filenum(ft_h->ft->cf), xid, keybs, valbs);
        }
        if (r!=0) return r;
    }

    LSN treelsn;
    if (oplsn_valid && oplsn.lsn <= (treelsn = toku_ft_checkpoint_lsn(ft_h->ft)).lsn) {
        r = 0;
    } else {
        r = toku_ft_send_insert(ft_h, key, val, message_xids, type);
    }
    return r;
}

static int
ft_send_update_msg(FT_HANDLE brt, FT_MSG_S *msg, TOKUTXN txn) {
    msg->xids = (txn
                 ? toku_txn_get_xids(txn)
                 : xids_get_root_xids());
    int r = toku_ft_root_put_cmd(brt->ft, msg);
    return r;
}

int
toku_ft_maybe_update(FT_HANDLE ft_h, const DBT *key, const DBT *update_function_extra,
                      TOKUTXN txn, BOOL oplsn_valid, LSN oplsn,
                      BOOL do_logging) {
    int r = 0;

    TXNID xid = toku_txn_get_txnid(txn);
    if (txn) {
        BYTESTRING keybs = { key->size, key->data };
        r = toku_logger_save_rollback_cmdupdate(
            txn, toku_cachefile_filenum(ft_h->ft->cf), &keybs);
        if (r != 0) { goto cleanup; }
        toku_txn_maybe_note_ft(txn, ft_h->ft);
    }

    TOKULOGGER logger = toku_txn_logger(txn);
    if (do_logging && logger &&
        ft_h->ft->txnid_that_suppressed_recovery_logs == TXNID_NONE) {
        BYTESTRING keybs = {.len=key->size, .data=key->data};
        BYTESTRING extrabs = {.len=update_function_extra->size,
                              .data=update_function_extra->data};
        r = toku_log_enq_update(logger, NULL, 0, txn,
                                toku_cachefile_filenum(ft_h->ft->cf),
                                xid, keybs, extrabs);
        if (r != 0) { goto cleanup; }
    }

    LSN treelsn;
    if (oplsn_valid &&
        oplsn.lsn <= (treelsn = toku_ft_checkpoint_lsn(ft_h->ft)).lsn) {
        r = 0;
    } else {
        FT_MSG_S msg = { FT_UPDATE, ZERO_MSN, NULL,
                          .u.id = { key, update_function_extra }};
        r = ft_send_update_msg(ft_h, &msg, txn);
    }

cleanup:
    return r;
}

int
toku_ft_maybe_update_broadcast(FT_HANDLE ft_h, const DBT *update_function_extra,
                                TOKUTXN txn, BOOL oplsn_valid, LSN oplsn,
                                BOOL do_logging, BOOL is_resetting_op) {
    int r = 0;

    TXNID xid = toku_txn_get_txnid(txn);
    u_int8_t  resetting = is_resetting_op ? 1 : 0;
    if (txn) {
        r = toku_logger_save_rollback_cmdupdatebroadcast(txn, toku_cachefile_filenum(ft_h->ft->cf), resetting);
        if (r != 0) { goto cleanup; }
        toku_txn_maybe_note_ft(txn, ft_h->ft);
    }

    TOKULOGGER logger = toku_txn_logger(txn);
    if (do_logging && logger &&
        ft_h->ft->txnid_that_suppressed_recovery_logs == TXNID_NONE) {
        BYTESTRING extrabs = {.len=update_function_extra->size,
                              .data=update_function_extra->data};
        r = toku_log_enq_updatebroadcast(logger, NULL, 0, txn,
                                         toku_cachefile_filenum(ft_h->ft->cf),
                                         xid, extrabs, resetting);
        if (r != 0) { goto cleanup; }
    }

    //TODO(yoni): remove treelsn here and similar calls (no longer being used)
    LSN treelsn;
    if (oplsn_valid &&
        oplsn.lsn <= (treelsn = toku_ft_checkpoint_lsn(ft_h->ft)).lsn) {
        r = 0;
    } else {
        DBT nullkey;
        const DBT *nullkeyp = toku_init_dbt(&nullkey);
        FT_MSG_S msg = { FT_UPDATE_BROADCAST_ALL, ZERO_MSN, NULL,
                          .u.id = { nullkeyp, update_function_extra }};
        r = ft_send_update_msg(ft_h, &msg, txn);
    }

cleanup:
    return r;
}

int
toku_ft_send_insert(FT_HANDLE brt, DBT *key, DBT *val, XIDS xids, enum ft_msg_type type) {
    FT_MSG_S ftcmd = { type, ZERO_MSN, xids, .u.id = { key, val }};
    int r = toku_ft_root_put_cmd(brt->ft, &ftcmd);
    return r;
}

int
toku_ft_send_commit_any(FT_HANDLE brt, DBT *key, XIDS xids) {
    DBT val;
    FT_MSG_S ftcmd = { FT_COMMIT_ANY, ZERO_MSN, xids, .u.id = { key, toku_init_dbt(&val) }};
    int r = toku_ft_root_put_cmd(brt->ft, &ftcmd);
    return r;
}

int toku_ft_delete(FT_HANDLE brt, DBT *key, TOKUTXN txn) {
    return toku_ft_maybe_delete(brt, key, txn, FALSE, ZERO_LSN, TRUE);
}

int
toku_ft_log_del(TOKUTXN txn, FT_HANDLE brt, const DBT *key) {
    int r = 0;
    TOKULOGGER logger = toku_txn_logger(txn);
    if (logger && brt->ft->txnid_that_suppressed_recovery_logs == TXNID_NONE) {
        BYTESTRING keybs = {.len=key->size, .data=key->data};
        TXNID xid = toku_txn_get_txnid(txn);
        r = toku_log_enq_delete_any(logger, (LSN*)0, 0, txn, toku_cachefile_filenum(brt->ft->cf), xid, keybs);
    }
    return r;
}

int
toku_ft_log_del_multiple (TOKUTXN txn, FT_HANDLE src_ft, FT_HANDLE *brts, int num_fts, const DBT *key, const DBT *val) {
    int r = 0;
    assert(txn);
    assert(num_fts > 0);
    TOKULOGGER logger = toku_txn_logger(txn);
    if (logger) {
        FILENUM         fnums[num_fts];
        int i;
        int num_unsuppressed_fts = 0;
        for (i = 0; i < num_fts; i++) {
            if (brts[i]->ft->txnid_that_suppressed_recovery_logs == TXNID_NONE) {
                //Logging not suppressed for this brt.
                fnums[num_unsuppressed_fts++] = toku_cachefile_filenum(brts[i]->ft->cf);
            }
        }
        if (num_unsuppressed_fts) {
            FILENUMS filenums = {.num = num_unsuppressed_fts, .filenums = fnums};
            BYTESTRING keybs = {.len=key->size, .data=key->data};
            BYTESTRING valbs = {.len=val->size, .data=val->data};
            TXNID xid = toku_txn_get_txnid(txn);
            FILENUM src_filenum = src_ft ? toku_cachefile_filenum(src_ft->ft->cf) : FILENUM_NONE;
            r = toku_log_enq_delete_multiple(logger, (LSN*)0, 0, txn, src_filenum, filenums, xid, keybs, valbs);
        }
    }
    return r;
}

int
toku_ft_maybe_delete(FT_HANDLE ft_h, DBT *key, TOKUTXN txn, BOOL oplsn_valid, LSN oplsn, BOOL do_logging) {
    int r;
    XIDS message_xids = xids_get_root_xids(); //By default use committed messages
    TXNID xid = toku_txn_get_txnid(txn);
    if (txn) {
        if (ft_h->ft->txnid_that_created_or_locked_when_empty != xid) {
            BYTESTRING keybs  = {key->size, key->data};
            r = toku_logger_save_rollback_cmddelete(txn, toku_cachefile_filenum(ft_h->ft->cf), &keybs);
            if (r!=0) return r;
            toku_txn_maybe_note_ft(txn, ft_h->ft);
            //We have transactions, and this is not 2440.  We must send the full root-to-leaf-path
            message_xids = toku_txn_get_xids(txn);
        }
        else if (txn->ancestor_txnid64 != ft_h->ft->h->root_xid_that_created) {
            //We have transactions, and this is 2440, however the txn doing 2440 did not create the dictionary.         We must send the full root-to-leaf-path
            message_xids = toku_txn_get_xids(txn);
        }
    }
    TOKULOGGER logger = toku_txn_logger(txn);
    if (do_logging && logger &&
        ft_h->ft->txnid_that_suppressed_recovery_logs == TXNID_NONE) {
        BYTESTRING keybs = {.len=key->size, .data=key->data};
        r = toku_log_enq_delete_any(logger, (LSN*)0, 0, txn, toku_cachefile_filenum(ft_h->ft->cf), xid, keybs);
        if (r!=0) return r;
    }

    LSN treelsn;
    if (oplsn_valid && oplsn.lsn <= (treelsn = toku_ft_checkpoint_lsn(ft_h->ft)).lsn) {
        r = 0;
    } else {
        r = toku_ft_send_delete(ft_h, key, message_xids);
    }
    return r;
}

int
toku_ft_send_delete(FT_HANDLE brt, DBT *key, XIDS xids) {
    DBT val; toku_init_dbt(&val);
    FT_MSG_S ftcmd = { FT_DELETE_ANY, ZERO_MSN, xids, .u.id = { key, &val }};
    int result = toku_ft_root_put_cmd(brt->ft, &ftcmd);
    return result;
}

/* mempool support */

struct omt_compressor_state {
    struct mempool *new_kvspace;
    OMT omt;
};

static int move_it (OMTVALUE lev, u_int32_t idx, void *v) {
    LEAFENTRY le=lev;
    struct omt_compressor_state *oc = v;
    u_int32_t size = leafentry_memsize(le);
    LEAFENTRY newdata = toku_mempool_malloc(oc->new_kvspace, size, 1);
    lazy_assert(newdata); // we do this on a fresh mempool, so nothing bad shouldhapepn
    memcpy(newdata, le, size);
    toku_omt_set_at(oc->omt, newdata, idx);
    return 0;
}

// Compress things, and grow the mempool if needed.
// TODO 4092 should copy data to new memory, then call toku_mempool_destory() followed by toku_mempool_init()
static int omt_compress_kvspace (OMT omt, struct mempool *memp, size_t added_size, void **maybe_free) {
    u_int32_t total_size_needed = memp->free_offset-memp->frag_size + added_size;
    if (total_size_needed+total_size_needed/4 >= memp->size) {
        memp->size = total_size_needed+total_size_needed/4;
    }
    void *newmem = toku_xmalloc(memp->size);
    struct mempool new_kvspace;
    toku_mempool_init(&new_kvspace, newmem, memp->size);
    struct omt_compressor_state oc = { &new_kvspace, omt };
    toku_omt_iterate(omt, move_it, &oc);

    if (maybe_free) {
        *maybe_free = memp->base;
    } else {
        toku_free(memp->base);
    }
    *memp = new_kvspace;
    return 0;
}

void *
mempool_malloc_from_omt(OMT omt, struct mempool *mp, size_t size, void **maybe_free) {
    void *v = toku_mempool_malloc(mp, size, 1);
    if (v==0) {
        if (0 == omt_compress_kvspace(omt, mp, size, maybe_free)) {
            v = toku_mempool_malloc(mp, size, 1);
            lazy_assert(v);
        }
    }
    return v;
}

/* ******************** open,close and create  ********************** */

// Test only function (not used in running system). This one has no env
int toku_open_ft_handle (const char *fname, int is_create, FT_HANDLE *ft_handle_p, int nodesize,
                   int basementnodesize,
                   enum toku_compression_method compression_method,
                   CACHETABLE cachetable, TOKUTXN txn,
                   int (*compare_fun)(DB *, const DBT*,const DBT*)) {
    FT_HANDLE brt;
    int r;
    const int only_create = 0;

    r = toku_ft_handle_create(&brt);
    if (r != 0)
        return r;
    toku_ft_handle_set_nodesize(brt, nodesize);
    toku_ft_handle_set_basementnodesize(brt, basementnodesize);
    toku_ft_handle_set_compression_method(brt, compression_method);
    r = toku_ft_set_bt_compare(brt, compare_fun); assert_zero(r);

    r = toku_ft_handle_open(brt, fname, is_create, only_create, cachetable, txn);
    if (r != 0) {
        return r;
    }

    *ft_handle_p = brt;
    return r;
}

// open a file for use by the brt
// Requires:  File does not exist.
static int ft_create_file(FT_HANDLE UU(brt), const char *fname, int *fdp) {
    mode_t mode = S_IRWXU|S_IRWXG|S_IRWXO;
    int r;
    int fd;
    fd = open(fname, O_RDWR | O_BINARY, mode);
    assert(fd==-1);
    if (errno != ENOENT) {
        r = errno;
        return r;
    }
    fd = open(fname, O_RDWR | O_CREAT | O_BINARY, mode);
    if (fd==-1) {
        r = errno;
        return r;
    }

    r = toku_fsync_directory(fname);
    resource_assert_zero(r);

    *fdp = fd;
    return 0;
}

// open a file for use by the brt.  if the file does not exist, error
static int ft_open_file(const char *fname, int *fdp) {
    mode_t mode = S_IRWXU|S_IRWXG|S_IRWXO;
    int r;
    int fd;
    fd = open(fname, O_RDWR | O_BINARY, mode);
    if (fd==-1) {
        r = errno;
        assert(r!=0);
        return r;
    }
    *fdp = fd;
    return 0;
}

void
toku_ft_handle_set_compression_method(FT_HANDLE t, enum toku_compression_method method)
{
    if (t->ft) {
        toku_ft_set_compression_method(t->ft, method);
    }
    else {
        t->options.compression_method = method;
    }
}

void
toku_ft_handle_get_compression_method(FT_HANDLE t, enum toku_compression_method *methodp)
{
    if (t->ft) {
        toku_ft_get_compression_method(t->ft, methodp);
    }
    else {
        *methodp = t->options.compression_method;
    }
}

static int
verify_builtin_comparisons_consistent(FT_HANDLE t, u_int32_t flags) {
    if ((flags & TOKU_DB_KEYCMP_BUILTIN) && (t->options.compare_fun != toku_builtin_compare_fun))
        return EINVAL;
    return 0;
}

//
// See comments in toku_db_change_descriptor to understand invariants 
// in the system when this function is called
//
int
toku_ft_change_descriptor(
    FT_HANDLE ft_h,
    const DBT* old_descriptor,
    const DBT* new_descriptor,
    BOOL do_log,
    TOKUTXN txn,
    BOOL update_cmp_descriptor
    )
{
    int r = 0;
    DESCRIPTOR_S new_d;
    BYTESTRING old_desc_bs = { old_descriptor->size, old_descriptor->data };
    BYTESTRING new_desc_bs = { new_descriptor->size, new_descriptor->data };
    if (!txn) {
        r = EINVAL;
        goto cleanup;
    }
    // put information into rollback file
    r = toku_logger_save_rollback_change_fdescriptor(
        txn,
        toku_cachefile_filenum(ft_h->ft->cf),
        &old_desc_bs
        );
    if (r != 0) { goto cleanup; }
    toku_txn_maybe_note_ft(txn, ft_h->ft);

    if (do_log) {
        TOKULOGGER logger = toku_txn_logger(txn);
        TXNID xid = toku_txn_get_txnid(txn);
        r = toku_log_change_fdescriptor(
            logger, NULL, 0,
            txn,
            toku_cachefile_filenum(ft_h->ft->cf),
            xid,
            old_desc_bs,
            new_desc_bs,
            update_cmp_descriptor
            );
        if (r != 0) { goto cleanup; }
    }

    // write new_descriptor to header
    new_d.dbt = *new_descriptor;
    toku_ft_update_descriptor(ft_h->ft, &new_d);
    // very infrequent operation, worth precise threadsafe count
    if (r == 0) {
        STATUS_VALUE(FT_DESCRIPTOR_SET)++;
    }
    if (r!=0) goto cleanup;

    if (update_cmp_descriptor) {
        toku_ft_update_cmp_descriptor(ft_h->ft);
    }
cleanup:
    return r;
}

static void
toku_ft_handle_inherit_options(FT_HANDLE t, FT ft) {
    struct ft_options options = {
        .nodesize = ft->h->nodesize,
        .basementnodesize = ft->h->basementnodesize,
        .compression_method = ft->h->compression_method,
        .flags = ft->h->flags,
        .compare_fun = ft->compare_fun,
        .update_fun = ft->update_fun
    };
    t->options = options;
    t->did_set_flags = TRUE;
}

// This is the actual open, used for various purposes, such as normal use, recovery, and redirect.
// fname_in_env is the iname, relative to the env_dir  (data_dir is already in iname as prefix).
// The checkpointed version (checkpoint_lsn) of the dictionary must be no later than max_acceptable_lsn .
// Requires: The multi-operation client lock must be held to prevent a checkpoint from occuring.
static int
ft_handle_open(FT_HANDLE ft_h, const char *fname_in_env, int is_create, int only_create, CACHETABLE cachetable, TOKUTXN txn, FILENUM use_filenum, DICTIONARY_ID use_dictionary_id, LSN max_acceptable_lsn) {
    int r;
    BOOL txn_created = FALSE;
    char *fname_in_cwd = NULL;
    CACHEFILE cf = NULL;
    FT ft = NULL;
    BOOL did_create = FALSE;
    toku_ft_open_close_lock();

    if (ft_h->did_set_flags) {
        r = verify_builtin_comparisons_consistent(ft_h, ft_h->options.flags);
        if (r!=0) { goto exit; }
    }
    if (txn && txn->logger->is_panicked) {
        r = EINVAL;
        goto exit;
    }

    assert(is_create || !only_create);
    FILENUM reserved_filenum = use_filenum;
    fname_in_cwd = toku_cachetable_get_fname_in_cwd(cachetable, fname_in_env);
    {
        int fd = -1;
        r = ft_open_file(fname_in_cwd, &fd);
        if (reserved_filenum.fileid == FILENUM_NONE.fileid) {
            reserved_filenum = toku_cachetable_reserve_filenum(cachetable);
        }
        if (r==ENOENT && is_create) {
            did_create = TRUE;
            mode_t mode = S_IRWXU|S_IRWXG|S_IRWXO;
            if (txn) {
                BYTESTRING bs = { .len=strlen(fname_in_env), .data = (char*)fname_in_env };
                r = toku_logger_save_rollback_fcreate(txn, reserved_filenum, &bs); // bs is a copy of the fname relative to the environment
                assert_zero(r);
            }
            txn_created = (BOOL)(txn!=NULL);
            r = toku_logger_log_fcreate(txn, fname_in_env, reserved_filenum, mode, ft_h->options.flags, ft_h->options.nodesize, ft_h->options.basementnodesize, ft_h->options.compression_method);
            assert_zero(r); // only possible failure is panic, which we check above
            r = ft_create_file(ft_h, fname_in_cwd, &fd);
            assert_zero(r);
        }
        if (r) { goto exit; }
        r=toku_cachetable_openfd_with_filenum(&cf, cachetable, fd, fname_in_env, reserved_filenum);
        if (r) { goto exit; }
    }
    assert(ft_h->options.nodesize>0);
    BOOL was_already_open;
    if (is_create) {
        r = toku_read_ft_and_store_in_cachefile(ft_h, cf, max_acceptable_lsn, &ft, &was_already_open);
        if (r==TOKUDB_DICTIONARY_NO_HEADER) {
            r = toku_create_new_ft(&ft, &ft_h->options, cf, txn);
            if (r) { goto exit; }
        }
        else if (r!=0) {
            goto exit;
        }
        else if (only_create) {
            assert_zero(r);
            r = EEXIST;
            goto exit;
        }
        // if we get here, then is_create was true but only_create was false,
        // so it is ok for toku_read_ft_and_store_in_cachefile to have read
        // the header via toku_read_ft_and_store_in_cachefile
    } else {
        r = toku_read_ft_and_store_in_cachefile(ft_h, cf, max_acceptable_lsn, &ft, &was_already_open);
        if (r) { goto exit; }
    }
    if (!ft_h->did_set_flags) {
        r = verify_builtin_comparisons_consistent(ft_h, ft_h->options.flags);
        if (r) { goto exit; }
    } else if (ft_h->options.flags != ft->h->flags) {                  /* if flags have been set then flags must match */
        r = EINVAL;
        goto exit;
    }
    toku_ft_handle_inherit_options(ft_h, ft);

    if (!was_already_open) {
        if (!did_create) { //Only log the fopen that OPENs the file.  If it was already open, don't log.
            r = toku_logger_log_fopen(txn, fname_in_env, toku_cachefile_filenum(cf), ft_h->options.flags);
            assert_zero(r);
        }
    }
    int use_reserved_dict_id = use_dictionary_id.dictid != DICTIONARY_ID_NONE.dictid;
    if (!was_already_open) {
        DICTIONARY_ID dict_id;
        if (use_reserved_dict_id) {
            dict_id = use_dictionary_id;
        }
        else {
            dict_id = next_dict_id();
        }
        ft->dict_id = dict_id;
    }
    else {
        // dict_id is already in header
        if (use_reserved_dict_id) {
            assert(ft->dict_id.dictid == use_dictionary_id.dictid);
        }
    }
    assert(ft);
    assert(ft->dict_id.dictid != DICTIONARY_ID_NONE.dictid);
    assert(ft->dict_id.dictid < dict_id_serial);

    // important note here,
    // after this point, where we associate the header
    // with the brt, the function is not allowed to fail
    // Code that handles failure (located below "exit"),
    // depends on this
    toku_ft_note_ft_handle_open(ft, ft_h);
    if (txn_created) {
        assert(txn);
        toku_ft_suppress_rollbacks(ft, txn);
        toku_txn_maybe_note_ft(txn, ft);
    }

    //Opening a brt may restore to previous checkpoint.         Truncate if necessary.
    {
        int fd = toku_cachefile_get_fd (ft->cf);
        toku_maybe_truncate_file_on_open(ft->blocktable, fd);
    }

    r = 0;
exit:
    if (fname_in_cwd) {
        toku_free(fname_in_cwd);
    }
    if (r != 0 && cf) {
        if (ft) {
            // we only call toku_ft_note_ft_handle_open
            // when the function succeeds, so if we are here,
            // then that means we have a reference to the header
            // but we have not linked it to this brt. So,
            // we can simply try to remove the header.
            // We don't need to unlink this brt from the header
            toku_ft_grab_reflock(ft);
            BOOL needed = toku_ft_needed_unlocked(ft);
            toku_ft_release_reflock(ft);
            if (!needed) {
                //Close immediately.
                char *error_string = NULL;
                r = toku_ft_evict_from_memory(ft, &error_string, false, ZERO_LSN);
                lazy_assert_zero(r);
            }
        }
        else {
            toku_cachefile_close(&cf, 0, FALSE, ZERO_LSN);
        }
    }
    toku_ft_open_close_unlock();
    return r;
}

// Open a brt for the purpose of recovery, which requires that the brt be open to a pre-determined FILENUM
// and may require a specific checkpointed version of the file.
// (dict_id is assigned by the ft_handle_open() function.)
int
toku_ft_handle_open_recovery(FT_HANDLE t, const char *fname_in_env, int is_create, int only_create, CACHETABLE cachetable, TOKUTXN txn, FILENUM use_filenum, LSN max_acceptable_lsn) {
    int r;
    assert(use_filenum.fileid != FILENUM_NONE.fileid);
    r = ft_handle_open(t, fname_in_env, is_create, only_create, cachetable,
                 txn, use_filenum, DICTIONARY_ID_NONE, max_acceptable_lsn);
    return r;
}

// Open a brt in normal use.  The FILENUM and dict_id are assigned by the ft_handle_open() function.
// Requires: The multi-operation client lock must be held to prevent a checkpoint from occuring.
int
toku_ft_handle_open(FT_HANDLE t, const char *fname_in_env, int is_create, int only_create, CACHETABLE cachetable, TOKUTXN txn) {
    int r;
    r = ft_handle_open(t, fname_in_env, is_create, only_create, cachetable, txn, FILENUM_NONE, DICTIONARY_ID_NONE, MAX_LSN);
    return r;
}

// clone an ft handle. the cloned handle has a new dict_id but refers to the same fractal tree
int 
toku_ft_handle_clone(FT_HANDLE *cloned_ft_handle, FT_HANDLE ft_handle, TOKUTXN txn) {
    int r;
    FT_HANDLE result_ft_handle; 
    r = toku_ft_handle_create(&result_ft_handle);
    resource_assert_zero(r);

    // we're cloning, so the handle better have an open ft and open cf
    invariant(ft_handle->ft);
    invariant(ft_handle->ft->cf);

    // inherit the options of the ft whose handle is being cloned.
    toku_ft_handle_inherit_options(result_ft_handle, ft_handle->ft);

    // we can clone the handle by creating a new handle with the same fname
    CACHEFILE cf = ft_handle->ft->cf;
    CACHETABLE ct = toku_cachefile_get_cachetable(cf);
    const char *fname_in_env = toku_cachefile_fname_in_env(cf);
    r = toku_ft_handle_open(result_ft_handle, fname_in_env, false, false, ct, txn); 
    if (r != 0) {
        toku_ft_handle_close(result_ft_handle);
        result_ft_handle = NULL;
    }
    *cloned_ft_handle = result_ft_handle;
    return r;
}

// Open a brt in normal use.  The FILENUM and dict_id are assigned by the ft_handle_open() function.
int
toku_ft_handle_open_with_dict_id(
    FT_HANDLE t,
    const char *fname_in_env,
    int is_create,
    int only_create,
    CACHETABLE cachetable,
    TOKUTXN txn,
    DICTIONARY_ID use_dictionary_id
    )
{
    int r;
    r = ft_handle_open(
        t,
        fname_in_env,
        is_create,
        only_create,
        cachetable,
        txn,
        FILENUM_NONE,
        use_dictionary_id,
        MAX_LSN
        );
    return r;
}

DICTIONARY_ID
toku_ft_get_dictionary_id(FT_HANDLE brt) {
    FT h = brt->ft;
    DICTIONARY_ID dict_id = h->dict_id;
    return dict_id;
}

int toku_ft_set_flags(FT_HANDLE brt, unsigned int flags) {
    assert(flags==(flags&TOKU_DB_KEYCMP_BUILTIN)); // make sure there are no extraneous flags
    brt->did_set_flags = TRUE;
    brt->options.flags = flags;
    return 0;
}

int toku_ft_get_flags(FT_HANDLE brt, unsigned int *flags) {
    *flags = brt->options.flags;
    assert(brt->options.flags==(brt->options.flags&TOKU_DB_KEYCMP_BUILTIN)); // make sure there are no extraneous flags
    return 0;
}

void toku_ft_get_maximum_advised_key_value_lengths (unsigned int *max_key_len, unsigned int *max_val_len)
// return the maximum advisable key value lengths.  The brt doesn't enforce these.
{
    *max_key_len = 32*1024;
    *max_val_len = 32*1024*1024;
}


void toku_ft_handle_set_nodesize(FT_HANDLE ft_handle, unsigned int nodesize) {
    if (ft_handle->ft) {
        toku_ft_set_nodesize(ft_handle->ft, nodesize);
    }
    else {
        ft_handle->options.nodesize = nodesize;
    }
}

void toku_ft_handle_get_nodesize(FT_HANDLE ft_handle, unsigned int *nodesize) {
    if (ft_handle->ft) {
        toku_ft_get_nodesize(ft_handle->ft, nodesize);
    }
    else {
        *nodesize = ft_handle->options.nodesize;
    }
}

void toku_ft_handle_set_basementnodesize(FT_HANDLE ft_handle, unsigned int basementnodesize) {
    if (ft_handle->ft) {
        toku_ft_set_basementnodesize(ft_handle->ft, basementnodesize);
    }
    else {
        ft_handle->options.basementnodesize = basementnodesize;
    }
}

void toku_ft_handle_get_basementnodesize(FT_HANDLE ft_handle, unsigned int *basementnodesize) {
    if (ft_handle->ft) {
        toku_ft_get_basementnodesize(ft_handle->ft, basementnodesize);
    }
    else {
        *basementnodesize = ft_handle->options.basementnodesize;
    }
}

int toku_ft_set_bt_compare(FT_HANDLE brt, int (*bt_compare)(DB*, const DBT*, const DBT*)) {
    brt->options.compare_fun = bt_compare;
    return 0;
}

void toku_ft_set_redirect_callback(FT_HANDLE brt, on_redirect_callback redir_cb, void* extra) {
    brt->redirect_callback = redir_cb;
    brt->redirect_callback_extra = extra;
}


int toku_ft_set_update(FT_HANDLE brt, ft_update_func update_fun) {
    brt->options.update_fun = update_fun;
    return 0;
}

ft_compare_func toku_ft_get_bt_compare (FT_HANDLE brt) {
    return brt->options.compare_fun;
}

static void
ft_remove_handle_ref_callback(FT UU(ft), void *extra) {
    FT_HANDLE handle = extra;
    toku_list_remove(&handle->live_ft_handle_link);
}

// close an ft handle during normal operation. the underlying ft may or may not close,
// depending if there are still references. an lsn for this close will come from the logger.
void
toku_ft_handle_close(FT_HANDLE ft_handle) {
    // There are error paths in the ft_handle_open that end with ft_handle->ft==NULL.
    FT ft = ft_handle->ft;
    if (ft) {
        const bool oplsn_valid = false;
        toku_ft_remove_reference(ft, oplsn_valid, ZERO_LSN, ft_remove_handle_ref_callback, ft_handle);
    }
    toku_free(ft_handle);
}

// close an ft handle during recovery. the underlying ft must close, and will use the given lsn.
void 
toku_ft_handle_close_recovery(FT_HANDLE ft_handle, LSN oplsn) {
    FT ft = ft_handle->ft;
    // the ft must exist if closing during recovery. error paths during 
    // open for recovery should close handles using toku_ft_handle_close()
    assert(ft);
    const bool oplsn_valid = true;
    toku_ft_remove_reference(ft, oplsn_valid, oplsn, ft_remove_handle_ref_callback, ft_handle);
    toku_free(ft_handle);
}

// TODO: remove this, callers should instead just use toku_ft_handle_close()
int 
toku_close_ft_handle_nolsn (FT_HANDLE ft_handle, char** UU(error_string)) {
    toku_ft_handle_close(ft_handle);
    return 0;
}

int toku_ft_handle_create(FT_HANDLE *ft_handle_ptr) {
    FT_HANDLE MALLOC(brt);
    if (brt == 0)
        return ENOMEM;
    memset(brt, 0, sizeof *brt);
    toku_list_init(&brt->live_ft_handle_link);
    brt->options.flags = 0;
    brt->did_set_flags = FALSE;
    brt->options.nodesize = FT_DEFAULT_NODE_SIZE;
    brt->options.basementnodesize = FT_DEFAULT_BASEMENT_NODE_SIZE;
    brt->options.compression_method = TOKU_DEFAULT_COMPRESSION_METHOD;
    brt->options.compare_fun = toku_builtin_compare_fun;
    brt->options.update_fun = NULL;
    *ft_handle_ptr = brt;
    return 0;
}

/* ************* CURSORS ********************* */

static inline void
ft_cursor_cleanup_dbts(FT_CURSOR c) {
    if (c->key.data) toku_free(c->key.data);
    if (c->val.data) toku_free(c->val.data);
    memset(&c->key, 0, sizeof(c->key));
    memset(&c->val, 0, sizeof(c->val));
}

//
// This function is used by the leafentry iterators.
// returns TOKUDB_ACCEPT if live transaction context is allowed to read a value
// that is written by transaction with LSN of id
// live transaction context may read value if either id is the root ancestor of context, or if
// id was committed before context's snapshot was taken.
// For id to be committed before context's snapshot was taken, the following must be true:
//  - id < context->snapshot_txnid64 AND id is not in context's live root transaction list
// For the above to NOT be true:
//  - id > context->snapshot_txnid64 OR id is in context's live root transaction list
//
static int
does_txn_read_entry(TXNID id, TOKUTXN context) {
    int rval;
    TXNID oldest_live_in_snapshot = toku_get_oldest_in_live_root_txn_list(context);
    if (id < oldest_live_in_snapshot || id == context->ancestor_txnid64) {
        rval = TOKUDB_ACCEPT;
    }
    else if (id > context->snapshot_txnid64 || toku_is_txn_in_live_root_txn_list(context->live_root_txn_list, id)) {
        rval = 0;
    }
    else {
        rval = TOKUDB_ACCEPT;
    }
    return rval;
}

static inline void
ft_cursor_extract_key_and_val(LEAFENTRY le,
                               FT_CURSOR cursor,
                               u_int32_t *keylen,
                               void            **key,
                               u_int32_t *vallen,
                               void            **val) {
    if (toku_ft_cursor_is_leaf_mode(cursor)) {
        *key = le_key_and_len(le, keylen);
        *val = le;
        *vallen = leafentry_memsize(le);
    } else if (cursor->is_snapshot_read) {
        le_iterate_val(
            le,
            does_txn_read_entry,
            val,
            vallen,
            cursor->ttxn
            );
        *key = le_key_and_len(le, keylen);
    } else {
        *key = le_key_and_len(le, keylen);
        *val = le_latest_val_and_len(le, vallen);
    }
}

int toku_ft_cursor (
    FT_HANDLE brt,
    FT_CURSOR *cursorptr,
    TOKUTXN ttxn,
    BOOL is_snapshot_read,
    BOOL disable_prefetching
    )
{
    if (is_snapshot_read) {
        invariant(ttxn != NULL);
        int accepted = does_txn_read_entry(brt->ft->h->root_xid_that_created, ttxn);
        if (accepted!=TOKUDB_ACCEPT) {
            invariant(accepted==0);
            return TOKUDB_MVCC_DICTIONARY_TOO_NEW;
        }
    }
    FT_CURSOR cursor = toku_malloc(sizeof *cursor);
    // if this cursor is to do read_committed fetches, then the txn objects must be valid.
    if (cursor == 0)
        return ENOMEM;
    memset(cursor, 0, sizeof(*cursor));
    cursor->ft_handle = brt;
    cursor->prefetching = FALSE;
    toku_init_dbt(&cursor->range_lock_left_key);
    toku_init_dbt(&cursor->range_lock_right_key);
    cursor->left_is_neg_infty = FALSE;
    cursor->right_is_pos_infty = FALSE;
    cursor->is_snapshot_read = is_snapshot_read;
    cursor->is_leaf_mode = FALSE;
    cursor->ttxn = ttxn;
    cursor->disable_prefetching = disable_prefetching;
    cursor->is_temporary = FALSE;
    *cursorptr = cursor;
    return 0;
}

void
toku_ft_cursor_set_temporary(FT_CURSOR ftcursor) {
    ftcursor->is_temporary = TRUE;
}

void
toku_ft_cursor_set_leaf_mode(FT_CURSOR ftcursor) {
    ftcursor->is_leaf_mode = TRUE;
}

int
toku_ft_cursor_is_leaf_mode(FT_CURSOR ftcursor) {
    return ftcursor->is_leaf_mode;
}

void
toku_ft_cursor_set_range_lock(FT_CURSOR cursor, const DBT *left, const DBT *right,
                               BOOL left_is_neg_infty, BOOL right_is_pos_infty)
{
    if (cursor->range_lock_left_key.data) {
        toku_free(cursor->range_lock_left_key.data);
        toku_init_dbt(&cursor->range_lock_left_key);
    }
    if (cursor->range_lock_right_key.data) {
        toku_free(cursor->range_lock_right_key.data);
        toku_init_dbt(&cursor->range_lock_right_key);
    }

    if (left_is_neg_infty) {
        cursor->left_is_neg_infty = TRUE;
    } else {
        toku_fill_dbt(&cursor->range_lock_left_key,
                      toku_xmemdup(left->data, left->size), left->size);
    }
    if (right_is_pos_infty) {
        cursor->right_is_pos_infty = TRUE;
    } else {
        toku_fill_dbt(&cursor->range_lock_right_key,
                      toku_xmemdup(right->data, right->size), right->size);
    }
}

int toku_ft_cursor_close(FT_CURSOR cursor) {
    ft_cursor_cleanup_dbts(cursor);
    if (cursor->range_lock_left_key.data) {
        toku_free(cursor->range_lock_left_key.data);
        toku_destroy_dbt(&cursor->range_lock_left_key);
    }
    if (cursor->range_lock_right_key.data) {
        toku_free(cursor->range_lock_right_key.data);
        toku_destroy_dbt(&cursor->range_lock_right_key);
    }
    toku_free(cursor);
    return 0;
}

static inline void ft_cursor_set_prefetching(FT_CURSOR cursor) {
    cursor->prefetching = TRUE;
}

static inline BOOL ft_cursor_prefetching(FT_CURSOR cursor) {
    return cursor->prefetching;
}

//Return TRUE if cursor is uninitialized.  FALSE otherwise.
static BOOL
ft_cursor_not_set(FT_CURSOR cursor) {
    assert((cursor->key.data==NULL) == (cursor->val.data==NULL));
    return (BOOL)(cursor->key.data == NULL);
}

static int
pair_leafval_heaviside_le (u_int32_t klen, void *kval,
                           ft_search_t *search) {
    DBT x;
    int cmp = search->compare(search,
                              search->k ? toku_fill_dbt(&x, kval, klen) : 0);
    // The search->compare function returns only 0 or 1
    switch (search->direction) {
    case FT_SEARCH_LEFT:   return cmp==0 ? -1 : +1;
    case FT_SEARCH_RIGHT:  return cmp==0 ? +1 : -1; // Because the comparison runs backwards for right searches.
    }
    abort(); return 0;
}


static int
heaviside_from_search_t (OMTVALUE lev, void *extra) {
    LEAFENTRY le=lev;
    ft_search_t *search = extra;
    u_int32_t keylen;
    void* key = le_key_and_len(le, &keylen);

    return pair_leafval_heaviside_le (keylen, key,
                                      search);
}


//
// Returns true if the value that is to be read is empty.
//
static inline int
is_le_val_del(LEAFENTRY le, FT_CURSOR ftcursor) {
    int rval;
    if (ftcursor->is_snapshot_read) {
        BOOL is_del;
        le_iterate_is_del(
            le,
            does_txn_read_entry,
            &is_del,
            ftcursor->ttxn
            );
        rval = is_del;
    }
    else {
        rval = le_latest_is_del(le);
    }
    return rval;
}

static const DBT zero_dbt = {0,0,0,0};

static void search_save_bound (ft_search_t *search, DBT *pivot) {
    if (search->have_pivot_bound) {
        toku_free(search->pivot_bound.data);
    }
    search->pivot_bound = zero_dbt;
    search->pivot_bound.data = toku_malloc(pivot->size);
    search->pivot_bound.size = pivot->size;
    memcpy(search->pivot_bound.data, pivot->data, pivot->size);
    search->have_pivot_bound = TRUE;
}

static BOOL search_pivot_is_bounded (ft_search_t *search, DESCRIPTOR desc, ft_compare_func cmp, DBT *pivot) __attribute__((unused));
static BOOL search_pivot_is_bounded (ft_search_t *search, DESCRIPTOR desc, ft_compare_func cmp, DBT *pivot)
// Effect:  Return TRUE iff the pivot has already been searched (for fixing #3522.)
//  If searching from left to right, if we have already searched all the values less than pivot, we don't want to search again.
//  If searching from right to left, if we have already searched all the vlaues greater than pivot, we don't want to search again.
{
    if (!search->have_pivot_bound) return TRUE; // isn't bounded.
    FAKE_DB(db, desc);
    int comp = cmp(&db, pivot, &search->pivot_bound);
    if (search->direction == FT_SEARCH_LEFT) {
        // searching from left to right.  If the comparison function says the pivot is <= something we already compared, don't do it again.
        return comp>0;
    } else {
        return comp<0;
    }
}

struct copy_to_stale_extra {
    FT_HANDLE ft_handle;
    NONLEAF_CHILDINFO bnc;
};

static int
copy_to_stale(OMTVALUE v, u_int32_t UU(idx), void *extrap)
{
    struct copy_to_stale_extra *extra = extrap;
    const long offset = (long) v;
    struct fifo_entry *entry = (struct fifo_entry *) toku_fifo_get_entry(extra->bnc->buffer, offset);
    entry->is_fresh = false;
    DBT keydbt;
    DBT *key = fill_dbt_for_fifo_entry(&keydbt, entry);
    struct toku_fifo_entry_key_msn_heaviside_extra heaviside_extra = { .desc = &extra->ft_handle->ft->cmp_descriptor, .cmp = extra->ft_handle->ft->compare_fun, .fifo = extra->bnc->buffer, .key = key, .msn = entry->msn };
    int r = toku_omt_insert(extra->bnc->stale_message_tree, (OMTVALUE) offset, toku_fifo_entry_key_msn_heaviside, &heaviside_extra, NULL);
    assert_zero(r);
    return r;
}

struct store_fifo_offset_extra {
    long *offsets;
    int i;
};

static int
store_fifo_offset(OMTVALUE v, u_int32_t UU(idx), void *extrap)
{
    struct store_fifo_offset_extra *extra = extrap;
    const long offset = (long) v;
    extra->offsets[extra->i] = offset;
    extra->i++;
    return 0;
}

/**
 * Given pointers to offsets within a FIFO where we can find messages,
 * figure out the MSN of each message, and compare those MSNs.  Returns 1,
 * 0, or -1 if a is larger than, equal to, or smaller than b.
 */
static int
fifo_offset_msn_cmp(void *extrap, const void *va, const void *vb)
{
    FIFO fifo = extrap;
    const long *ao = va;
    const long *bo = vb;
    const struct fifo_entry *a = toku_fifo_get_entry(fifo, *ao);
    const struct fifo_entry *b = toku_fifo_get_entry(fifo, *bo);
    if (a->msn.msn > b->msn.msn) {
        return +1;
    }
    if (a->msn.msn < b->msn.msn) {
        return -1;
    }
    return 0;
}

/**
 * Given a fifo_entry, either decompose it into its parameters and call
 * toku_ft_bn_apply_cmd, or discard it, based on its MSN and the MSN of the
 * basement node.
 */
static void
do_bn_apply_cmd(FT_HANDLE t, BASEMENTNODE bn, FTNODE ancestor, int childnum, const struct fifo_entry *entry, STAT64INFO stats_to_update)
{
    // The messages are being iterated over in (key,msn) order or just in
    // msn order, so all the messages for one key, from one buffer, are in
    // ascending msn order.  So it's ok that we don't update the basement
    // node's msn until the end.
    if (entry->msn.msn > bn->max_msn_applied.msn) {
        ITEMLEN keylen = entry->keylen;
        ITEMLEN vallen = entry->vallen;
        enum ft_msg_type type = fifo_entry_get_msg_type(entry);
        MSN msn = entry->msn;
        const XIDS xids = (XIDS) &entry->xids_s;
        bytevec key = xids_get_end_of_array(xids);
        bytevec val = (u_int8_t*)key + entry->keylen;

        DBT hk;
        toku_fill_dbt(&hk, key, keylen);
        DBT hv;
        FT_MSG_S ftcmd = { type, msn, xids, .u.id = { &hk, toku_fill_dbt(&hv, val, vallen) } };
        toku_ft_bn_apply_cmd(
            t->ft->compare_fun,
            t->ft->update_fun,
            &t->ft->cmp_descriptor,
            bn,
            &ftcmd,
            &BP_WORKDONE(ancestor, childnum),
            stats_to_update
            );
    } else {
        STATUS_VALUE(FT_MSN_DISCARDS)++;
    }
}

struct iterate_do_bn_apply_cmd_extra {
    FT_HANDLE t;
    BASEMENTNODE bn;
    FTNODE ancestor;
    int childnum;
    STAT64INFO stats_to_update;
};

static int
iterate_do_bn_apply_cmd(OMTVALUE v, u_int32_t UU(idx), void *extrap)
{
    struct iterate_do_bn_apply_cmd_extra *e = extrap;
    const long offset = (long) v;
    NONLEAF_CHILDINFO bnc = BNC(e->ancestor, e->childnum);
    const struct fifo_entry *entry = toku_fifo_get_entry(bnc->buffer, offset);
    do_bn_apply_cmd(e->t, e->bn, e->ancestor, e->childnum, entry, e->stats_to_update);
    return 0;
}

/**
 * Given the bounds of the basement node to which we will apply messages,
 * find the indexes within message_tree which contain the range of
 * relevant messages.
 *
 * The message tree contains offsets into the buffer, where messages are
 * found.  The pivot_bounds are the lower bound exclusive and upper bound
 * inclusive, because they come from pivot keys in the tree.  We want OMT
 * indices, which must have the lower bound be inclusive and the upper
 * bound exclusive.  We will get these by telling toku_omt_find to look
 * for something strictly bigger than each of our pivot bounds.
 *
 * Outputs the OMT indices in lbi (lower bound inclusive) and ube (upper
 * bound exclusive).
 */
static void
find_bounds_within_message_tree(
    DESCRIPTOR desc,       /// used for cmp
    ft_compare_func cmp,  /// used to compare keys
    OMT message_tree,      /// tree holding FIFO offsets, in which we want to look for indices
    FIFO buffer,           /// buffer in which messages are found
    struct pivot_bounds const * const bounds,  /// key bounds within the basement node we're applying messages to
    u_int32_t *lbi,        /// (output) "lower bound inclusive" (index into message_tree)
    u_int32_t *ube         /// (output) "upper bound exclusive" (index into message_tree)
    )
{
    int r = 0;

    if (bounds->lower_bound_exclusive) {
        // By setting msn to MAX_MSN and by using direction of +1, we will
        // get the first message greater than (in (key, msn) order) any
        // message (with any msn) with the key lower_bound_exclusive.
        // This will be a message we want to try applying, so it is the
        // "lower bound inclusive" within the message_tree.
        struct toku_fifo_entry_key_msn_heaviside_extra lbi_extra = {
            .desc = desc, .cmp = cmp,
            .fifo = buffer,
            .key = bounds->lower_bound_exclusive,
            .msn = MAX_MSN };
        OMTVALUE found_lb;
        r = toku_omt_find(message_tree, toku_fifo_entry_key_msn_heaviside,
                          &lbi_extra, +1, &found_lb, lbi);
        if (r == DB_NOTFOUND) {
            // There is no relevant data (the lower bound is bigger than
            // any message in this tree), so we have no range and we're
            // done.
            *lbi = 0;
            *ube = 0;
            return;
        }
        if (bounds->upper_bound_inclusive) {
            // Check if what we found for lbi is greater than the upper
            // bound inclusive that we have.  If so, there are no relevant
            // messages between these bounds.
            const DBT *ubi = bounds->upper_bound_inclusive;
            const long offset = (long) found_lb;
            DBT found_lbidbt;
            fill_dbt_for_fifo_entry(&found_lbidbt, toku_fifo_get_entry(buffer, offset));
            FAKE_DB(db, desc);
            int c = cmp(&db, &found_lbidbt, ubi);
            // These DBTs really are both inclusive bounds, so we need
            // strict inequality in order to determine that there's
            // nothing between them.  If they're equal, then we actually
            // need to apply the message pointed to by lbi, and also
            // anything with the same key but a bigger msn.
            if (c > 0) {
                *lbi = 0;
                *ube = 0;
                return;
            }
        }
    } else {
        // No lower bound given, it's negative infinity, so we start at
        // the first message in the OMT.
        *lbi = 0;
    }
    if (bounds->upper_bound_inclusive) {
        // Again, we use an msn of MAX_MSN and a direction of +1 to get
        // the first thing bigger than the upper_bound_inclusive key.
        // This is therefore the smallest thing we don't want to apply,
        // and toku_omt_iterate_on_range will not examine it.
        struct toku_fifo_entry_key_msn_heaviside_extra ube_extra = {
            .desc = desc, .cmp = cmp,
            .fifo = buffer,
            .key = bounds->upper_bound_inclusive,
            .msn = MAX_MSN };
        r = toku_omt_find(message_tree, toku_fifo_entry_key_msn_heaviside,
                          &ube_extra, +1, NULL, ube);
        if (r == DB_NOTFOUND) {
            // Couldn't find anything in the buffer bigger than our key,
            // so we need to look at everything up to the end of
            // message_tree.
            *ube = toku_omt_size(message_tree);
        }
    } else {
        // No upper bound given, it's positive infinity, so we need to go
        // through the end of the OMT.
        *ube = toku_omt_size(message_tree);
    }
}

/**
 * For each message in the ancestor's buffer (determined by childnum) that
 * is key-wise between lower_bound_exclusive and upper_bound_inclusive,
 * apply the message to the basement node.  We treat the bounds as minus
 * or plus infinity respectively if they are NULL.  Do not mark the node
 * as dirty (preserve previous state of 'dirty' bit).
 */
static int
bnc_apply_messages_to_basement_node(
    FT_HANDLE t,             // used for comparison function
    BASEMENTNODE bn,   // where to apply messages
    FTNODE ancestor,  // the ancestor node where we can find messages to apply
    int childnum,      // which child buffer of ancestor contains messages we want
    struct pivot_bounds const * const bounds,  // contains pivot key bounds of this basement node
    BOOL* msgs_applied
    )
{
    int r;
    NONLEAF_CHILDINFO bnc = BNC(ancestor, childnum);

    // Determine the offsets in the message trees between which we need to
    // apply messages from this buffer
    STAT64INFO_S stats_delta = {0,0};
    u_int32_t stale_lbi, stale_ube;
    if (!bn->stale_ancestor_messages_applied) {
        find_bounds_within_message_tree(&t->ft->cmp_descriptor, t->ft->compare_fun, bnc->stale_message_tree, bnc->buffer, bounds, &stale_lbi, &stale_ube);
    } else {
        stale_lbi = 0;
        stale_ube = 0;
    }
    u_int32_t fresh_lbi, fresh_ube;
    find_bounds_within_message_tree(&t->ft->cmp_descriptor, t->ft->compare_fun, bnc->fresh_message_tree, bnc->buffer, bounds, &fresh_lbi, &fresh_ube);

    // We now know where all the messages we must apply are, so one of the
    // following 4 cases will do the application, depending on which of
    // the lists contains relevant messages:
    //
    // 1. broadcast messages and anything else
    // 2. only fresh messages
    // 3. only stale messages
    // 4. fresh and stale messages but no broadcasts
    if (toku_omt_size(bnc->broadcast_list) > 0) {
        // We have some broadcasts, which don't have keys, so we grab all
        // the relevant messages' offsets and sort them by MSN, then apply
        // them in MSN order.
        const int buffer_size = ((stale_ube - stale_lbi) + (fresh_ube - fresh_lbi) + toku_omt_size(bnc->broadcast_list));
        long *XMALLOC_N(buffer_size, offsets);
        struct store_fifo_offset_extra sfo_extra = { .offsets = offsets, .i = 0 };

        // Populate offsets array with offsets to stale messages
        r = toku_omt_iterate_on_range(bnc->stale_message_tree, stale_lbi, stale_ube, store_fifo_offset, &sfo_extra);
        assert_zero(r);

        // Then store fresh offsets
        r = toku_omt_iterate_on_range(bnc->fresh_message_tree, fresh_lbi, fresh_ube, store_fifo_offset, &sfo_extra);
        assert_zero(r);

        // Store offsets of all broadcast messages.
        r = toku_omt_iterate(bnc->broadcast_list, store_fifo_offset, &sfo_extra);
        assert_zero(r);
        invariant(sfo_extra.i == buffer_size);

        // Sort by MSN.
        r = mergesort_r(offsets, buffer_size, sizeof offsets[0], bnc->buffer, fifo_offset_msn_cmp);
        assert_zero(r);

        // Apply the messages in MSN order.
        for (int i = 0; i < buffer_size; ++i) {
            *msgs_applied = TRUE;
            const struct fifo_entry *entry = toku_fifo_get_entry(bnc->buffer, offsets[i]);
            do_bn_apply_cmd(t, bn, ancestor, childnum, entry, &stats_delta);
        }

        toku_free(offsets);
    } else if (stale_lbi == stale_ube) {
        // No stale messages to apply, we just apply fresh messages.
        struct iterate_do_bn_apply_cmd_extra iter_extra = { .t = t, .bn = bn, .ancestor = ancestor, .childnum = childnum, .stats_to_update = &stats_delta};
        if (fresh_ube - fresh_lbi > 0) *msgs_applied = TRUE;
        r = toku_omt_iterate_on_range(bnc->fresh_message_tree, fresh_lbi, fresh_ube, iterate_do_bn_apply_cmd, &iter_extra);
        assert_zero(r);
    } else if (fresh_lbi == fresh_ube) {
        // No fresh messages to apply, we just apply stale messages.

        if (stale_ube - stale_lbi > 0) *msgs_applied = TRUE;
        struct iterate_do_bn_apply_cmd_extra iter_extra = { .t = t, .bn = bn, .ancestor = ancestor, .childnum = childnum , .stats_to_update = &stats_delta};

        r = toku_omt_iterate_on_range(bnc->stale_message_tree, stale_lbi, stale_ube, iterate_do_bn_apply_cmd, &iter_extra);
        assert_zero(r);
    } else {
        // We have stale and fresh messages but no broadcasts.  We can
        // iterate over both OMTs together.

        // For the loop, we'll keep the indices into both the fresh and
        // stale trees, and also the OMTVALUE at those indices.
        u_int32_t stale_i = stale_lbi, fresh_i = fresh_lbi;
        OMTVALUE stale_v, fresh_v;
        r = toku_omt_fetch(bnc->stale_message_tree, stale_i, &stale_v);
        assert_zero(r);
        r = toku_omt_fetch(bnc->fresh_message_tree, fresh_i, &fresh_v);
        assert_zero(r);

        // This comparison extra struct won't change during iteration.
        struct toku_fifo_entry_key_msn_cmp_extra extra = { .desc= &t->ft->cmp_descriptor, .cmp = t->ft->compare_fun, .fifo = bnc->buffer };

        // Iterate over both lists, applying the smaller (in (key, msn)
        // order) message at each step
        while (stale_i < stale_ube && fresh_i < fresh_ube) {
            *msgs_applied = TRUE;
            const long stale_offset = (long) stale_v;
            const long fresh_offset = (long) fresh_v;
            int c = toku_fifo_entry_key_msn_cmp(&extra, &stale_offset, &fresh_offset);
            if (c < 0) {
                // The stale message we're pointing to either has a
                // smaller key than the fresh message, or has the same key
                // but a smaller MSN.  We'll apply it, then get the next
                // stale message into stale_i and stale_v.
                const struct fifo_entry *stale_entry = toku_fifo_get_entry(bnc->buffer, stale_offset);
                do_bn_apply_cmd(t, bn, ancestor, childnum, stale_entry, &stats_delta);
                stale_i++;
                if (stale_i != stale_ube) {
                    invariant(stale_i < stale_ube);
                    r = toku_omt_fetch(bnc->stale_message_tree, stale_i, &stale_v);
                    assert_zero(r);
                }
            } else if (c > 0) {
                // The fresh message we're pointing to either has a
                // smaller key than the stale message, or has the same key
                // but a smaller MSN.  We'll apply it, then get the next
                // fresh message into fresh_i and fresh_v.
                const struct fifo_entry *fresh_entry = toku_fifo_get_entry(bnc->buffer, fresh_offset);
                do_bn_apply_cmd(t, bn, ancestor, childnum, fresh_entry, &stats_delta);
                fresh_i++;
                if (fresh_i != fresh_ube) {
                    invariant(fresh_i < fresh_ube);
                    r = toku_omt_fetch(bnc->fresh_message_tree, fresh_i, &fresh_v);
                    assert_zero(r);
                }
            } else {
                // We have found the same MSN in both trees.  This means a
                // single message showing up in both trees.  This should
                // not happen.
                assert(false);
            }
        }

        // Apply the rest of the stale messages, if any exist
        while (stale_i < stale_ube) {
            const long stale_offset = (long) stale_v;
            const struct fifo_entry *stale_entry = toku_fifo_get_entry(bnc->buffer, stale_offset);
            do_bn_apply_cmd(t, bn, ancestor, childnum, stale_entry, &stats_delta);
            stale_i++;
            if (stale_i != stale_ube) {
                r = toku_omt_fetch(bnc->stale_message_tree, stale_i, &stale_v);
                assert_zero(r);
            }
        }

        // Apply the rest of the fresh messages, if any exist
        while (fresh_i < fresh_ube) {
            const long fresh_offset = (long) fresh_v;
            const struct fifo_entry *fresh_entry = toku_fifo_get_entry(bnc->buffer, fresh_offset);
            do_bn_apply_cmd(t, bn, ancestor, childnum, fresh_entry, &stats_delta);
            fresh_i++;
            if (fresh_i != fresh_ube) {
                r = toku_omt_fetch(bnc->fresh_message_tree, fresh_i, &fresh_v);
                assert_zero(r);
            }
        }
    }
    //
    // update stats
    //
    if (stats_delta.numbytes || stats_delta.numrows) {
        toku_ft_update_stats(&t->ft->in_memory_stats, stats_delta);
    }
    // We can't delete things out of the fresh tree inside the above
    // procedures because we're still looking at the fresh tree.  Instead
    // we have to move messages after we're done looking at it.
    struct copy_to_stale_extra cts_extra = { .ft_handle = t, .bnc = bnc };
    r = toku_omt_iterate_on_range(bnc->fresh_message_tree, fresh_lbi, fresh_ube, copy_to_stale, &cts_extra);
    assert_zero(r);
    for (u_int32_t ube = fresh_ube; fresh_lbi < ube; --ube) {
        // When we delete the message at the fresh_lbi index, everything
        // to the right moves down one spot, including the offset at ube.
        r = toku_omt_delete_at(bnc->fresh_message_tree, fresh_lbi);
        assert_zero(r);
    }
    return r;
}

void
maybe_apply_ancestors_messages_to_node (FT_HANDLE t, FTNODE node, ANCESTORS ancestors, struct pivot_bounds const * const bounds, BOOL* msgs_applied)
// Effect:
//   Bring a leaf node up-to-date according to all the messages in the ancestors.
//   If the leaf node is already up-to-date then do nothing.
//   If the leaf node is not already up-to-date, then record the work done
//   for that leaf in each ancestor.
// Requires:
//   This is being called when pinning a leaf node for the query path.
//   The entire root-to-leaf path is pinned and appears in the ancestors list.
{
    VERIFY_NODE(t, node);
    if (node->height > 0) { goto exit; }
    // know we are a leaf node
    // An important invariant:
    // We MUST bring every available basement node up to date.
    // flushing on the cleaner thread depends on this. This invariant
    // allows the cleaner thread to just pick an internal node and flush it
    // as opposed to being forced to start from the root.
    for (int i = 0; i < node->n_children; i++) {
        if (BP_STATE(node, i) != PT_AVAIL) { continue; }
        BASEMENTNODE curr_bn = BLB(node, i);
        struct pivot_bounds curr_bounds = next_pivot_keys(node, i, bounds);
        for (ANCESTORS curr_ancestors = ancestors; curr_ancestors; curr_ancestors = curr_ancestors->next) {
            if (curr_ancestors->node->max_msn_applied_to_node_on_disk.msn > curr_bn->max_msn_applied.msn) {
                assert(BP_STATE(curr_ancestors->node, curr_ancestors->childnum) == PT_AVAIL);
                bnc_apply_messages_to_basement_node(
                    t,
                    curr_bn,
                    curr_ancestors->node,
                    curr_ancestors->childnum,
                    &curr_bounds,
                    msgs_applied
                    );
                // We don't want to check this ancestor node again if the
                // next time we query it, the msn hasn't changed.
                curr_bn->max_msn_applied = curr_ancestors->node->max_msn_applied_to_node_on_disk;
            }
        }
        // At this point, we know all the stale messages above this
        // basement node have been applied, and any new messages will be
        // fresh, so we don't need to look at stale messages for this
        // basement node, unless it gets evicted (and this field becomes
        // false when it's read in again).
        curr_bn->stale_ancestor_messages_applied = true;
    }
exit:
    VERIFY_NODE(t, node);
}

static int
ft_cursor_shortcut (
    FT_CURSOR cursor,
    int direction,
    FT_GET_CALLBACK_FUNCTION getf,
    void *getf_v,
    u_int32_t *keylen,
    void **key,
    u_int32_t *vallen,
    void **val
    );

// This is a bottom layer of the search functions.
static int
ft_search_basement_node(
    BASEMENTNODE bn,
    ft_search_t *search,
    FT_GET_CALLBACK_FUNCTION getf,
    void *getf_v,
    BOOL *doprefetch,
    FT_CURSOR ftcursor,
    BOOL can_bulk_fetch
    )
{
    // Now we have to convert from ft_search_t to the heaviside function with a direction.  What a pain...

    int direction;
    switch (search->direction) {
    case FT_SEARCH_LEFT:   direction = +1; goto ok;
    case FT_SEARCH_RIGHT:  direction = -1; goto ok;
    }
    return EINVAL;  // This return and the goto are a hack to get both compile-time and run-time checking on enum
ok: ;
    OMTVALUE datav;
    u_int32_t idx = 0;
    int r = toku_omt_find(bn->buffer,
                          heaviside_from_search_t,
                          search,
                          direction,
                          &datav, &idx);
    if (r!=0) return r;

    LEAFENTRY le = datav;
    if (toku_ft_cursor_is_leaf_mode(ftcursor))
        goto got_a_good_value;        // leaf mode cursors see all leaf entries
    if (is_le_val_del(le,ftcursor)) {
        // Provisionally deleted stuff is gone.
        // So we need to scan in the direction to see if we can find something
        while (1) {
            switch (search->direction) {
            case FT_SEARCH_LEFT:
                idx++;
                if (idx >= toku_omt_size(bn->buffer))
                    return DB_NOTFOUND;
                break;
            case FT_SEARCH_RIGHT:
                if (idx == 0)
                    return DB_NOTFOUND;
                idx--;
                break;
            default:
                assert(FALSE);
            }
            r = toku_omt_fetch(bn->buffer, idx, &datav);
            assert_zero(r); // we just validated the index
            le = datav;
            if (!is_le_val_del(le,ftcursor)) goto got_a_good_value;
        }
    }
got_a_good_value:
    {
        u_int32_t keylen;
        void *key;
        u_int32_t vallen;
        void *val;

        ft_cursor_extract_key_and_val(le,
                                       ftcursor,
                                       &keylen,
                                       &key,
                                       &vallen,
                                       &val
            );

        r = getf(keylen, key, vallen, val, getf_v, false);
        if (r==0 || r == TOKUDB_CURSOR_CONTINUE) {
            ftcursor->leaf_info.to_be.omt   = bn->buffer;
            ftcursor->leaf_info.to_be.index = idx;

            if (r == TOKUDB_CURSOR_CONTINUE && can_bulk_fetch) {
                r = ft_cursor_shortcut(
                    ftcursor,
                    direction,
                    getf,
                    getf_v,
                    &keylen,
                    &key,
                    &vallen,
                    &val
                    );
            }

            ft_cursor_cleanup_dbts(ftcursor);
            if (!ftcursor->is_temporary) {
                ftcursor->key.data = toku_memdup(key, keylen);
                ftcursor->val.data = toku_memdup(val, vallen);
                ftcursor->key.size = keylen;
                ftcursor->val.size = vallen;
            }
            //The search was successful.  Prefetching can continue.
            *doprefetch = TRUE;
        }
    }
    if (r == TOKUDB_CURSOR_CONTINUE) r = 0;
    return r;
}

static int
ft_search_node (
    FT_HANDLE brt,
    FTNODE node,
    ft_search_t *search,
    int child_to_search,
    FT_GET_CALLBACK_FUNCTION getf,
    void *getf_v,
    BOOL *doprefetch,
    FT_CURSOR ftcursor,
    UNLOCKERS unlockers,
    ANCESTORS,
    struct pivot_bounds const * const bounds,
    BOOL can_bulk_fetch
    );

// the number of nodes to prefetch
#define TOKU_DO_PREFETCH 1
#if TOKU_DO_PREFETCH

static int
ftnode_fetch_callback_and_free_bfe(CACHEFILE cf, int fd, BLOCKNUM nodename, u_int32_t fullhash, void **ftnode_pv, void** UU(disk_data), PAIR_ATTR *sizep, int *dirtyp, void *extraargs)
{
    int r = toku_ftnode_fetch_callback(cf, fd, nodename, fullhash, ftnode_pv, disk_data, sizep, dirtyp, extraargs);
    destroy_bfe_for_prefetch(extraargs);
    toku_free(extraargs);
    return r;
}

static int
ftnode_pf_callback_and_free_bfe(void *ftnode_pv, void* disk_data, void *read_extraargs, int fd, PAIR_ATTR *sizep)
{
    int r = toku_ftnode_pf_callback(ftnode_pv, disk_data, read_extraargs, fd, sizep);
    destroy_bfe_for_prefetch(read_extraargs);
    toku_free(read_extraargs);
    return r;
}

static void
ft_node_maybe_prefetch(FT_HANDLE brt, FTNODE node, int childnum, FT_CURSOR ftcursor, BOOL *doprefetch) {

    // if we want to prefetch in the tree
    // then prefetch the next children if there are any
    if (*doprefetch && ft_cursor_prefetching(ftcursor) && !ftcursor->disable_prefetching) {
        int rc = ft_cursor_rightmost_child_wanted(ftcursor, brt, node);
        for (int i = childnum + 1; (i <= childnum + TOKU_DO_PREFETCH) && (i <= rc); i++) {
            BLOCKNUM nextchildblocknum = BP_BLOCKNUM(node, i);
            u_int32_t nextfullhash = compute_child_fullhash(brt->ft->cf, node, i);
            struct ftnode_fetch_extra *MALLOC(bfe);
            fill_bfe_for_prefetch(bfe, brt->ft, ftcursor);
            BOOL doing_prefetch = FALSE;
            toku_cachefile_prefetch(
                brt->ft->cf,
                nextchildblocknum,
                nextfullhash,
                get_write_callbacks_for_node(brt->ft),
                ftnode_fetch_callback_and_free_bfe,
                toku_ftnode_pf_req_callback,
                ftnode_pf_callback_and_free_bfe,
                bfe,
                &doing_prefetch
                );
            if (!doing_prefetch) {
                destroy_bfe_for_prefetch(bfe);
                toku_free(bfe);
            }
            *doprefetch = FALSE;
        }
    }
}

#endif

struct unlock_ftnode_extra {
    FT_HANDLE ft_handle;
    FTNODE node;
    BOOL msgs_applied;
};
// When this is called, the cachetable lock is held
static void
unlock_ftnode_fun (void *v) {
    struct unlock_ftnode_extra *x = v;
    FT_HANDLE brt = x->ft_handle;
    FTNODE node = x->node;
    // CT lock is held
    int r = toku_cachetable_unpin_ct_prelocked_no_flush(
        brt->ft->cf,
        node->thisnodename,
        node->fullhash,
        (enum cachetable_dirty) node->dirty,
        x->msgs_applied ? make_ftnode_pair_attr(node) : make_invalid_pair_attr()
        );
    assert(r==0);
}

/* search in a node's child */
static int
ft_search_child(FT_HANDLE brt, FTNODE node, int childnum, ft_search_t *search, FT_GET_CALLBACK_FUNCTION getf, void *getf_v, BOOL *doprefetch, FT_CURSOR ftcursor, UNLOCKERS unlockers,
                 ANCESTORS ancestors, struct pivot_bounds const * const bounds, BOOL can_bulk_fetch)
// Effect: Search in a node's child.  Searches are read-only now (at least as far as the hardcopy is concerned).
{
    struct ancestors next_ancestors = {node, childnum, ancestors};

    BLOCKNUM childblocknum = BP_BLOCKNUM(node,childnum);
    u_int32_t fullhash = compute_child_fullhash(brt->ft->cf, node, childnum);
    FTNODE childnode;

    struct ftnode_fetch_extra bfe;
    fill_bfe_for_subset_read(
        &bfe,
        brt->ft,
        search,
        &ftcursor->range_lock_left_key,
        &ftcursor->range_lock_right_key,
        ftcursor->left_is_neg_infty,
        ftcursor->right_is_pos_infty,
        ftcursor->disable_prefetching
        );
    BOOL msgs_applied = FALSE;
    {
        int rr = toku_pin_ftnode(brt, childblocknum, fullhash,
                                  unlockers,
                                  &next_ancestors, bounds,
                                  &bfe,
                                  (node->height == 1), // may_modify_node TRUE iff child is leaf
                                  TRUE,
                                  &childnode,
                                  &msgs_applied);
        if (rr==TOKUDB_TRY_AGAIN) return rr;
        assert(rr==0);
    }

    struct unlock_ftnode_extra unlock_extra   = {brt,childnode,msgs_applied};
    struct unlockers next_unlockers = {TRUE, unlock_ftnode_fun, (void*)&unlock_extra, unlockers};

    int r = ft_search_node(brt, childnode, search, bfe.child_to_read, getf, getf_v, doprefetch, ftcursor, &next_unlockers, &next_ancestors, bounds, can_bulk_fetch);
    if (r!=TOKUDB_TRY_AGAIN) {
#if TOKU_DO_PREFETCH
        // maybe prefetch the next child
        if (r == 0 && node->height == 1) {
            ft_node_maybe_prefetch(brt, node, childnum, ftcursor, doprefetch);
        }
#endif

        assert(next_unlockers.locked);
        if (msgs_applied) {
            toku_unpin_ftnode(brt->ft, childnode);
        }
        else {
            toku_unpin_ftnode_read_only(brt, childnode);
        }
    } else {
        // try again.

        // there are two cases where we get TOKUDB_TRY_AGAIN
        //  case 1 is when some later call to toku_pin_ftnode returned
        //  that value and unpinned all the nodes anyway. case 2
        //  is when ft_search_node had to stop its search because
        //  some piece of a node that it needed was not in memory. In this case,
        //  the node was not unpinned, so we unpin it here
        if (next_unlockers.locked) {
            if (msgs_applied) {
                toku_unpin_ftnode(brt->ft, childnode);
            }
            else {
                toku_unpin_ftnode_read_only(brt, childnode);
            }
        }
    }

    return r;
}

static inline int
search_which_child_cmp_with_bound(DB *db, ft_compare_func cmp, FTNODE node, int childnum, ft_search_t *search, DBT *dbt)
{
    return cmp(db, toku_copyref_dbt(dbt, node->childkeys[childnum]), &search->pivot_bound);
}

int
toku_ft_search_which_child(
    DESCRIPTOR desc,
    ft_compare_func cmp,
    FTNODE node,
    ft_search_t *search
    )
{
#define DO_SEARCH_WHICH_CHILD_BINARY 1
#if DO_SEARCH_WHICH_CHILD_BINARY
    if (node->n_children <= 1) return 0;

    DBT pivotkey;
    toku_init_dbt(&pivotkey);
    int lo = 0;
    int hi = node->n_children - 1;
    int mi;
    while (lo < hi) {
        mi = (lo + hi) / 2;
        toku_copyref_dbt(&pivotkey, node->childkeys[mi]);
        // search->compare is really strange, and only works well with a
        // linear search, it makes binary search a pita.
        //
        // if you are searching left to right, it returns
        //   "0" for pivots that are < the target, and
        //   "1" for pivots that are >= the target
        // if you are searching right to left, it's the opposite.
        //
        // so if we're searching from the left and search->compare says
        // "1", we want to go left from here, if it says "0" we want to go
        // right.  searching from the right does the opposite.
        bool c = search->compare(search, &pivotkey);
        if (((search->direction == FT_SEARCH_LEFT) && c) ||
            ((search->direction == FT_SEARCH_RIGHT) && !c)) {
            hi = mi;
        } else {
            assert(((search->direction == FT_SEARCH_LEFT) && !c) ||
                   ((search->direction == FT_SEARCH_RIGHT) && c));
            lo = mi + 1;
        }
    }
    // ready to return something, if the pivot is bounded, we have to move
    // over a bit to get away from what we've already searched
    if (search->have_pivot_bound) {
        FAKE_DB(db, desc);
        if (search->direction == FT_SEARCH_LEFT) {
            while (lo < node->n_children - 1 &&
                   search_which_child_cmp_with_bound(&db, cmp, node, lo, search, &pivotkey) <= 0) {
                // searching left to right, if the comparison says the
                // current pivot (lo) is left of or equal to our bound,
                // don't search that child again
                lo++;
            }
        } else {
            while (lo > 0 &&
                   search_which_child_cmp_with_bound(&db, cmp, node, lo - 1, search, &pivotkey) >= 0) {
                // searching right to left, same argument as just above
                // (but we had to pass lo - 1 because the pivot between lo
                // and the thing just less than it is at that position in
                // the childkeys array)
                lo--;
            }
        }
    }
    return lo;
#endif
#define DO_SEARCH_WHICH_CHILD_LINEAR 0
#if DO_SEARCH_WHICH_CHILD_LINEAR
    int c;
    DBT pivotkey;
    toku_init_dbt(&pivotkey);

    /* binary search is overkill for a small array */
    int child[node->n_children];

    /* scan left to right or right to left depending on the search direction */
    for (c = 0; c < node->n_children; c++) {
        child[c] = (search->direction == FT_SEARCH_LEFT) ? c : node->n_children - 1 - c;
    }
    for (c = 0; c < node->n_children-1; c++) {
        int p = (search->direction == FT_SEARCH_LEFT) ? child[c] : child[c] - 1;
        toku_copyref_dbt(&pivotkey, node->childkeys[p]);
        if (search_pivot_is_bounded(search, desc, cmp, &pivotkey) && search->compare(search, &pivotkey)) {
            return child[c];
        }
    }
    /* check the first (left) or last (right) node if nothing has been found */
    return child[c];
#endif
}

static void
maybe_search_save_bound(
    FTNODE node,
    int child_searched,
    ft_search_t *search)
{
    int p = (search->direction == FT_SEARCH_LEFT) ? child_searched : child_searched - 1;
    if (p >= 0 && p < node->n_children-1) {
        search_save_bound(search, &node->childkeys[p]);
    }
}

static int
ft_search_node(
    FT_HANDLE brt,
    FTNODE node,
    ft_search_t *search,
    int child_to_search,
    FT_GET_CALLBACK_FUNCTION getf,
    void *getf_v,
    BOOL *doprefetch,
    FT_CURSOR ftcursor,
    UNLOCKERS unlockers,
    ANCESTORS ancestors,
    struct pivot_bounds const * const bounds,
    BOOL can_bulk_fetch
    )
{   int r = 0;
    // assert that we got a valid child_to_search
    assert(child_to_search >= 0 && child_to_search < node->n_children);
    //
    // At this point, we must have the necessary partition available to continue the search
    //
    assert(BP_STATE(node,child_to_search) == PT_AVAIL);
    while (child_to_search >= 0 && child_to_search < node->n_children) {
        //
        // Normally, the child we want to use is available, as we checked
        // before entering this while loop. However, if we pass through
        // the loop once, getting DB_NOTFOUND for this first value
        // of child_to_search, we enter the while loop again with a
        // child_to_search that may not be in memory. If it is not,
        // we need to return TOKUDB_TRY_AGAIN so the query can
        // read the appropriate partition into memory
        //
        if (BP_STATE(node,child_to_search) != PT_AVAIL) {
            return TOKUDB_TRY_AGAIN;
        }
        const struct pivot_bounds next_bounds = next_pivot_keys(node, child_to_search, bounds);
        if (node->height > 0) {
            r = ft_search_child(
                brt,
                node,
                child_to_search,
                search,
                getf,
                getf_v,
                doprefetch,
                ftcursor,
                unlockers,
                ancestors,
                &next_bounds,
                can_bulk_fetch
                );
        }
        else {
            r = ft_search_basement_node(
                BLB(node, child_to_search),
                search,
                getf,
                getf_v,
                doprefetch,
                ftcursor,
                can_bulk_fetch
                );
        }
        if (r == 0) return r; //Success

        if (r != DB_NOTFOUND) {
            return r; //Error (or message to quit early, such as TOKUDB_FOUND_BUT_REJECTED or TOKUDB_TRY_AGAIN)
        }
        // we have a new pivotkey
        else {
            if (node->height == 0) {
                // when we run off the end of a basement, try to lock the range up to the pivot. solves #3529
                const DBT *pivot = NULL;
                if (search->direction == FT_SEARCH_LEFT)
                    pivot = next_bounds.upper_bound_inclusive; // left -> right
                else
                    pivot = next_bounds.lower_bound_exclusive; // right -> left
                if (pivot) {
                    int rr = getf(pivot->size, pivot->data, 0, NULL, getf_v, true);
                    if (rr != 0)
                        return rr; // lock was not granted
                }
            }

            // If we got a DB_NOTFOUND then we have to search the next record.        Possibly everything present is not visible.
            // This way of doing DB_NOTFOUND is a kludge, and ought to be simplified.  Something like this is needed for DB_NEXT, but
            //        for point queries, it's overkill.  If we got a DB_NOTFOUND on a point query then we should just stop looking.
            // When releasing locks on I/O we must not search the same subtree again, or we won't be guaranteed to make forward progress.
            // If we got a DB_NOTFOUND, then the pivot is too small if searching from left to right (too large if searching from right to left).
            // So save the pivot key in the search object.
            maybe_search_save_bound(node, child_to_search, search);
        }
        // not really necessary, just put this here so that reading the
        // code becomes simpler. The point is at this point in the code,
        // we know that we got DB_NOTFOUND and we have to continue
        assert(r == DB_NOTFOUND);
        if (search->direction == FT_SEARCH_LEFT) {
            child_to_search++;
        }
        else {
            child_to_search--;
        }
    }
    return r;
}

static int
toku_ft_search (FT_HANDLE brt, ft_search_t *search, FT_GET_CALLBACK_FUNCTION getf, void *getf_v, FT_CURSOR ftcursor, BOOL can_bulk_fetch)
// Effect: Perform a search.  Associate cursor with a leaf if possible.
// All searches are performed through this function.
{
    int r;
    uint trycount = 0;     // How many tries did it take to get the result?

try_again:

    trycount++;
    assert(brt->ft);

    //
    // Here is how searches work
    // At a high level, we descend down the tree, using the search parameter
    // to guide us towards where to look. But the search parameter is not
    // used here to determine which child of a node to read (regardless
    // of whether that child is another node or a basement node)
    // The search parameter is used while we are pinning the node into
    // memory, because that is when the system needs to ensure that
    // the appropriate partition of the child we are using is in memory.
    // So, here are the steps for a search (and this applies to this function
    // as well as ft_search_child:
    //  - Take the search parameter, and create a ftnode_fetch_extra, that will be used by toku_pin_ftnode(_holding_lock)
    //  - Call toku_pin_ftnode(_holding_lock) with the bfe as the extra for the fetch callback (in case the node is not at all in memory)
    //       and the partial fetch callback (in case the node is perhaps partially in memory) to the fetch the node
    //  - This eventually calls either toku_ftnode_fetch_callback or  toku_ftnode_pf_req_callback depending on whether the node is in
    //     memory at all or not.
    //  - Within these functions, the "ft_search_t search" parameter is used to evaluate which child the search is interested in.
    //     If the node is not in memory at all, toku_ftnode_fetch_callback will read the node and decompress only the partition for the
    //     relevant child, be it a message buffer or basement node. If the node is in memory, then toku_ftnode_pf_req_callback
    //     will tell the cachetable that a partial fetch is required if and only if the relevant child is not in memory. If the relevant child
    //     is not in memory, then toku_ftnode_pf_callback is called to fetch the partition.
    //  - These functions set bfe->child_to_read so that the search code does not need to reevaluate it.
    //  - Just to reiterate, all of the last item happens within toku_ftnode_pin(_holding_lock)
    //  - At this point, toku_ftnode_pin_holding_lock has returned, with bfe.child_to_read set,
    //  - ft_search_node is called, assuming that the node and its relevant partition are in memory.
    //
    struct ftnode_fetch_extra bfe;
    fill_bfe_for_subset_read(
        &bfe,
        brt->ft,
        search,
        &ftcursor->range_lock_left_key,
        &ftcursor->range_lock_right_key,
        ftcursor->left_is_neg_infty,
        ftcursor->right_is_pos_infty,
        ftcursor->disable_prefetching
        );
    FTNODE node = NULL;
    {
        toku_ft_grab_treelock(brt->ft);
        u_int32_t fullhash;
        CACHEKEY root_key;
        toku_calculate_root_offset_pointer(brt->ft, &root_key, &fullhash);
        toku_pin_ftnode_off_client_thread(
            brt->ft,
            root_key,
            fullhash,
            &bfe,
            FALSE, // may_modify_node set to FALSE, because root cannot change during search
            0,
            NULL,
            &node
            );
        toku_ft_release_treelock(brt->ft);
    }

    uint tree_height = node->height + 1;  // How high is the tree?  This is the height of the root node plus one (leaf is at height 0).


    struct unlock_ftnode_extra unlock_extra   = {brt,node,FALSE};
    struct unlockers                unlockers      = {TRUE, unlock_ftnode_fun, (void*)&unlock_extra, (UNLOCKERS)NULL};

    {
        BOOL doprefetch = FALSE;
        //static int counter = 0;         counter++;
        r = ft_search_node(brt, node, search, bfe.child_to_read, getf, getf_v, &doprefetch, ftcursor, &unlockers, (ANCESTORS)NULL, &infinite_bounds, can_bulk_fetch);
        if (r==TOKUDB_TRY_AGAIN) {
            // there are two cases where we get TOKUDB_TRY_AGAIN
            //  case 1 is when some later call to toku_pin_ftnode returned
            //  that value and unpinned all the nodes anyway. case 2
            //  is when ft_search_node had to stop its search because
            //  some piece of a node that it needed was not in memory.
            //  In this case, the node was not unpinned, so we unpin it here
            if (unlockers.locked) {
                toku_unpin_ftnode_read_only(brt, node);
            }
            goto try_again;
        } else {
            assert(unlockers.locked);
        }
    }

    assert(unlockers.locked);
    toku_unpin_ftnode_read_only(brt, node);


    //Heaviside function (+direction) queries define only a lower or upper
    //bound.  Some queries require both an upper and lower bound.
    //They do this by wrapping the FT_GET_CALLBACK_FUNCTION with another
    //test that checks for the other bound.  If the other bound fails,
    //it returns TOKUDB_FOUND_BUT_REJECTED which means not found, but
    //stop searching immediately, as opposed to DB_NOTFOUND
    //which can mean not found, but keep looking in another leaf.
    if (r==TOKUDB_FOUND_BUT_REJECTED) r = DB_NOTFOUND;
    else if (r==DB_NOTFOUND) {
        //We truly did not find an answer to the query.
        //Therefore, the FT_GET_CALLBACK_FUNCTION has NOT been called.
        //The contract specifies that the callback function must be called
        //for 'r= (0|DB_NOTFOUND|TOKUDB_FOUND_BUT_REJECTED)'
        //TODO: #1378 This is not the ultimate location of this call to the
        //callback.  It is surely wrong for node-level locking, and probably
        //wrong for the STRADDLE callback for heaviside function(two sets of key/vals)
        int r2 = getf(0,NULL, 0,NULL, getf_v, false);
        if (r2!=0) r = r2;
    }
    {   // accounting (to detect and measure thrashing)
        uint retrycount = trycount - 1;         // how many retries were needed?
        if (retrycount) STATUS_VALUE(FT_TOTAL_RETRIES) += retrycount;
        if (retrycount > tree_height) {         // if at least one node was read from disk more than once
            STATUS_VALUE(FT_SEARCH_TRIES_GT_HEIGHT)++;
            uint excess_tries = retrycount - tree_height;
            if (excess_tries > STATUS_VALUE(FT_MAX_SEARCH_EXCESS_RETRIES))
                STATUS_VALUE(FT_MAX_SEARCH_EXCESS_RETRIES) = excess_tries;
            if (retrycount > (tree_height+3))
                STATUS_VALUE(FT_SEARCH_TRIES_GT_HEIGHTPLUS3)++;
        }
    }
    return r;
}

struct ft_cursor_search_struct {
    FT_GET_CALLBACK_FUNCTION getf;
    void *getf_v;
    FT_CURSOR cursor;
    ft_search_t *search;
};

/* search for the first kv pair that matches the search object */
static int
ft_cursor_search(FT_CURSOR cursor, ft_search_t *search, FT_GET_CALLBACK_FUNCTION getf, void *getf_v, BOOL can_bulk_fetch)
{
    int r = toku_ft_search(cursor->ft_handle, search, getf, getf_v, cursor, can_bulk_fetch);
    return r;
}

static inline int compare_k_x(FT_HANDLE brt, const DBT *k, const DBT *x) {
    FAKE_DB(db, &brt->ft->cmp_descriptor);
    return brt->ft->compare_fun(&db, k, x);
}

static int
ft_cursor_compare_one(ft_search_t *search __attribute__((__unused__)), DBT *x __attribute__((__unused__)))
{
    return 1;
}

static int ft_cursor_compare_set(ft_search_t *search, DBT *x) {
    FT_HANDLE brt = search->context;
    return compare_k_x(brt, search->k, x) <= 0; /* return min xy: kv <= xy */
}

static int
ft_cursor_current_getf(ITEMLEN keylen,                 bytevec key,
                        ITEMLEN vallen,                 bytevec val,
                        void *v, bool lock_only) {
    struct ft_cursor_search_struct *bcss = v;
    int r;
    if (key==NULL) {
        r = bcss->getf(0, NULL, 0, NULL, bcss->getf_v, lock_only);
    } else {
        FT_CURSOR cursor = bcss->cursor;
        DBT newkey = {.size=keylen, .data=(void*)key}; // initializes other fields to zero
        if (compare_k_x(cursor->ft_handle, &cursor->key, &newkey) != 0) {
            r = bcss->getf(0, NULL, 0, NULL, bcss->getf_v, lock_only); // This was once DB_KEYEMPTY
            if (r==0) r = TOKUDB_FOUND_BUT_REJECTED;
        }
        else
            r = bcss->getf(keylen, key, vallen, val, bcss->getf_v, lock_only);
    }
    return r;
}

int
toku_ft_cursor_current(FT_CURSOR cursor, int op, FT_GET_CALLBACK_FUNCTION getf, void *getf_v)
{
    if (ft_cursor_not_set(cursor))
        return EINVAL;
    if (op == DB_CURRENT) {
        struct ft_cursor_search_struct bcss = {getf, getf_v, cursor, 0};
        ft_search_t search; ft_search_init(&search, ft_cursor_compare_set, FT_SEARCH_LEFT, &cursor->key, cursor->ft_handle);
        int r = toku_ft_search(cursor->ft_handle, &search, ft_cursor_current_getf, &bcss, cursor, FALSE);
        ft_search_finish(&search);
        return r;
    }
    return getf(cursor->key.size, cursor->key.data, cursor->val.size, cursor->val.data, getf_v, false); // ft_cursor_copyout(cursor, outkey, outval);
}

int
toku_ft_cursor_first(FT_CURSOR cursor, FT_GET_CALLBACK_FUNCTION getf, void *getf_v)
{
    ft_search_t search; ft_search_init(&search, ft_cursor_compare_one, FT_SEARCH_LEFT, 0, cursor->ft_handle);
    int r = ft_cursor_search(cursor, &search, getf, getf_v, FALSE);
    ft_search_finish(&search);
    return r;
}

int
toku_ft_cursor_last(FT_CURSOR cursor, FT_GET_CALLBACK_FUNCTION getf, void *getf_v)
{
    ft_search_t search; ft_search_init(&search, ft_cursor_compare_one, FT_SEARCH_RIGHT, 0, cursor->ft_handle);
    int r = ft_cursor_search(cursor, &search, getf, getf_v, FALSE);
    ft_search_finish(&search);
    return r;
}

static int ft_cursor_compare_next(ft_search_t *search, DBT *x) {
    FT_HANDLE brt = search->context;
    return compare_k_x(brt, search->k, x) < 0; /* return min xy: kv < xy */
}


static int
ft_cursor_shortcut (
    FT_CURSOR cursor,
    int direction,
    FT_GET_CALLBACK_FUNCTION getf,
    void *getf_v,
    u_int32_t *keylen,
    void **key,
    u_int32_t *vallen,
    void **val
    )
{
    int r = 0;
    u_int32_t index = cursor->leaf_info.to_be.index;
    OMT omt = cursor->leaf_info.to_be.omt;
    // if we are searching towards the end, limit is last element
    // if we are searching towards the beginning, limit is the first element
    u_int32_t limit = (direction > 0) ? (toku_omt_size(omt) - 1) : 0;

    //Starting with the prev, find the first real (non-provdel) leafentry.
    OMTVALUE le = NULL;
    while (index != limit) {
        index += direction;
        r = toku_omt_fetch(omt, index, &le);
        assert_zero(r);

        if (toku_ft_cursor_is_leaf_mode(cursor) || !is_le_val_del(le, cursor)) {

            ft_cursor_extract_key_and_val(
                le,
                cursor,
                keylen,
                key,
                vallen,
                val
                );

            r = getf(*keylen, *key, *vallen, *val, getf_v, false);
            if (r == 0 || r == TOKUDB_CURSOR_CONTINUE) {
                //Update cursor.
                cursor->leaf_info.to_be.index = index;
            }
            if (r == TOKUDB_CURSOR_CONTINUE) {
                continue;
            }
            else {
                break;
            }
        }
    }

    return r;
}

int
toku_ft_cursor_next(FT_CURSOR cursor, FT_GET_CALLBACK_FUNCTION getf, void *getf_v)
{
    ft_search_t search; ft_search_init(&search, ft_cursor_compare_next, FT_SEARCH_LEFT, &cursor->key, cursor->ft_handle);
    int r = ft_cursor_search(cursor, &search, getf, getf_v, TRUE);
    ft_search_finish(&search);
    if (r == 0) ft_cursor_set_prefetching(cursor);
    return r;
}

static int
ft_cursor_search_eq_k_x_getf(ITEMLEN keylen,               bytevec key,
                              ITEMLEN vallen,               bytevec val,
                              void *v, bool lock_only) {
    struct ft_cursor_search_struct *bcss = v;
    int r;
    if (key==NULL) {
        r = bcss->getf(0, NULL, 0, NULL, bcss->getf_v, false);
    } else {
        FT_CURSOR cursor = bcss->cursor;
        DBT newkey = {.size=keylen, .data=(void*)key}; // initializes other fields to zero
        if (compare_k_x(cursor->ft_handle, bcss->search->k, &newkey) == 0) {
            r = bcss->getf(keylen, key, vallen, val, bcss->getf_v, lock_only);
        } else {
            r = bcss->getf(0, NULL, 0, NULL, bcss->getf_v, lock_only);
            if (r==0) r = TOKUDB_FOUND_BUT_REJECTED;
        }
    }
    return r;
}

/* search for the kv pair that matches the search object and is equal to k */
static int
ft_cursor_search_eq_k_x(FT_CURSOR cursor, ft_search_t *search, FT_GET_CALLBACK_FUNCTION getf, void *getf_v)
{
    struct ft_cursor_search_struct bcss = {getf, getf_v, cursor, search};
    int r = toku_ft_search(cursor->ft_handle, search, ft_cursor_search_eq_k_x_getf, &bcss, cursor, FALSE);
    return r;
}

static int ft_cursor_compare_prev(ft_search_t *search, DBT *x) {
    FT_HANDLE brt = search->context;
    return compare_k_x(brt, search->k, x) > 0; /* return max xy: kv > xy */
}

int
toku_ft_cursor_prev(FT_CURSOR cursor, FT_GET_CALLBACK_FUNCTION getf, void *getf_v)
{
    ft_search_t search; ft_search_init(&search, ft_cursor_compare_prev, FT_SEARCH_RIGHT, &cursor->key, cursor->ft_handle);
    int r = ft_cursor_search(cursor, &search, getf, getf_v, TRUE);
    ft_search_finish(&search);
    return r;
}

static int ft_cursor_compare_set_range(ft_search_t *search, DBT *x) {
    FT_HANDLE brt = search->context;
    return compare_k_x(brt, search->k,        x) <= 0; /* return kv <= xy */
}

int
toku_ft_cursor_set(FT_CURSOR cursor, DBT *key, FT_GET_CALLBACK_FUNCTION getf, void *getf_v)
{
    ft_search_t search; ft_search_init(&search, ft_cursor_compare_set_range, FT_SEARCH_LEFT, key, cursor->ft_handle);
    int r = ft_cursor_search_eq_k_x(cursor, &search, getf, getf_v);
    ft_search_finish(&search);
    return r;
}

int
toku_ft_cursor_set_range(FT_CURSOR cursor, DBT *key, FT_GET_CALLBACK_FUNCTION getf, void *getf_v)
{
    ft_search_t search; ft_search_init(&search, ft_cursor_compare_set_range, FT_SEARCH_LEFT, key, cursor->ft_handle);
    int r = ft_cursor_search(cursor, &search, getf, getf_v, FALSE);
    ft_search_finish(&search);
    return r;
}

static int ft_cursor_compare_set_range_reverse(ft_search_t *search, DBT *x) {
    FT_HANDLE brt = search->context;
    return compare_k_x(brt, search->k, x) >= 0; /* return kv >= xy */
}

int
toku_ft_cursor_set_range_reverse(FT_CURSOR cursor, DBT *key, FT_GET_CALLBACK_FUNCTION getf, void *getf_v)
{
    ft_search_t search; ft_search_init(&search, ft_cursor_compare_set_range_reverse, FT_SEARCH_RIGHT, key, cursor->ft_handle);
    int r = ft_cursor_search(cursor, &search, getf, getf_v, FALSE);
    ft_search_finish(&search);
    return r;
}


//TODO: When tests have been rewritten, get rid of this function.
//Only used by tests.
int
toku_ft_cursor_get (FT_CURSOR cursor, DBT *key, FT_GET_CALLBACK_FUNCTION getf, void *getf_v, int get_flags)
{
    int op = get_flags & DB_OPFLAGS_MASK;
    if (get_flags & ~DB_OPFLAGS_MASK)
        return EINVAL;

    switch (op) {
    case DB_CURRENT:
    case DB_CURRENT_BINDING:
        return toku_ft_cursor_current(cursor, op, getf, getf_v);
    case DB_FIRST:
        return toku_ft_cursor_first(cursor, getf, getf_v);
    case DB_LAST:
        return toku_ft_cursor_last(cursor, getf, getf_v);
    case DB_NEXT:
    case DB_NEXT_NODUP:
        if (ft_cursor_not_set(cursor))
            return toku_ft_cursor_first(cursor, getf, getf_v);
        else
            return toku_ft_cursor_next(cursor, getf, getf_v);
    case DB_PREV:
    case DB_PREV_NODUP:
        if (ft_cursor_not_set(cursor))
            return toku_ft_cursor_last(cursor, getf, getf_v);
        else
            return toku_ft_cursor_prev(cursor, getf, getf_v);
    case DB_SET:
        return toku_ft_cursor_set(cursor, key, getf, getf_v);
    case DB_SET_RANGE:
        return toku_ft_cursor_set_range(cursor, key, getf, getf_v);
    default: ;// Fall through
    }
    return EINVAL;
}

void
toku_ft_cursor_peek(FT_CURSOR cursor, const DBT **pkey, const DBT **pval)
// Effect: Retrieves a pointer to the DBTs for the current key and value.
// Requires:  The caller may not modify the DBTs or the memory at which they points.
// Requires:  The caller must be in the context of a
// FT_GET_(STRADDLE_)CALLBACK_FUNCTION
{
    *pkey = &cursor->key;
    *pval = &cursor->val;
}

//We pass in toku_dbt_fake to the search functions, since it will not pass the
//key(or val) to the heaviside function if key(or val) is NULL.
//It is not used for anything else,
//the actual 'extra' information for the heaviside function is inside the
//wrapper.
static const DBT __toku_dbt_fake;
static const DBT* const toku_dbt_fake = &__toku_dbt_fake;

BOOL toku_ft_cursor_uninitialized(FT_CURSOR c) {
    return ft_cursor_not_set(c);
}


/* ********************************* lookup **************************************/

int
toku_ft_lookup (FT_HANDLE brt, DBT *k, FT_GET_CALLBACK_FUNCTION getf, void *getf_v)
{
    int r, rr;
    FT_CURSOR cursor;

    rr = toku_ft_cursor(brt, &cursor, NULL, FALSE, FALSE);
    if (rr != 0) return rr;

    int op = DB_SET;
    r = toku_ft_cursor_get(cursor, k, getf, getf_v, op);

    rr = toku_ft_cursor_close(cursor); assert_zero(rr);

    return r;
}

/* ********************************* delete **************************************/
static int
getf_nothing (ITEMLEN UU(keylen), bytevec UU(key), ITEMLEN UU(vallen), bytevec UU(val), void *UU(pair_v), bool UU(lock_only)) {
    return 0;
}

int
toku_ft_cursor_delete(FT_CURSOR cursor, int flags, TOKUTXN txn) {
    int r;

    int unchecked_flags = flags;
    BOOL error_if_missing = (BOOL) !(flags&DB_DELETE_ANY);
    unchecked_flags &= ~DB_DELETE_ANY;
    if (unchecked_flags!=0) r = EINVAL;
    else if (ft_cursor_not_set(cursor)) r = EINVAL;
    else {
        r = 0;
        if (error_if_missing) {
            r = toku_ft_cursor_current(cursor, DB_CURRENT, getf_nothing, NULL);
        }
        if (r == 0) {
            r = toku_ft_delete(cursor->ft_handle, &cursor->key, txn);
        }
    }
    return r;
}

/* ********************* keyrange ************************ */


struct keyrange_compare_s {
    FT_HANDLE ft_handle;
    DBT *key;
};

static int
keyrange_compare (OMTVALUE lev, void *extra) {
    LEAFENTRY le = lev;
    u_int32_t keylen;
    void* key = le_key_and_len(le, &keylen);
    DBT   omt_dbt;
    toku_fill_dbt(&omt_dbt, key, keylen);
    struct keyrange_compare_s *s = extra;
    // TODO: maybe put a const fake_db in the header
    FAKE_DB(db, &s->ft_handle->ft->cmp_descriptor);
    return s->ft_handle->ft->compare_fun(&db, &omt_dbt, s->key);
}

static void
keyrange_in_leaf_partition (FT_HANDLE brt, FTNODE node, DBT *key, int child_number, u_int64_t estimated_num_rows,
                            u_int64_t *less, u_int64_t *equal, u_int64_t *greater)
// If the partition is in main memory then estimate the number
// If KEY==NULL then use an arbitrary key (leftmost or zero)
{
    assert(node->height == 0); // we are in a leaf
    if (BP_STATE(node, child_number) == PT_AVAIL) {
        // If the partition is in main memory then get an exact count.
        struct keyrange_compare_s s = {brt,key};
        BASEMENTNODE bn = BLB(node, child_number);
        OMTVALUE datav;
        u_int32_t idx = 0;
        // if key is NULL then set r==-1 and idx==0.
        int r = key ? toku_omt_find_zero(bn->buffer, keyrange_compare, &s, &datav, &idx) : -1;
        if (r==0) {
            *less    = idx;
            *equal   = 1;
            *greater = toku_omt_size(bn->buffer)-idx-1;
        } else {
            // If not found, then the idx says where it's between.
            *less    = idx;
            *equal   = 0;
            *greater = toku_omt_size(bn->buffer)-idx;
        }
    } else {
        *less    = estimated_num_rows / 2;
        *equal   = 0;
        *greater = *less;
    }
}

static int
toku_ft_keyrange_internal (FT_HANDLE brt, FTNODE node,
                            DBT *key, u_int64_t *less, u_int64_t *equal, u_int64_t *greater,
                            u_int64_t estimated_num_rows,
                            struct ftnode_fetch_extra *bfe, // set up to read a minimal read.
                            struct unlockers *unlockers, ANCESTORS ancestors, struct pivot_bounds const * const bounds)
// Implementation note: Assign values to less, equal, and greater, and then on the way out (returning up the stack) we add more values in.
{
    int r = 0;
    // if KEY is NULL then use the leftmost key.
    int child_number = key ? toku_ftnode_which_child (node, key, &brt->ft->cmp_descriptor, brt->ft->compare_fun) : 0;
    uint64_t rows_per_child = estimated_num_rows / node->n_children;
    if (node->height == 0) {

        keyrange_in_leaf_partition(brt, node, key, child_number, rows_per_child, less, equal, greater);

        *less    += rows_per_child * child_number;
        *greater += rows_per_child * (node->n_children - child_number - 1);

    } else {
        // do the child.
        struct ancestors next_ancestors = {node, child_number, ancestors};
        BLOCKNUM childblocknum = BP_BLOCKNUM(node, child_number);
        u_int32_t fullhash = compute_child_fullhash(brt->ft->cf, node, child_number);
        FTNODE childnode;
        BOOL msgs_applied = FALSE;
        r = toku_pin_ftnode(
            brt,
            childblocknum,
            fullhash,
            unlockers,
            &next_ancestors,
            bounds,
            bfe,
            FALSE, // may_modify_node is FALSE, because node guaranteed to not change
            FALSE,
            &childnode,
            &msgs_applied
            );
        assert(!msgs_applied);
        if (r != TOKUDB_TRY_AGAIN) {
            assert(r == 0);

            struct unlock_ftnode_extra unlock_extra   = {brt,childnode,FALSE};
            struct unlockers next_unlockers = {TRUE, unlock_ftnode_fun, (void*)&unlock_extra, unlockers};
            const struct pivot_bounds next_bounds = next_pivot_keys(node, child_number, bounds);

            r = toku_ft_keyrange_internal(brt, childnode, key, less, equal, greater, rows_per_child,
                                           bfe, &next_unlockers, &next_ancestors, &next_bounds);
            if (r != TOKUDB_TRY_AGAIN) {
                assert(r == 0);

                *less    += rows_per_child * child_number;
                *greater += rows_per_child * (node->n_children - child_number - 1);

                assert(unlockers->locked);
                toku_unpin_ftnode_read_only(brt, childnode);
            }
        }
    }
    return r;
}

int
toku_ft_keyrange (FT_HANDLE brt, DBT *key, u_int64_t *less_p, u_int64_t *equal_p, u_int64_t *greater_p)
// Effect: Return an estimate  of the number of keys to the left, the number equal, and the number to the right of the key.
//   The values are an estimate.
//   If you perform a keyrange on two keys that are in the same in-memory and uncompressed basement,
//   you can use the keys_right numbers (or the keys_left) numbers to get an exact number keys in the range,
//   if the basement does not change between the keyrange queries.
//   TODO 4184: What to do with a NULL key?
//   If KEY is NULL then the system picks an arbitrary key and returns it.
{
    assert(brt->ft);
    struct ftnode_fetch_extra bfe;
    fill_bfe_for_min_read(&bfe, brt->ft);  // read pivot keys but not message buffers
try_again:
    {
        u_int64_t less = 0, equal = 0, greater = 0;
        FTNODE node = NULL;
        {
            toku_ft_grab_treelock(brt->ft);

            u_int32_t fullhash;
            CACHEKEY root_key;
            toku_calculate_root_offset_pointer(brt->ft, &root_key, &fullhash);
            toku_pin_ftnode_off_client_thread(
                brt->ft,
                root_key,
                fullhash,
                &bfe,
                FALSE, // may_modify_node, cannot change root during keyrange
                0,
                NULL,
                &node
                );
            toku_ft_release_treelock(brt->ft);
        }

        struct unlock_ftnode_extra unlock_extra = {brt,node,FALSE};
        struct unlockers unlockers = {TRUE, unlock_ftnode_fun, (void*)&unlock_extra, (UNLOCKERS)NULL};

        {
            int64_t numrows = brt->ft->in_memory_stats.numrows;
            if (numrows < 0)
                numrows = 0;  // prevent appearance of a negative number
            int r = toku_ft_keyrange_internal (brt, node, key,
                                                &less, &equal, &greater,
                                                numrows,
                                                &bfe, &unlockers, (ANCESTORS)NULL, &infinite_bounds);
            assert(r == 0 || r == TOKUDB_TRY_AGAIN);
            if (r == TOKUDB_TRY_AGAIN) {
                assert(!unlockers.locked);
                goto try_again;
            }
        }
        assert(unlockers.locked);
        toku_unpin_ftnode_read_only(brt, node);
        *less_p    = less;
        *equal_p   = equal;
        *greater_p = greater;
    }
    return 0;
}

int
toku_ft_handle_stat64 (FT_HANDLE brt, TOKUTXN UU(txn), struct ftstat64_s *s) {
    assert(brt->ft);
    toku_ft_stat64(brt->ft, s);
    return 0;
}

/* ********************* debugging dump ************************ */
static int
toku_dump_ftnode (FILE *file, FT_HANDLE brt, BLOCKNUM blocknum, int depth, const DBT *lorange, const DBT *hirange) {
    int result=0;
    FTNODE node;
    void* node_v;
    toku_get_node_for_verify(blocknum, brt, &node);
    result=toku_verify_ftnode(brt, ZERO_MSN, ZERO_MSN, node, -1, lorange, hirange, NULL, NULL, 0, 1, 0);
    u_int32_t fullhash = toku_cachetable_hash(brt->ft->cf, blocknum);
    struct ftnode_fetch_extra bfe;
    fill_bfe_for_full_read(&bfe, brt->ft);
    int r = toku_cachetable_get_and_pin(
        brt->ft->cf,
        blocknum,
        fullhash,
        &node_v,
        NULL,
        get_write_callbacks_for_node(brt->ft),
        toku_ftnode_fetch_callback,
        toku_ftnode_pf_req_callback,
        toku_ftnode_pf_callback,
        TRUE, // may_modify_value, just safe to set to TRUE, I think it could theoretically be FALSE
        &bfe
        );
    assert_zero(r);
    node=node_v;
    assert(node->fullhash==fullhash);
    fprintf(file, "%*sNode=%p\n", depth, "", node);

    fprintf(file, "%*sNode %"PRId64" nodesize=%u height=%d n_children=%d  keyrange=%s %s\n",
            depth, "", blocknum.b, node->nodesize, node->height, node->n_children, (char*)(lorange ? lorange->data : 0), (char*)(hirange ? hirange->data : 0));
    {
        int i;
        for (i=0; i+1< node->n_children; i++) {
            fprintf(file, "%*spivotkey %d =", depth+1, "", i);
            toku_print_BYTESTRING(file, node->childkeys[i].size, node->childkeys[i].data);
            fprintf(file, "\n");
        }
        for (i=0; i< node->n_children; i++) {
            if (node->height > 0) {
                NONLEAF_CHILDINFO bnc = BNC(node, i);
                fprintf(file, "%*schild %d buffered (%d entries):", depth+1, "", i, toku_bnc_n_entries(bnc));
                FIFO_ITERATE(bnc->buffer, key, keylen, data, datalen, type, msn, xids, UU(is_fresh),
                             {
                                 data=data; datalen=datalen; keylen=keylen;
                                 fprintf(file, "%*s xid=%"PRIu64" %u (type=%d) msn=0x%"PRIu64"\n", depth+2, "", xids_get_innermost_xid(xids), (unsigned)toku_dtoh32(*(int*)key), type, msn.msn);
                                 //assert(strlen((char*)key)+1==keylen);
                                 //assert(strlen((char*)data)+1==datalen);
                             });
            }
            else {
                int size = toku_omt_size(BLB_BUFFER(node, i));
                if (0)
                    for (int j=0; j<size; j++) {
                        OMTVALUE v = 0;
                        r = toku_omt_fetch(BLB_BUFFER(node, i), j, &v);
                        assert_zero(r);
                        fprintf(file, " [%d]=", j);
                        print_leafentry(file, v);
                        fprintf(file, "\n");
                    }
                //               printf(" (%d)%u ", len, *(int*)le_key(data)));
                fprintf(file, "\n");
            }
        }
        if (node->height > 0) {
            for (i=0; i<node->n_children; i++) {
                fprintf(file, "%*schild %d\n", depth, "", i);
                if (i>0) {
                    char *key = node->childkeys[i-1].data;
                    fprintf(file, "%*spivot %d len=%u %u\n", depth+1, "", i-1, node->childkeys[i-1].size, (unsigned)toku_dtoh32(*(int*)key));
                }
                toku_dump_ftnode(file, brt, BP_BLOCKNUM(node, i), depth+4,
                                  (i==0) ? lorange : &node->childkeys[i-1],
                                  (i==node->n_children-1) ? hirange : &node->childkeys[i]);
            }
        }
    }
    r = toku_cachetable_unpin(brt->ft->cf, blocknum, fullhash, CACHETABLE_CLEAN, make_ftnode_pair_attr(node));
    assert_zero(r);
    return result;
}

int toku_dump_ft (FILE *f, FT_HANDLE brt) {
    int r;
    assert(brt->ft);
    toku_dump_translation_table(f, brt->ft->blocktable);
    {
        toku_ft_grab_treelock(brt->ft);

        u_int32_t fullhash = 0;
        CACHEKEY root_key;
        toku_calculate_root_offset_pointer(brt->ft, &root_key, &fullhash);
        r = toku_dump_ftnode(f, brt, root_key, 0, 0, 0);

        toku_ft_release_treelock(brt->ft);
    }
    return r;
}

int toku_ft_layer_init(void) {
    int r = 0;
    //Portability must be initialized first
    r = toku_portability_init();
    if (r) { goto exit; }

    toku_checkpoint_init();

    toku_ft_serialize_layer_init();

    toku_mutex_init(&ft_open_close_lock, NULL);
exit:
    return r;
}

void toku_ft_layer_destroy(void) {
    toku_mutex_destroy(&ft_open_close_lock);

    toku_ft_serialize_layer_destroy();
    toku_checkpoint_destroy();
    //Portability must be cleaned up last
    toku_portability_destroy();
}

// This lock serializes all opens and closes because the cachetable requires that clients do not try to open or close a cachefile in parallel.  We made
// it coarser by not allowing any cachefiles to be open or closed in parallel.
void toku_ft_open_close_lock(void) {
    toku_mutex_lock(&ft_open_close_lock);
}

void toku_ft_open_close_unlock(void) {
    toku_mutex_unlock(&ft_open_close_lock);
}

//Suppress both rollback and recovery logs.
void
toku_ft_suppress_recovery_logs (FT_HANDLE brt, TOKUTXN txn) {
    assert(brt->ft->txnid_that_created_or_locked_when_empty == toku_txn_get_txnid(txn));
    assert(brt->ft->txnid_that_suppressed_recovery_logs           == TXNID_NONE);
    brt->ft->txnid_that_suppressed_recovery_logs                   = toku_txn_get_txnid(txn);
    txn->checkpoint_needed_before_commit = TRUE;
}

int toku_ft_handle_set_panic(FT_HANDLE brt, int panic, char *panic_string) {
    return toku_ft_set_panic(brt->ft, panic, panic_string);
}

// Prepare to remove a dictionary from the database when this transaction is committed:
//  - mark transaction as NEED fsync on commit
//  - make entry in rollback log
//  - make fdelete entry in recovery log
//
// Effect: when the txn commits, the ft's cachefile will be marked as unlink
//         on close. see toku_commit_fdelete and how unlink on close works
//         in toku_cachefile_close();
// Requires: serialized with begin checkpoint
//           this does not need to take the open close lock because
//           1.) the ft/cf cannot go away because we have a live handle.
//           2.) we're not setting the unlink on close bit _here_. that
//           happens on txn commit (as the name suggests).
//           3.) we're already holding the multi operation lock to 
//           synchronize with begin checkpoint.
// Contract: the iname of the ft should never be reused.
int 
toku_ft_unlink_on_commit(FT_HANDLE handle, TOKUTXN txn) {
    int r;
    CACHEFILE cf;

    assert(txn);
    cf = handle->ft->cf;
    FT ft = toku_cachefile_get_userdata(cf);

    toku_txn_maybe_note_ft(txn, ft);

    // If the txn commits, the commit MUST be in the log before the file is actually unlinked
    toku_txn_force_fsync_on_commit(txn); 
    // make entry in rollback log
    FILENUM filenum = toku_cachefile_filenum(cf);
    r = toku_logger_save_rollback_fdelete(txn, filenum);
    assert_zero(r);
    // make entry in recovery log
    r = toku_logger_log_fdelete(txn, filenum);
    return r;
}

// Non-transactional version of fdelete
//
// Effect: The ft file is unlinked when the handle closes and it's ft is not
//         pinned by checkpoint. see toku_remove_ft_ref() and how unlink on
//         close works in toku_cachefile_close();
// Requires: serialized with begin checkpoint
void 
toku_ft_unlink(FT_HANDLE handle) {
    CACHEFILE cf;
    cf = handle->ft->cf;
    toku_cachefile_unlink_on_close(cf);
}

int
toku_ft_get_fragmentation(FT_HANDLE brt, TOKU_DB_FRAGMENTATION report) {
    int r;

    int fd = toku_cachefile_get_fd(brt->ft->cf);
    toku_ft_lock(brt->ft);

    int64_t file_size;
    r = toku_os_get_file_size(fd, &file_size);
    if (r==0) {
        report->file_size_bytes = file_size;
        toku_block_table_get_fragmentation_unlocked(brt->ft->blocktable, report);
    }
    toku_ft_unlock(brt->ft);
    return r;
}

static BOOL is_empty_fast_iter (FT_HANDLE brt, FTNODE node) {
    if (node->height > 0) {
        for (int childnum=0; childnum<node->n_children; childnum++) {
            if (toku_bnc_nbytesinbuf(BNC(node, childnum)) != 0) {
                return 0; // it's not empty if there are bytes in buffers
            }
            FTNODE childnode;
            {
                BLOCKNUM childblocknum = BP_BLOCKNUM(node,childnum);
                u_int32_t fullhash =  compute_child_fullhash(brt->ft->cf, node, childnum);
                struct ftnode_fetch_extra bfe;
                fill_bfe_for_full_read(&bfe, brt->ft);
                // don't need to pass in dependent nodes as we are not
                // modifying nodes we are pinning
                toku_pin_ftnode_off_client_thread(
                    brt->ft,
                    childblocknum,
                    fullhash,
                    &bfe,
                    FALSE, // may_modify_node set to FALSE, as nodes not modified
                    0,
                    NULL,
                    &childnode
                    );
            }
            int child_is_empty = is_empty_fast_iter(brt, childnode);
            toku_unpin_ftnode(brt->ft, childnode);
            if (!child_is_empty) return 0;
        }
        return 1;
    } else {
        // leaf:  If the omt is empty, we are happy.
        for (int i = 0; i < node->n_children; i++) {
            if (toku_omt_size(BLB_BUFFER(node, i))) {
                return FALSE;
            }
        }
        return TRUE;
    }
}

BOOL toku_ft_is_empty_fast (FT_HANDLE brt)
// A fast check to see if the tree is empty.  If there are any messages or leafentries, we consider the tree to be nonempty.  It's possible that those
// messages and leafentries would all optimize away and that the tree is empty, but we'll say it is nonempty.
{
    u_int32_t fullhash;
    FTNODE node;
    //assert(fullhash == toku_cachetable_hash(brt->ft->cf, *rootp));
    {
        toku_ft_grab_treelock(brt->ft);

        CACHEKEY root_key;
        toku_calculate_root_offset_pointer(brt->ft, &root_key, &fullhash);
        struct ftnode_fetch_extra bfe;
        fill_bfe_for_full_read(&bfe, brt->ft);
        toku_pin_ftnode_off_client_thread(
            brt->ft,
            root_key,
            fullhash,
            &bfe,
            FALSE, // may_modify_node set to FALSE, node does not change
            0,
            NULL,
            &node
            );

        toku_ft_release_treelock(brt->ft);
    }
    BOOL r = is_empty_fast_iter(brt, node);
    toku_unpin_ftnode(brt->ft, node);
    return r;
}

int toku_ft_strerror_r(int error, char *buf, size_t buflen)
{
    if (error>=0) {
        return (long) strerror_r(error, buf, buflen);
    } else {
        switch (error) {
        case DB_KEYEXIST:
            snprintf(buf, buflen, "Key exists");
            return 0;
        case TOKUDB_CANCELED:
            snprintf(buf, buflen, "User canceled operation");
            return 0;
        default:
            snprintf(buf, buflen, "Unknown error %d", error);
            errno = EINVAL;
            return -1;
        }
    }
}

#include <valgrind/helgrind.h>
void __attribute__((__constructor__)) toku_ft_helgrind_ignore(void);
void
toku_ft_helgrind_ignore(void) {
    VALGRIND_HG_DISABLE_CHECKING(&ft_status, sizeof ft_status);
}

#undef STATUS_VALUE
