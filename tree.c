/* Metric tree sample implementation.

   This generates a bunch of pseudorandom 32-bit integers, inserts
   them into an index, and queries the index for points within a
   certain distance of the given point.

   That is,

   Let S = { N pseudorandom 32-bit integers }
   Let d(x,y) be the (base-2) Hamming distance between x and y
   Let q(x,r) = { y in S : d(x,y) <= r }

   There are three implementations in here which can be selected at runtime.

   "bk" is a BK-Tree.  Each internal node has a center point, and each
   child node contains a set of all points a certain distance away
   from the center.

   "vp" is a VP-Tree.  Each internal node has a center point and two
   children.  The "near" child contains all points contained in a
   closed ball of a certain radius around the center, and the "far"
   node contains all other points.

   "linear" is a linear search.

   The tree implementations use a linear search for leaf nodes.  The
   maximum number of points in a leaf node is configurable at runtime,
   but 1000 is a good number.  If the number is low, say 1, then the
   memory usage of the tree implementations will skyrocket to
   unreasonable levels: more than 24 bytes per element.

   Note that VP trees are slightly faster than BK trees for this
   problem, and neither tree implementation significantly outperforms
   linear search (that is, by a factor of two or more) for r > 6.  */

#include <stdint.h>
#include <stdlib.h>
#include <err.h>
#include <time.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>

#ifndef DO_PRINT
#define DO_PRINT 0
#endif

#ifndef HAVE_POPCNT
#define HAVE_POPCNT 0
#endif

static uint32_t rand_x0, rand_x1, rand_c;
#define RAND_A 4284966893U

void
seedrand(void)
{
    time_t t;
    time(&t);
    rand_x0 = t;
    fprintf(stderr, "seed: %u\n", rand_x0);
    rand_x1 = 0x038acaf3U;
    rand_c = 0xa2cc5886U;
}

uint32_t
irand(void)
{
    uint64_t y = (uint64_t)rand_x0 * RAND_A + rand_c;
    rand_x0 = rand_x1;
    rand_x1 = y;
    rand_c = y >> 32;
    return y;
}

__attribute__((malloc))
static void *
xmalloc(size_t sz)
{
    void *p;
    if (!sz)
        return NULL;
    p = malloc(sz);
    if (!p)
        err(1, "malloc");
    return p;
}

unsigned long
xatoul(const char *p)
{
    char *e;
    unsigned long x;
    x = strtoul(p, &e, 0);
    if (*e)
        errx(1, "must be a number: '%s'", p);
    return x;
}

typedef uint32_t bkey_t;
enum { MAX_DISTANCE = 32 };

#if HAVE_POPCNT

static inline unsigned
distance(bkey_t x, bkey_t y)
{
    return __builtin_popcount(x^y);
}

#else

static inline unsigned
distance(bkey_t x, bkey_t y)
{
    uint32_t d = x^y;
    d = (d & 0x55555555U) + ((d >> 1) & 0x55555555U);
    d = (d & 0x33333333U) + ((d >> 2) & 0x33333333U);
    d = (d + (d >> 4)) & 0x0f0f0f0fU;
    d = d + (d >> 8);
    d = d + (d >> 16);
    return d & 63;
}

#endif

static char keybuf[33];

static const char *
keystr(bkey_t k)
{
    unsigned i;
    for (i = 0; i < 32; ++i) {
        keybuf[31 - i] = '0' + (k & 1);
        k >>= 1;
    }
    keybuf[32] = '\0';
    return keybuf;
}

static const char *
keystr2(bkey_t k, bkey_t ref)
{
    unsigned i;
    bkey_t d = ref ^ k;
    for (i = 0; i < 32; ++i) {
        keybuf[31 - i] = (d & 1) ? ('0' + (k & 1)) : '.';
        d >>= 1;
        k >>= 1;
    }
    keybuf[32] = '\0';
    return keybuf;
}

static unsigned num_nodes = 0;
static size_t tree_size = 0;

struct buf {
    bkey_t *keys;
    size_t n, a;
};

static void
addkey(struct buf *restrict b, bkey_t k)
{
    size_t na;
    bkey_t *np;
    if (b->n >= b->a) {
        na = b->a ? 2*b->a : 16;
        np = xmalloc(sizeof(*np) * na);
        memcpy(np, b->keys, sizeof(*np) * b->n);
        free(b->keys);
        b->keys = np;
        b->a = na;
    }
    b->keys[b->n++] = k;
}

