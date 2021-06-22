/*
 * Copyright (C) 2015-present ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "mutation.hh"
#include "sstables.hh"
#include "types.hh"
#include <seastar/core/future-util.hh>
#include <seastar/core/coroutine.hh>
#include "key.hh"
#include "keys.hh"
#include <seastar/core/do_with.hh>
#include "unimplemented.hh"
#include "dht/i_partitioner.hh"
#include <seastar/core/byteorder.hh>
#include "index_reader.hh"
#include "counters.hh"
#include "utils/data_input.hh"
#include "clustering_ranges_walker.hh"
#include "binary_search.hh"
#include "../dht/i_partitioner.hh"
#include "flat_mutation_reader_v2.hh"

namespace sstables {

namespace kl {
    class mp_row_consumer_k_l;
}

namespace mx {
    class mp_row_consumer_m;
}

class mp_row_consumer_reader_base {
protected:
    shared_sstable _sst;

    // Whether index lower bound is in current partition
    bool _index_in_current_partition = false;

    // True iff the consumer finished generating fragments for a partition and hasn't
    // entered the new partition yet.
    // Implies that partition_end was emitted for the last partition.
    // Will cause the reader to skip to the next partition if !_before_partition.
    bool _partition_finished = true;

    // When set, the consumer is positioned right before a partition or at end of the data file.
    // _index_in_current_partition applies to the partition which is about to be read.
    bool _before_partition = true;

    std::optional<dht::decorated_key> _current_partition_key;
public:
    mp_row_consumer_reader_base(shared_sstable sst)
        : _sst(std::move(sst))
    { }

    // Called when all fragments relevant to the query range or fast forwarding window
    // within the current partition have been pushed.
    // If no skipping is required, this method may not be called before transitioning
    // to the next partition.
    virtual void on_out_of_clustering_range() = 0;
};

class mp_row_consumer_reader_k_l : public mp_row_consumer_reader_base, public flat_mutation_reader::impl {
    friend class sstables::kl::mp_row_consumer_k_l;
public:
    mp_row_consumer_reader_k_l(schema_ptr s, reader_permit permit, shared_sstable sst)
        : mp_row_consumer_reader_base(std::move(sst))
        , impl(std::move(s), std::move(permit))
    {}

    void on_next_partition(dht::decorated_key, tombstone);
};

inline atomic_cell make_atomic_cell(const abstract_type& type,
                                    api::timestamp_type timestamp,
                                    fragmented_temporary_buffer::view value,
                                    gc_clock::duration ttl,
                                    gc_clock::time_point expiration,
                                    atomic_cell::collection_member cm) {
    if (ttl != gc_clock::duration::zero()) {
        return atomic_cell::make_live(type, timestamp, value, expiration, ttl, cm);
    } else {
        return atomic_cell::make_live(type, timestamp, value, cm);
    }
}

atomic_cell make_counter_cell(api::timestamp_type timestamp, fragmented_temporary_buffer::view value);

position_in_partition_view get_slice_upper_bound(const schema& s, const query::partition_slice& slice, dht::ring_position_view key);

// data_consume_rows() iterates over rows in the data file from
// a particular range, feeding them into the consumer. The iteration is
// done as efficiently as possible - reading only the data file (not the
// summary or index files) and reading data in batches.
//
// The consumer object may request the iteration to stop before reaching
// the end of the requested data range (e.g. stop after each sstable row).
// A context object is returned which allows to resume this consumption:
// This context's read() method requests that consumption begins, and
// returns a future which will be resolved when it ends (because the
// consumer asked to stop, or the data range ended). Only after the
// returned future is resolved, may read() be called again to consume
// more.
// The caller must ensure (e.g., using do_with()) that the context object,
// as well as the sstable, remains alive as long as a read() is in
// progress (i.e., returned a future which hasn't completed yet).
//
// The "toread" range specifies the range we want to read initially.
// However, the object returned by the read, a data_consume_context, also
// provides a fast_forward_to(start,end) method which allows resetting
// the reader to a new range. To allow that, we also have a "last_end"
// byte which should be the last end to which fast_forward_to is
// eventually allowed. If last_end==end, fast_forward_to is not allowed
// at all, if last_end==file_size fast_forward_to is allowed until the
// end of the file, and it can be something in between if we know that we
// are planning to skip parts, but eventually read until last_end.
// When last_end==end, we guarantee that the read will only read the
// desired byte range from disk. However, when last_end > end, we may
// read beyond end in anticipation of a small skip via fast_foward_to.
// The amount of this excessive read is controlled by read ahead
// hueristics which learn from the usefulness of previous read aheads.
template <typename DataConsumeRowsContext>
inline std::unique_ptr<DataConsumeRowsContext> data_consume_rows(const schema& s, shared_sstable sst, typename DataConsumeRowsContext::consumer& consumer, sstable::disk_read_range toread, uint64_t last_end) {
    // Although we were only asked to read until toread.end, we'll not limit
    // the underlying file input stream to this end, but rather to last_end.
    // This potentially enables read-ahead beyond end, until last_end, which
    // can be beneficial if the user wants to fast_forward_to() on the
    // returned context, and may make small skips.
    auto input = sst->data_stream(toread.start, last_end - toread.start, consumer.io_priority(),
            consumer.permit(), consumer.trace_state(), sst->_partition_range_history);
    return std::make_unique<DataConsumeRowsContext>(s, std::move(sst), consumer, std::move(input), toread.start, toread.end - toread.start);
}

template <typename DataConsumeRowsContext>
inline std::unique_ptr<DataConsumeRowsContext> data_consume_single_partition(const schema& s, shared_sstable sst, typename DataConsumeRowsContext::consumer& consumer, sstable::disk_read_range toread) {
    auto input = sst->data_stream(toread.start, toread.end - toread.start, consumer.io_priority(),
            consumer.permit(), consumer.trace_state(), sst->_single_partition_history);
    return std::make_unique<DataConsumeRowsContext>(s, std::move(sst), consumer, std::move(input), toread.start, toread.end - toread.start);
}

// Like data_consume_rows() with bounds, but iterates over whole range
template <typename DataConsumeRowsContext>
inline std::unique_ptr<DataConsumeRowsContext> data_consume_rows(const schema& s, shared_sstable sst, typename DataConsumeRowsContext::consumer& consumer) {
        auto data_size = sst->data_size();
        return data_consume_rows<DataConsumeRowsContext>(s, std::move(sst), consumer, {0, data_size}, data_size);
}

template<typename T>
concept RowConsumer =
    requires(T t,
                    const partition_key& pk,
                    position_range cr,
                    db::timeout_clock::time_point timeout) {
        { t.io_priority() } -> std::convertible_to<const io_priority_class&>;
        { t.is_mutation_end() } -> std::same_as<bool>;
        { t.setup_for_partition(pk) } -> std::same_as<void>;
        { t.push_ready_fragments() } -> std::same_as<void>;
        { t.maybe_skip() } -> std::same_as<std::optional<position_in_partition_view>>;
        { t.fast_forward_to(std::move(cr), timeout) } -> std::same_as<std::optional<position_in_partition_view>>;
    };

/*
 * Helper method to set or reset the range tombstone start bound according to the
 * end open marker of a promoted index block.
 *
 * Only applies to consumers that have the following methods:
 *      void reset_range_tombstone_start();
 *      void set_range_tombstone_start(clustering_key_prefix, bound_kind, tombstone);
 *
 * For other consumers, it is a no-op.
 */
template <typename Consumer>
void set_range_tombstone_start_from_end_open_marker(Consumer& c, const schema& s, const index_reader& idx) {
    if constexpr (Consumer::is_setting_range_tombstone_start_supported) {
        auto open_end_marker = idx.end_open_marker();
        if (open_end_marker) {
            auto[pos, tomb] = *open_end_marker;
            c.set_range_tombstone_start(tomb);
        }
    }
}

}