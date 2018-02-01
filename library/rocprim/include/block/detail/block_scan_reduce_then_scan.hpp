// Copyright (c) 2017 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef ROCPRIM_BLOCK_DETAIL_BLOCK_SCAN_REDUCE_THEN_SCAN_HPP_
#define ROCPRIM_BLOCK_DETAIL_BLOCK_SCAN_REDUCE_THEN_SCAN_HPP_

#include <type_traits>

// HC API
#include <hcc/hc.hpp>

#include "../../detail/config.hpp"
#include "../../detail/various.hpp"

#include "../../intrinsics.hpp"
#include "../../functional.hpp"

#include "../../warp/warp_scan.hpp"

BEGIN_ROCPRIM_NAMESPACE

namespace detail
{

template<
    class T,
    unsigned int BlockSize
>
class block_scan_reduce_then_scan
{
    // Number of items to reduce per thread
    static constexpr unsigned int thread_reduction_size_ =
        (BlockSize + ::rocprim::warp_size() - 1)/ ::rocprim::warp_size();

    // Warp scan, warp_scan_shuffle does not require shared memory (storage), but
    // logical warp size must be a power of two.
    static constexpr unsigned int warp_size_ =
        detail::get_min_warp_size(BlockSize, ::rocprim::warp_size());
    using warp_scan_prefix_type = ::rocprim::detail::warp_scan_shuffle<T, warp_size_>;

    // Minimize LDS bank conflicts
    static constexpr unsigned int banks_no_ = ::rocprim::detail::get_lds_banks_no();
    static constexpr bool has_bank_conflicts_ =
        ::rocprim::detail::is_power_of_two(thread_reduction_size_) && thread_reduction_size_ > 1;
    static constexpr unsigned int bank_conflicts_padding =
        has_bank_conflicts_ ? (warp_size_ * thread_reduction_size_ / banks_no_) : 0;

public:

    struct storage_type
    {
        T threads[warp_size_ * thread_reduction_size_ + bank_conflicts_padding];
    };

    template<class BinaryFunction>
    void inclusive_scan(T input,
                        T& output,
                        storage_type& storage,
                        BinaryFunction scan_op) [[hc]]
    {
        const auto flat_tid = ::rocprim::flat_block_thread_id();
        this->inclusive_scan_impl(flat_tid, input, output, storage, scan_op);
    }

    template<class BinaryFunction>
    void inclusive_scan(T input,
                        T& output,
                        BinaryFunction scan_op) [[hc]]
    {
        tile_static storage_type storage;
        this->inclusive_scan(input, output, storage, scan_op);
    }

    template<class BinaryFunction>
    void inclusive_scan(T input,
                        T& output,
                        T& reduction,
                        storage_type& storage,
                        BinaryFunction scan_op) [[hc]]
    {
        this->inclusive_scan(input, output, storage, scan_op);
        reduction = storage.threads[index(BlockSize - 1)];
    }

    template<class BinaryFunction>
    void inclusive_scan(T input,
                        T& output,
                        T& reduction,
                        BinaryFunction scan_op) [[hc]]
    {
        tile_static storage_type storage;
        this->inclusive_scan(input, output, reduction, storage, scan_op);
    }

    template<class PrefixCallback, class BinaryFunction>
    void inclusive_scan(T input,
                        T& output,
                        storage_type& storage,
                        PrefixCallback& prefix_callback_op,
                        BinaryFunction scan_op) [[hc]]
    {
        const auto flat_tid = ::rocprim::flat_block_thread_id();
        const auto warp_id = ::rocprim::warp_id();
        this->inclusive_scan_impl(flat_tid, input, output, storage, scan_op);
        // Include block prefix (this operation overwrites storage.threads[0])
        T block_prefix = this->get_block_prefix(
            flat_tid, warp_id,
            storage.threads[index(BlockSize - 1)], // block reduction
            prefix_callback_op, storage
        );
        output = scan_op(block_prefix, output);
    }

    template<unsigned int ItemsPerThread, class BinaryFunction>
    void inclusive_scan(T (&input)[ItemsPerThread],
                        T (&output)[ItemsPerThread],
                        storage_type& storage,
                        BinaryFunction scan_op) [[hc]]
    {
        // Reduce thread items
        T thread_input = input[0];
        #pragma unroll
        for(unsigned int i = 1; i < ItemsPerThread; i++)
        {
            thread_input = scan_op(thread_input, input[i]);
        }

        // Scan of reduced values to get prefixes
        const auto flat_tid = ::rocprim::flat_block_thread_id();
        this->exclusive_scan_impl(
            flat_tid,
            thread_input, thread_input, // input, output
            storage,
            scan_op
        );

        // Include prefix (first thread does not have prefix)
        output[0] = flat_tid == 0 ? input[0] : scan_op(thread_input, input[0]);
        // Final thread-local scan
        #pragma unroll
        for(unsigned int i = 1; i < ItemsPerThread; i++)
        {
            output[i] = scan_op(output[i-1], input[i]);
        }
    }