/* Linear search ==================== */

struct linear {
    size_t count;
    bkey_t *keys;
};

static struct linear *
mktree_linear(const bkey_t *restrict keys, size_t n, size_t max_linear)
{
    struct linear* node;
    (void)max_linear;
    node = xmalloc(sizeof(*node));
    node->count = n;
    node->keys = xmalloc(sizeof(bkey_t) * n);
    num_nodes += 1;
    tree_size += sizeof(bkey_t) * n + sizeof(*node);
    memcpy(node->keys, keys, sizeof(bkey_t) * n);
    return node;
}

static size_t
query_linear(struct buf *restrict b, struct linear *restrict root,
             bkey_t ref, unsigned maxd)
{
    size_t i;
    const bkey_t *restrict p = root->keys;
    for (i = 0; i < root->count; ++i)
        if (distance(ref, p[i]) <= maxd)
            addkey(b, p[i]);
    return root->count;
}

/* BK-tree ==================== */

struct bktree {
    unsigned short distance;
    unsigned short linear;
    union {
        struct {
            bkey_t key;
            struct bktree *child;
        } tree;
        struct {
            unsigned count;
            bkey_t *keys;
        } linear;
    } data;
    struct bktree *sibling;
};

static struct bktree *
mktree_bk(const bkey_t *restrict keys, size_t n, size_t max_linear)
{
    size_t dcnt[MAX_DISTANCE + 1], i, a, pos[MAX_DISTANCE + 1], off, len;
    bkey_t rootkey = keys[0], *tmp;
    struct bktree *root, *child, *prev;
    assert(n > 0);

    num_nodes += 1;

    /* Build root */
    root = xmalloc(sizeof(*root));
    tree_size += sizeof(*root);
    root->distance = 0;
    root->sibling = NULL;
    if (n <= max_linear || n <= 1) {
        root->linear = 1;
        tmp = xmalloc(sizeof(*tmp) * n);
        tree_size += sizeof(*tmp) * n;
        memcpy(tmp, keys, sizeof(*tmp) * n);
        root->data.linear.count = n;
        root->data.linear.keys = tmp;
        return root;
    }
    root->linear = 0;
    root->data.tree.key = rootkey;
    root->data.tree.child = NULL;

    n -= 1;
    keys += 1;
    if (!n)
        return root;

    /* Sort keys by distance to root */
    tmp = xmalloc(sizeof(*tmp) * n);
    for (i = 0; i <= MAX_DISTANCE; ++i)
        dcnt[i] = 0;
    for (i = 0; i < n; ++i)
        dcnt[distance(rootkey, keys[i])]++;
    for (i = 0, a = 0; i <= MAX_DISTANCE; ++i)
        dcnt[i] = (a += dcnt[i]);
    assert(a == n);
    memcpy(pos, dcnt, sizeof(pos));
    for (i = 0; i < n; ++i)
        tmp[--pos[distance(rootkey, keys[i])]] = keys[i];

    /* Add child nodes */
    for (i = 1, prev = NULL; i <= MAX_DISTANCE; ++i) {
        off = dcnt[i-1];
        len = dcnt[i] - off;
        if (!len)
            continue;
        child = mktree_bk(tmp + off, len, max_linear);
        child->distance = i;
        if (prev)
            prev->sibling = child;
        else
            root->data.tree.child = child;
        prev = child;
    }

    free(tmp);
    return root;
}


static size_t
query_bk(struct buf *restrict b, struct bktree *restrict root,
         bkey_t ref, unsigned maxd)
{
    /* We are trying to find x that satisfy d(ref,x) <= maxd
       By triangle inequality, we know: d(root,x) <= d(root,ref) + d(ref,x)
       By algebra: d(root,x) - d(root,ref) <= d(ref,x)
       By transitivity: d(root,x) - d(root,ref) <= maxd
       By algebra: d(root,x) <= maxd + d(root,ref) */
    if (root->linear) {
        const bkey_t *restrict keys = root->data.linear.keys;
        unsigned i, n = root->data.linear.count;
        for (i = 0; i < n; ++i)
            if (distance(ref, keys[i]) <= maxd)
                addkey(b, keys[i]);
        return n;
    } else {
        unsigned d = distance(root->data.tree.key, ref);
        struct bktree *p = root->data.tree.child;
        size_t nc = 1;
        if (d <= maxd)
            addkey(b, root->data.tree.key);
        for (; p && p->distance + maxd < d; p = p->sibling);
        for (; p && p->distance <= maxd + d; p = p->sibling)
            nc += query_bk(b, p, ref, maxd);
        return nc;
    }
}

