/*
 * Copyright (c) 2019, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 * LLNL-CODE-741539
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
 /*
  * This file is a simple, thread-safe, segment tree implementation.  The
  * segments in the tree are non-overlapping.  Added segments overwrite the old
  * segments in the tree.  This is used to coalesce writes before an fsync.
  */
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#include "seg_tree.h"
#include "tree.h"

#define MIN(a, b) (a < b ? a : b)
#define MAX(a, b) (a > b ? a : b)

int
compare_func(struct seg_tree_node* node1, struct seg_tree_node* node2)
{
    if (node1->start > node2->end) {
        return 1;
    } else if (node1->end < node2->start) {
        return -1;
    } else {
        return 0;
    }
}

RB_PROTOTYPE(inttree, seg_tree_node, entry, compare_func)
RB_GENERATE(inttree, seg_tree_node, entry, compare_func)

/* Returns 0 on success, positive non-zero error code otherwise */
int seg_tree_init(struct seg_tree* seg_tree)
{
    memset(seg_tree, 0, sizeof(*seg_tree));
    pthread_rwlock_init(&seg_tree->rwlock, NULL);
    RB_INIT(&seg_tree->head);

    return 0;
};

/*
 * Remove and free all nodes in the seg_tree.
 */
void seg_tree_destroy(struct seg_tree* seg_tree)
{
    seg_tree_clear(seg_tree);
};

/* Allocate a range tree.  Free it with free() when finished */
static struct seg_tree_node*
seg_tree_node_alloc(unsigned long start, unsigned long end, unsigned long ptr)
{
    struct seg_tree_node* node;
    node = calloc(1, sizeof(*node));
    if (!node) {
        return NULL;
    }

    node->start = start;
    node->end = end;
    node->ptr = ptr;
    return node;
}

/*
 * Given two start/end ranges, return a new range from start1/end1 that
 * does not overlap start2/end2.  The non-overlapping range is stored
 * in new_start/new_end.   If there are no non-overlapping ranges,
 * return 1 from this function, else return 0.  If there are two
 * non-overlapping ranges, return the first one in new_start/new_end.
 */
static int
get_non_overlapping_range(unsigned long start1, unsigned long end1,
    long start2, long end2, long* new_start, long* new_end)
{
    if (start1 >= start2 && end1 <= end2) {
        /* Completely overlapping */
        return 1;
    } else if (start1 < start2) {
        /*
         * s1 ------- e1
         *      s2--------e2
         *    ---- non-overlap
         *
         * also:
         *
         * s1 -------------------e1
         *      s2--------e2
         *    ---- non-overlap
         */
        *new_start = start1;
        *new_end = MIN(end1, start2 - 1);
    } else if (start1 > start2 && end1 > end2) {
        /*
         *       s1 ----- e1
         *  s2------- e2
         */
        *new_start = MAX(start1, end2 + 1);
        *new_end = end1;
    } else if (start1 == start2 && end1 > end2) {
        *new_start = end2 + 1;
        *new_end = end1;
    }
    return 0;
}

/*
 * Add an entry to the range tree.  Returns 0 on success, nonzero otherwise.
 */
int seg_tree_add(struct seg_tree* seg_tree, unsigned long start,
    unsigned long end, unsigned long ptr)
{
    struct seg_tree_node* node;
    struct seg_tree_node* overlap = NULL;
    struct seg_tree_node* resized;
    struct seg_tree_node* remaining;
    long new_start = 0, new_end = 0;
    int rc;

    /* Create our range */
    node = seg_tree_node_alloc(start, end, ptr);
    if (!node) {
        return ENOMEM;
    }

    seg_tree_wrlock(seg_tree);
    /*
     * Try to insert our range into the RB tree.  If it overlaps with any other
     * range, then it is not inserted, and the overlapping range node is
     * returned in 'overlap'.  If 'overlap' is NULL, then there were no
     * overlaps, and our range was successfully inserted.
     */
    while ((overlap = RB_INSERT(inttree, &seg_tree->head, node))) {
        /*
         * Our range overlaps with another range (in 'overlap'). Is there any
         * any part of 'overlap' that does not overlap our range?  If so,
         * delete the old 'overlap' and insert the smaller, non-overlapping
         * range.
         */
        rc = get_non_overlapping_range(overlap->start, overlap->end, start, end,
                &new_start, &new_end);
        if (rc) {
            /* We can't find a non-overlapping range.  Delete the old range. */
            RB_REMOVE(inttree, &seg_tree->head, overlap);
            seg_tree->count--;
            free(overlap);
        } else {
            /*
             * Part of the old range was non-overlapping.  Split the old range
             * into two ranges: one for the non-overlapping section, and one for
             * the remaining section.  The non-overlapping section gets
             * inserted without issue.  The remaining section will be processed
             * on the next pass of this while() loop.
             */
            resized = seg_tree_node_alloc(new_start, new_end,
                overlap->ptr + (new_start - overlap->start));
            if (!resized) {
                return ENOMEM;
            }
            /* Remove our old range */
            RB_REMOVE(inttree, &seg_tree->head, overlap);

            /* Insert the non-overlapping part of the new range */
            RB_INSERT(inttree, &seg_tree->head, resized);

            if (resized->end + 1 >= overlap->start &&
                resized->end +1 <= overlap->end) {
                /*
                 * There's still a remaining section after the non-overlapping
                 * part.  Add it in.
                 */
                remaining = seg_tree_node_alloc(resized->end + 1, overlap->end,
                    overlap->ptr + (resized->end + 1 - overlap->start));
                if (!resized) {
                    free(overlap);
                    return ENOMEM;
                }
                RB_INSERT(inttree, &seg_tree->head, remaining);
                seg_tree->count++;
            }
            free(overlap);
        }
    }
    if (!overlap) {
        seg_tree->count++;
    }

    seg_tree->max = MAX(seg_tree->max, end);
    seg_tree_unlock(seg_tree);

    return 0;
}

