/*
 * Copyright (C) 2021-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "query_ranges_to_vnodes.hh"

static inline
const dht::token& start_token(const dht::partition_range& r) {
    static const dht::token min_token = dht::minimum_token();
    return r.start() ? r.start()->value().token() : min_token;
}

static inline
const dht::token& end_token(const dht::partition_range& r) {
    static const dht::token max_token = dht::maximum_token();
    return r.end() ? r.end()->value().token() : max_token;
}

query_ranges_to_vnodes_generator::query_ranges_to_vnodes_generator(const locator::token_metadata_ptr tmptr, schema_ptr s, dht::partition_range_vector ranges, bool local) :
        _s(s), _ranges(std::move(ranges)), _i(_ranges.begin()), _local(local), _tmptr(std::move(tmptr)) {}

dht::partition_range_vector query_ranges_to_vnodes_generator::operator()(size_t n) {
    n = std::min(n, size_t(1024));

    dht::partition_range_vector result;
    result.reserve(n);
    while (_i != _ranges.end() && result.size() != n) {
        process_one_range(n, result);
    }
    return result;
}

bool query_ranges_to_vnodes_generator::empty() const {
    return _ranges.end() == _i;
}

/**
 * Compute all ranges we're going to query, in sorted order. Nodes can be replica destinations for many ranges,
 * so we need to restrict each scan to the specific range we want, or else we'd get duplicate results.
 */
void query_ranges_to_vnodes_generator::process_one_range(size_t n, dht::partition_range_vector& ranges) {
    dht::ring_position_comparator cmp(*_s);
    dht::partition_range& cr = *_i;

    auto get_remainder = [this, &cr] {
        _i++;
       return std::move(cr);
    };

    auto add_range = [&ranges] (dht::partition_range&& r) {
        ranges.emplace_back(std::move(r));
    };

    if (_local) { // if the range is local no need to divide to vnodes
        add_range(get_remainder());
        return;
    }

    // special case for bounds containing exactly 1 token
    if (start_token(cr) == end_token(cr)) {
        if (start_token(cr).is_minimum()) {
            _i++; // empty range? Move to the next one
            return;
        }
        add_range(get_remainder());
        return;
    }

    // divide the queryRange into pieces delimited by the ring
    auto ring_iter = _tmptr->ring_range(cr.start());
    for (const dht::token& upper_bound_token : ring_iter) {
        /*
         * remainder can be a range/bounds of token _or_ keys and we want to split it with a token:
         *   - if remainder is tokens, then we'll just split using the provided token.
         *   - if remainder is keys, we want to split using token.upperBoundKey. For instance, if remainder
         *     is [DK(10, 'foo'), DK(20, 'bar')], and we have 3 nodes with tokens 0, 15, 30. We want to
         *     split remainder to A=[DK(10, 'foo'), 15] and B=(15, DK(20, 'bar')]. But since we can't mix
         *     tokens and keys at the same time in a range, we uses 15.upperBoundKey() to have A include all
         *     keys having 15 as token and B include none of those (since that is what our node owns).
         * asSplitValue() abstracts that choice.
         */

        dht::ring_position split_point(upper_bound_token, dht::ring_position::token_bound::end);
        if (!cr.contains(split_point, cmp)) {
            break; // no more splits
        }


        // We shouldn't attempt to split on upper bound, because it may result in
        // an ambiguous range of the form (x; x]
        if (end_token(cr) == upper_bound_token) {
            break;
        }

        std::pair<dht::partition_range, dht::partition_range> splits =
                cr.split(split_point, cmp);

        add_range(std::move(splits.first));
        cr = std::move(splits.second);

        // The left bound of cr is bound({upper_bound_token, token_bound::end}, false),
        // so we can end up with an empty range if the original end bound is its successor.
        const bool is_empty = cr.end().has_value() &&
            !cr.end()->is_inclusive() &&
            !cr.end()->value().has_key() &&
            cr.end()->value().bound() == dht::ring_position::token_bound::start &&
            dht::token::to_int64(cr.end()->value().token()) - dht::token::to_int64(cr.start()->value().token()) == 1;
        if (is_empty) {
            _i++;
            return;
        }
        if (ranges.size() == n) {
            // we have enough ranges
            break;
        }
    }

    if (ranges.size() < n) {
        add_range(get_remainder());
    }
}