/* VP-tree ==================== */

struct vptree {
    unsigned short linear;
    union {
        struct {
            /* Closed ball (d = threshold is included) */
            unsigned short threshold;
            bkey_t vantage;
            struct vptree *near;
            struct vptree *far;
        } tree;
        struct {
            unsigned count;
            bkey_t *keys;
        } linear;
    } data;
};

static struct vptree *
mktree_vp(const bkey_t *restrict keys, size_t n, size_t max_linear)
{
    size_t dcnt[MAX_DISTANCE + 1], i, a;
    bkey_t rootkey = keys[0], *tmp;
    struct vptree *root;
    unsigned k;
    size_t median, nnear, nfar, inear, ifar;
    assert(n > 0);

    num_nodes += 1;

    /* Build root */
    root = xmalloc(sizeof(*root));
    tree_size += sizeof(root);
    if (n <= max_linear || n <= 1) {
        root->linear = 1;
        tmp = xmalloc(sizeof(*tmp) * n);
        tree_size += sizeof(*tmp) * n;
        memcpy(tmp, keys, sizeof(*tmp) * n);
        root->data.linear.count = n;
        root->data.linear.keys = tmp;
        return root;
    }
    root->linear = 0;
    root->data.tree.threshold = 0;
    root->data.tree.vantage = rootkey;
    root->data.tree.near = NULL;
    root->data.tree.far = NULL;

    n -= 1;
    keys += 1;
    if (!n)
        return root;

    /* Count keys inside the given ball */
    for (i = 0; i <= MAX_DISTANCE; ++i)
        dcnt[i] = 0;
    for (i = 0; i < n; ++i)
        dcnt[distance(rootkey, keys[i])]++;
    for (i = 0, a = 0; i <= MAX_DISTANCE; ++i)
        dcnt[i] = (a += dcnt[i]);
    assert(a == n);
    median = dcnt[0] + (n - dcnt[0]) / 2;
    for (k = 1; k <= MAX_DISTANCE; ++k)
        if (dcnt[k] > median)
            break;
    if (k != 1 && median - dcnt[k-1] <= dcnt[k] - median)
        k--;
    nnear = dcnt[k] - dcnt[0];
    nfar = n - dcnt[k];
    // printf("keys: %zu; near: %zu; far: %zu; k=%u\n", n, nnear, nfar, k);

    /* Sort keys into near and far sets */
    tmp = xmalloc(sizeof(*tmp) * (nnear + nfar));
    inear = 0;
    ifar = nnear;
    for (i = 0; i < n; ++i) {
        if (keys[i] == rootkey)
            continue;
        if (distance(rootkey, keys[i]) <= k)
            tmp[inear++] = keys[i];
        else
            tmp[ifar++] = keys[i];
    }
    assert(inear == nnear);
    assert(ifar == nnear + nfar);

    root->data.tree.threshold = k;
    if (nnear)
        root->data.tree.near = mktree_vp(tmp, nnear, max_linear);
    if (nfar)
        root->data.tree.far = mktree_vp(tmp + nnear, nfar, max_linear);

    free(tmp);
    return root;
}

