# Metric tree sample implementation.

This was written in response to the Stack Overflow question,
[Efficiently find binary strings with low Hamming distance in large set][question]

[question]: http://stackoverflow.com/questions/6389841/efficiently-find-binary-strings-with-low-hamming-distance-in-large-set/6390606#6390606

This generates a bunch of pseudorandom 32-bit integers, inserts them
into an index, and queries the index for points within a certain
distance of the given point.

That is,

    Let S = { N pseudorandom 32-bit integers }
    Let d(x,y) be the (base-2) Hamming distance between x and y
    Let q(x,r) = { y in S : d(x,y) <= r }

There are three implementations in here which can be selected at
runtime.

"bk" is a BK-Tree.  Each internal node has a center point, and each
child node contains a set of all points a certain distance away from
the center.

"vp" is a VP-Tree.  Each internal node has a center point and two
children.  The "near" child contains all points contained in a closed
ball of a certain radius around the center, and the "far" node
contains all other points.

"linear" is a linear search.

The tree implementations use a linear search for leaf nodes.  The
maximum number of points in a leaf node is configurable at runtime and
this parameter will affect performance.  If the number is low, say 1,
then the memory usage of the tree implementations will skyrocket to
unreasonable levels: more than 24 bytes per element.  If the number is
high, say infinity, then the tree will degenerate to a linear search.

Note that VP trees are slightly faster than BK trees for this problem,
and neither tree implementation significantly outperforms linear
search (that is, by a factor of two or more) for large r (for r > 6,
it seems).

## Test results

Parameters:

* System: 3.2 GHz AMD Phenom II / 6 cores
* RAM: 4 GiB
* Database size: 100M points
* Results: Average # of query hits (very approximate)
* Speed: Number of queries per second
* Coverage: Average percentage of database examined per query
* Sample size: 1000 queries for distance 1..5, 100 for 6..10 and linear
* Max leaf size: 1000 points

Results:

                -- BK Tree --   -- VP Tree --	-- Linear --
    Dist	Results	Speed	Cov	Speed	Cov	Speed	Cov
    1	   0.90	3800	 0.048%	4200	 0.048%
    2	  11	 300	 0.68%	 330	 0.65%
    3	 130	  56	 3.8%	  63	 3.4%
    4	 970	  18	12%	  22	10%
    5	5700	   8.5	26%	  10	22%
    6	2.6e4	   5.2	42%	   6.0	37%
    7	1.1e5	   3.7	60%	   4.1	54%
    8	3.5e5	   3.0	74%	   3.2	70%
    9	1.0e6	   2.6	85%	   2.7	82%
    10	2.5e6	   2.3	91%	   2.4	90%
    any						2.2	100%

Above results were computed by:

    ./tree bk 1000 100000000 1000 1 2 3 4 5
    ./tree bk 100 100000000 1000 6 7 8 9 10
    ./tree vp 1000 100000000 1000 1 2 3 4 5
    ./tree vp 100 100000000 1000 6 7 8 9 10
    ./tree linear 1000 100000000 

Except "results", which was just grabbed from whatever was convenient.
It's just an evaluation of the binomial function, so no need to
generate it specially (and it's not accurate).

Conclusion: I think VP is faster than BK because, being "deeper"
rather than "shallower", it compares against more points rather than
using finer-grained comparisons against fewer points.  I suspect that
the differences are more extreme in higher dimensional spaces.

What is the correct maximum leaf node size?

    for n in 50 60 64 70 80 90
    do ./tree vp $n 100000000 1000 3 ; done | grep Rate

    50: Rate: 94.607379 query/sec
    60: Rate: 95.877277 query/sec
    64: Rate: 97.656250 query/sec
    70: Rate: 97.370983 query/sec
    80: Rate: 96.711799 query/sec
    90: Rate: 96.618357 query/sec

I had already narrowed it down to 10 <= N <= 100 by exponential
search.  The 64 was added after I saw the results for N*10.  I suspect
that 64 plays nicer with malloc than 70 does.

Answer: Allow up to 64 points per leaf node.

What is the speed with the new leaf size?

    ./tree vp 64 100000000 1000 1 2 3 4 5
    ./tree vp 64 100000000 100 6 7 8 9 10 11 12

Tree size: 426725132 (6.7% overhead)

    Dist	Speed	Cov	    Speedup	BK Speedup
    1	9100	 0.0088%    4200x
    2	 580	 0.18%	     270x
    3	  97	 1.2%	      45x
    4	  29	 4.3%	      13x
    5	  12	11%	       5.7x
    6	   6.6	21%	       3.0x	2.1x
    7	   4.1	34%	       1.9x	1.5x
    8	   2.9	50%	       1.3x	1.1x
    9	   2.3	64%	       1.0x	0.96x
    10	   1.9	77%	       0.87x	0.85x
    11	   1.7	87%	       0.75x	0.77x
    12	   1.5	93%	       0.67x	0.70x

Note: These answers computed with the original high precision numbers,
then rounded in the final step to two significant figures.