    template<unsigned int ItemsPerThread, class BinaryFunction>
    void inclusive_scan(T (&input)[ItemsPerThread],
                        T (&output)[ItemsPerThread],
                        BinaryFunction scan_op) [[hc]]
    {
        tile_static storage_type storage;
        this->inclusive_scan(input, output, storage, scan_op);
    }

    template<unsigned int ItemsPerThread, class BinaryFunction>
    void inclusive_scan(T (&input)[ItemsPerThread],
                        T (&output)[ItemsPerThread],
                        T& reduction,
                        storage_type& storage,
                        BinaryFunction scan_op) [[hc]]
    {
        this->inclusive_scan(input, output, storage, scan_op);
        // Save reduction result
        reduction = storage.threads[index(BlockSize - 1)];
    }

    template<unsigned int ItemsPerThread, class BinaryFunction>
    void inclusive_scan(T (&input)[ItemsPerThread],
                        T (&output)[ItemsPerThread],
                        T& reduction,
                        BinaryFunction scan_op) [[hc]]
    {
        tile_static storage_type storage;
        this->inclusive_scan(input, output, reduction, storage, scan_op);
    }

    template<
        class PrefixCallback,
        unsigned int ItemsPerThread,
        class BinaryFunction
    >
    void inclusive_scan(T (&input)[ItemsPerThread],
                        T (&output)[ItemsPerThread],
                        storage_type& storage,
                        PrefixCallback& prefix_callback_op,
                        BinaryFunction scan_op) [[hc]]
    {
        // Reduce thread items
        T thread_input = input[0];
        #pragma unroll
        for(unsigned int i = 1; i < ItemsPerThread; i++)
        {
            thread_input = scan_op(thread_input, input[i]);
        }

        // Scan of reduced values to get prefixes
        const auto flat_tid = ::rocprim::flat_block_thread_id();
        this->exclusive_scan_impl(
            flat_tid,
            thread_input, thread_input, // input, output
            storage,
            scan_op
        );

        // this operation overwrites storage.threads[0]
        T block_prefix = this->get_block_prefix(
            flat_tid, ::rocprim::warp_id(),
            storage.threads[index(BlockSize - 1)], // block reduction
            prefix_callback_op, storage
        );

        // Include prefix (first thread does not have prefix)
        output[0] = flat_tid == 0 ? input[0] : scan_op(thread_input, input[0]);
        // Include block prefix
        output[0] = scan_op(block_prefix, output[0]);
        // Final thread-local scan
        #pragma unroll
        for(unsigned int i = 1; i < ItemsPerThread; i++)
        {
            output[i] = scan_op(output[i-1], input[i]);
        }
    }

    template<class BinaryFunction>
    void exclusive_scan(T input,
                        T& output,
                        T init,
                        storage_type& storage,
                        BinaryFunction scan_op) [[hc]]
    {
        const auto flat_tid = ::rocprim::flat_block_thread_id();
        this->exclusive_scan_impl(flat_tid, input, output, init, storage, scan_op);
    }

    template<class BinaryFunction>
    void exclusive_scan(T input,
                        T& output,
                        T init,
                        BinaryFunction scan_op) [[hc]]
    {
        tile_static storage_type storage;
        this->exclusive_scan(input, output, init, storage, scan_op);
    }

    template<class BinaryFunction>
    void exclusive_scan(T input,
                        T& output,
                        T init,
                        T& reduction,
                        storage_type& storage,
                        BinaryFunction scan_op) [[hc]]
    {
        const auto flat_tid = ::rocprim::flat_block_thread_id();
        this->exclusive_scan_impl(
            flat_tid, input, output, init, storage, scan_op
        );
        // Save reduction result
        reduction = storage.threads[index(BlockSize - 1)];
    }

    template<class BinaryFunction>
    void exclusive_scan(T input,
                        T& output,
                        T init,
                        T& reduction,
                        BinaryFunction scan_op) [[hc]]
    {
        tile_static storage_type storage;
        this->exclusive_scan(input, output, init, reduction, storage, scan_op);
    }