static size_t
query_vp(struct buf *restrict b, struct vptree *restrict root,
         bkey_t ref, unsigned maxd)
{
    /* We are trying to find x that satisfy d(ref,x) <= maxd
       By triangle inequality, we know: d(root,x) <= d(root,ref) + d(ref,x)
       By algebra: d(root,x) - d(root,ref) <= d(ref,x)
       By transitivity: d(root,x) - d(root,ref) <= maxd
       By algebra: d(root,x) <= maxd + d(root,ref) */
    if (root->linear) {
        const bkey_t *restrict keys = root->data.linear.keys;
        unsigned i, n = root->data.linear.count;
        for (i = 0; i < n; ++i)
            if (distance(ref, keys[i]) <= maxd)
                addkey(b, keys[i]);
        return n;
    } else {
        unsigned d = distance(root->data.tree.vantage, ref);
        unsigned thr = root->data.tree.threshold;
        size_t nc = 1;
        if (d <= maxd + thr) {
            if (root->data.tree.near)
                nc += query_vp(b, root->data.tree.near, ref, maxd);
            if (d <= maxd)
                addkey(b, root->data.tree.vantage);
        }
        if (d + maxd > thr && root->data.tree.far)
            nc += query_vp(b, root->data.tree.far, ref, maxd);
        return nc;
    }
}

/* Main ==================== */

typedef void *(*mktree_t)(bkey_t *, size_t, size_t);
typedef size_t (*query_t)(struct buf *, void *, bkey_t, unsigned);

int main(int argc, char *argv[])
{
    double tm, qc;
    clock_t ckref, t;
    struct buf q = { 0, 0, 0 };
    unsigned long nkeys, nquery, dist, i, j, k;
    void *root;
    bkey_t ref, *keys;
    unsigned long long total, totalcmp, maxlin;
    size_t nc;
    char *type;
    mktree_t mktree;
    query_t query;

    if (argc < 5) {
        fputs("Usage: TYPE MAXLIN NKEYS NQUERY DIST...\n", stderr);
        return 1;
    }
    type = argv[1];
    if (!strcasecmp(type, "bk")) {
        puts("Type: BK-tree");
        mktree = (mktree_t) mktree_bk;
        query = (query_t) query_bk;
    } else if (!strcasecmp(type, "vp")) {
        puts("Type: VP-tree");
        mktree = (mktree_t) mktree_vp;
        query = (query_t) query_vp;
    } else if (!strcasecmp(type, "linear")) {
        puts("Type: Linear search");
        mktree = (mktree_t) mktree_linear;
        query = (query_t) query_linear;
    } else {
        puts("Unknown type");
        return 1;
    }
    maxlin = xatoul(argv[2]);
    nkeys = xatoul(argv[3]);
    nquery = xatoul(argv[4]);
    if (!nkeys) {
        fputs("Need at least one key\n", stderr);
        return 1;
    }
    seedrand();
    printf("Keys: %lu\n", nkeys);
    printf("Queries: %lu\n", nquery);
    putchar('\n');

    puts("Generating keys...");
    keys = malloc(sizeof(*keys) * nkeys);
    for (i = 0; i < nkeys; ++i)
        keys[i] = irand();

    puts("Building tree...");
    ckref = clock();
    root = mktree(keys, nkeys, maxlin);
    free(keys);
    t = clock();
    printf("Time: %.3f sec\n",
           (double)(t - ckref) / CLOCKS_PER_SEC);
    printf("Nodes: %u\n", num_nodes);
    printf("Tree size: %zu\n", tree_size);

    for (k = 5; k < (unsigned) argc; ++k) {
        total = 0;
        totalcmp = 0;
        ckref = clock();
        dist = xatoul(argv[k]);
        if (dist >= MAX_DISTANCE || dist <= 0) {
            fprintf(stderr, "Distance should be in the range 1..%d\n",
                    MAX_DISTANCE);
            return 1;
        }
        putchar('\n');
        printf("Distance: %lu\n", dist);
        for (i = 0; i < nquery; ++i) {
            ref = irand();
            q.n = 0;
            nc = query(&q, root, ref, dist);
            totalcmp += nc;
            total += q.n;
            if (DO_PRINT) {
                printf("Query: %s\n", keystr(ref));
                for (j = 0; j < q.n; ++j)
                    printf("       %s\n", keystr2(q.keys[j], ref));
            }
        }
        t = clock();
        tm = t - ckref;
        qc = (double) CLOCKS_PER_SEC * (double) nquery;
        printf("Rate: %f query/sec\n", qc / tm);
        printf("Time: %f msec/query\n", 1000.0 * tm / qc);
        printf("Hits: %f\n", total / (double)nquery);
        printf("Coverage: %f%%\n",
               100.0 * (double)totalcmp / ((double)nkeys * nquery));
        printf("Cmp/result: %f\n", (double)totalcmp / (double)total);
    }
    return 0;
}