/* Search tree for an entry that overlaps with given range of
 * [start, end].  Returns the first overlapping entry if found,
 * which is the overlapping entry having the lowest starting
 * offset, and returns NULL otherwise.  Assumes caller has lock
 * on tree. */
struct seg_tree_node* seg_tree_find_nolock(
    struct seg_tree* seg_tree,
    unsigned long start,
    unsigned long end)
{
    /* Create a range of just our starting byte offset */
    struct seg_tree_node* node = seg_tree_node_alloc(start, start, 0);
    if (!node) {
        return NULL;
    }

    /* Search tree for either a range that overlaps with
     * the target range (starting byte), or otherwise the
     * node for the next biggest starting byte. */
    struct seg_tree_node* next = RB_NFIND(inttree, &seg_tree->head, node);

    free(node);

    /* We may have found a node that doesn't include our starting
     * byte offset, but it would be the range with the lowest
     * starting offset after the target starting offset.  Check whether
     * this overlaps our end offset */
    if (next && next->start <= end) {
        return next;
    }

    /* Otherwise, there is not element that overlaps with the
     * target range of [start, end]. */
    return NULL;
}

/*
 * Given a range tree and a starting node, iterate though all the nodes
 * in the tree, returning the next one each time.  If start is NULL, then
 * start with the first node in the tree.
 *
 * This is meant to be called in a loop, like:
 *
 *    seg_tree_rdlock(seg_tree);
 *
 *    struct seg_tree_node *node = NULL;
 *    while ((node = seg_tree_iter(seg_tree, node))) {
 *       printf("[%d-%d]", node->start, node->end);
 *    }
 *
 *    seg_tree_unlock(seg_tree);
 *
 * Note: this function does no locking, and assumes you're properly locking
 * and unlocking the seg_tree before doing the iteration (see
 * seg_tree_rdlock()/seg_tree_wrlock()/seg_tree_unlock()).
 */
struct seg_tree_node*
seg_tree_iter(struct seg_tree* seg_tree, struct seg_tree_node* start)
{
    struct seg_tree_node* next = NULL;
    if (start == NULL) {
        /* Initial case, no starting node */
        next = RB_MIN(inttree, &seg_tree->head);
        return next;
    }

    /*
     * We were given a valid start node.  Look it up to start our traversal
     * from there.
     */
    next = RB_FIND(inttree, &seg_tree->head, start);
    if (!next) {
        /* Some kind of error */
        return NULL;
    }

    /* Look up our next node */
    next = RB_NEXT(inttree, &seg_tree->head, start);

    return next;
}

/*
 * Lock a seg_tree for reading.  This should only be used for calling
 * seg_tree_iter().  All the other seg_tree functions provide their
 * own locking.
 */
void
seg_tree_rdlock(struct seg_tree* seg_tree)
{
    assert(pthread_rwlock_rdlock(&seg_tree->rwlock) == 0);
}

/*
 * Lock a seg_tree for read/write.  This should only be used for calling
 * seg_tree_iter().  All the other seg_tree functions provide their
 * own locking.
 */
void
seg_tree_wrlock(struct seg_tree* seg_tree)
{
    assert(pthread_rwlock_wrlock(&seg_tree->rwlock) == 0);
}

/*
 * Unlock a seg_tree for read/write.  This should only be used for calling
 * seg_tree_iter().  All the other seg_tree functions provide their
 * own locking.
 */
void
seg_tree_unlock(struct seg_tree* seg_tree)
{
    assert(pthread_rwlock_unlock(&seg_tree->rwlock) == 0);
}

/*
 * Remove all nodes in seg_tree, but keep it initialized so you can
 * seg_tree_add() to it.
 */
void seg_tree_clear(struct seg_tree* seg_tree)
{
    struct seg_tree_node* node = NULL;
    struct seg_tree_node* oldnode = NULL;

    seg_tree_wrlock(seg_tree);

    if (RB_EMPTY(&seg_tree->head)) {
        /* seg_tree is empty, nothing to do */
        seg_tree_unlock(seg_tree);
        return;
    }

    /* Remove and free each node in the tree */
    while ((node = seg_tree_iter(seg_tree, node))) {
        if (oldnode) {
            RB_REMOVE(inttree, &seg_tree->head, oldnode);
            free(oldnode);
        }
        oldnode = node;
    }
    if (oldnode) {
        RB_REMOVE(inttree, &seg_tree->head, oldnode);
        free(oldnode);
    }

    seg_tree->count = 0;
    seg_tree->max = 0;
    seg_tree_unlock(seg_tree);
}

/* Return the number of segments in the segment tree */
unsigned long seg_tree_count(struct seg_tree* seg_tree)
{
    unsigned long count;

    seg_tree_wrlock(seg_tree);
    count = seg_tree->count;
    seg_tree_unlock(seg_tree);
    return count;
}

/* Return the maximum segment value in the tree */
unsigned long seg_tree_max(struct seg_tree* seg_tree)
{
    unsigned long max;

    seg_tree_wrlock(seg_tree);
    max = seg_tree->max;
    seg_tree_unlock(seg_tree);
    return max;
}