    template<class PrefixCallback, class BinaryFunction>
    void exclusive_scan(T input,
                        T& output,
                        storage_type& storage,
                        PrefixCallback& prefix_callback_op,
                        BinaryFunction scan_op) [[hc]]
    {
        const auto flat_tid = ::rocprim::flat_block_thread_id();
        const auto warp_id = ::rocprim::warp_id();
        this->exclusive_scan_impl(
            flat_tid, input, output, storage, scan_op
        );
        // Get reduction result
        T reduction = storage.threads[index(BlockSize - 1)];
        // Include block prefix (this operation overwrites storage.threads[0])
        T block_prefix = this->get_block_prefix(
            flat_tid, warp_id, reduction,
            prefix_callback_op, storage
        );
        output = flat_tid == 0 ? block_prefix : scan_op(block_prefix, output);
    }

    template<unsigned int ItemsPerThread, class BinaryFunction>
    void exclusive_scan(T (&input)[ItemsPerThread],
                        T (&output)[ItemsPerThread],
                        T init,
                        storage_type& storage,
                        BinaryFunction scan_op) [[hc]]
    {
        // Reduce thread items
        T thread_input = input[0];
        #pragma unroll
        for(unsigned int i = 1; i < ItemsPerThread; i++)
        {
            thread_input = scan_op(thread_input, input[i]);
        }

        // Scan of reduced values to get prefixes
        const auto flat_tid = ::rocprim::flat_block_thread_id();
        this->exclusive_scan_impl(
            flat_tid,
            thread_input, thread_input, // input, output
            init,
            storage,
            scan_op
        );

        // Include init value
        T prev = input[0];
        T exclusive = flat_tid == 0 ? init : thread_input;
        output[0] = exclusive;
        #pragma unroll
        for(unsigned int i = 1; i < ItemsPerThread; i++)
        {
            exclusive = scan_op(exclusive, prev);
            prev = input[i];
            output[i] = exclusive;
        }
    }

    template<unsigned int ItemsPerThread, class BinaryFunction>
    void exclusive_scan(T (&input)[ItemsPerThread],
                        T (&output)[ItemsPerThread],
                        T init,
                        BinaryFunction scan_op) [[hc]]
    {
        tile_static storage_type storage;
        this->exclusive_scan(input, output, init, storage, scan_op);
    }

    template<unsigned int ItemsPerThread, class BinaryFunction>
    void exclusive_scan(T (&input)[ItemsPerThread],
                        T (&output)[ItemsPerThread],
                        T init,
                        T& reduction,
                        storage_type& storage,
                        BinaryFunction scan_op) [[hc]]
    {
        this->exclusive_scan(input, output, init, storage, scan_op);
        // Save reduction result
        reduction = storage.threads[index(BlockSize - 1)];
    }

    template<unsigned int ItemsPerThread, class BinaryFunction>
    void exclusive_scan(T (&input)[ItemsPerThread],
                        T (&output)[ItemsPerThread],
                        T init,
                        T& reduction,
                        BinaryFunction scan_op) [[hc]]
    {
        tile_static storage_type storage;
        this->exclusive_scan(input, output, init, reduction, storage, scan_op);
    }

    template<
        class PrefixCallback,
        unsigned int ItemsPerThread,
        class BinaryFunction
    >
    void exclusive_scan(T (&input)[ItemsPerThread],
                        T (&output)[ItemsPerThread],
                        storage_type& storage,
                        PrefixCallback& prefix_callback_op,
                        BinaryFunction scan_op) [[hc]]
    {
        // Reduce thread items
        T thread_input = input[0];
        #pragma unroll
        for(unsigned int i = 1; i < ItemsPerThread; i++)
        {
            thread_input = scan_op(thread_input, input[i]);
        }

        // Scan of reduced values to get prefixes
        const auto flat_tid = ::rocprim::flat_block_thread_id();
        this->exclusive_scan_impl(
            flat_tid,
            thread_input, thread_input, // input, output
            storage,
            scan_op
        );

        // this operation overwrites storage.warp_prefixes[0]
        T block_prefix = this->get_block_prefix(
            flat_tid, ::rocprim::warp_id(),
            storage.threads[index(BlockSize - 1)], // block reduction
            prefix_callback_op, storage
        );

        // Include init value and block prefix
        T prev = input[0];
        T exclusive = flat_tid == 0 ? block_prefix : scan_op(block_prefix, thread_input);
        output[0] = exclusive;
        #pragma unroll
        for(unsigned int i = 1; i < ItemsPerThread; i++)
        {
            exclusive = scan_op(exclusive, prev);
            prev = input[i];
            output[i] = exclusive;
        }
    }

private:

    // Calculates inclusive scan results and stores them in storage.threads,
    // result for each thread is stored in storage.threads[flat_tid], and sets
    // output to storage.threads[flat_tid]
    template<class BinaryFunction>
    void inclusive_scan_impl(const unsigned int flat_tid,
                             T input,
                             T& output,
                             storage_type& storage,
                             BinaryFunction scan_op) [[hc]]
    {
        // Calculate inclusive scan,
        // result for each thread is stored in storage.threads[flat_tid]
        this->inclusive_scan_base(flat_tid, input, storage, scan_op);
        output = storage.threads[index(flat_tid)];
    }

    // Calculates inclusive scan results and stores them in storage.threads,
    // result for each thread is stored in storage.threads[flat_tid]
    template<class BinaryFunction>
    void inclusive_scan_base(const unsigned int flat_tid,
                             T input,
                             storage_type& storage,
                             BinaryFunction scan_op) [[hc]]
    {
        storage.threads[index(flat_tid)] = input;
        ::rocprim::syncthreads();
        if(flat_tid < warp_size_)
        {
            const unsigned int idx_start = index(flat_tid * thread_reduction_size_);
            const unsigned int idx_end = idx_start + thread_reduction_size_;

            T thread_reduction = storage.threads[idx_start];
            #pragma unroll
            for(unsigned int i = idx_start + 1; i < idx_end; i++)
            {
                thread_reduction = scan_op(
                    thread_reduction, storage.threads[i]
                );
            }

            // Calculate warp prefixes
            warp_scan_prefix_type().inclusive_scan(thread_reduction, thread_reduction, scan_op);
            thread_reduction = warp_shuffle_up(thread_reduction, 1, warp_size_);

            // Include warp prefix
            thread_reduction =
                flat_tid == 0 ?
                    input : scan_op(thread_reduction, storage.threads[idx_start]);

            storage.threads[idx_start] = thread_reduction;
            #pragma unroll
            for(unsigned int i = idx_start + 1; i < idx_end; i++)
            {
                thread_reduction = scan_op(
                    thread_reduction, storage.threads[i]
                );
                storage.threads[i] = thread_reduction;
            }
        }
        ::rocprim::syncthreads();
    }

    template<class BinaryFunction>
    void exclusive_scan_impl(const unsigned int flat_tid,
                             T input,
                             T& output,
                             T init,
                             storage_type& storage,
                             BinaryFunction scan_op) [[hc]]
    {
        // Calculates inclusive scan, result for each thread is stored in storage.threads[flat_tid]
        this->inclusive_scan_base(flat_tid, input, storage, scan_op);
        output = flat_tid == 0 ?
            init : scan_op(init, storage.threads[index(flat_tid-1)]);
    }

    template<class BinaryFunction>
    void exclusive_scan_impl(const unsigned int flat_tid,
                             T input,
                             T& output,
                             storage_type& storage,
                             BinaryFunction scan_op) [[hc]]
    {
        // Calculates inclusive scan, result for each thread is stored in storage.threads[flat_tid]
        this->inclusive_scan_base(flat_tid, input, storage, scan_op);
        if(flat_tid > 0)
        {
            output = storage.threads[index(flat_tid-1)];
        }
    }

    // OVERWRITES storage.threads[0]
    template<class PrefixCallback, class BinaryFunction>
    void include_block_prefix(const unsigned int flat_tid,
                              const unsigned int warp_id,
                              const T input,
                              T& output,
                              const T reduction,
                              PrefixCallback& prefix_callback_op,
                              storage_type& storage,
                              BinaryFunction scan_op) [[hc]]
    {
        T block_prefix = this->get_block_prefix(
            flat_tid, warp_id, reduction,
            prefix_callback_op, storage
        );
        output = scan_op(block_prefix, input);
    }

    // OVERWRITES storage.threads[0]
    template<class PrefixCallback>
    T get_block_prefix(const unsigned int flat_tid,
                       const unsigned int warp_id,
                       const T reduction,
                       PrefixCallback& prefix_callback_op,
                       storage_type& storage) [[hc]]
    {
        if(warp_id == 0)
        {
            T block_prefix = prefix_callback_op(reduction);
            if(flat_tid == 0)
            {
                // Reuse storage.threads[0] which should not be
                // needed at that point.
                storage.threads[0] = block_prefix;
            }
        }
        ::rocprim::syncthreads();
        return storage.threads[0];
    }

    // Change index to minimize LDS bank conflicts if necessary
    unsigned int index(unsigned int n) const [[hc]]
    {
        // Move every 32-bank wide "row" (32 banks * 4 bytes) by one item
        return has_bank_conflicts_ ? (n + (n/banks_no_)) : n;
    }
};

} // end namespace detail

END_ROCPRIM_NAMESPACE

#endif // ROCPRIM_BLOCK_DETAIL_BLOCK_SCAN_REDUCE_THEN_SCAN_HPP_
