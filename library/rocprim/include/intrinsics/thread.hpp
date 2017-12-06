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

#ifndef ROCPRIM_INTRINSICS_THREAD_HPP_
#define ROCPRIM_INTRINSICS_THREAD_HPP_

// HC API
#include <hcc/hc.hpp>

#include "../detail/config.hpp"

BEGIN_ROCPRIM_NAMESPACE

/// \brief Returns number of threads in a warp.
constexpr unsigned int warp_size() [[hc]] [[cpu]]
{
    // Using marco allows contexpr, but we may have to
    // change it to hc::__wavesize() for safety
    return __HSA_WAVEFRONT_SIZE__;
    // return hc::__wavesize();
}

/// \brief Returns thread id in a warp.
inline unsigned int lane_id() [[hc]]
{
    return hc::__lane_id();
}

namespace detail
{

inline unsigned int flat_block_size() [[hc]]
{
    return hc_get_group_size(2) * hc_get_group_size(1) * hc_get_group_size(0);
}

inline unsigned int flat_thread_id() [[hc]]
{
    return (hc_get_group_size(2) * hc_get_group_size(1) * hc_get_workitem_id(2))
        + (hc_get_group_size(0) * hc_get_workitem_id(1))
        + hc_get_workitem_id(0);
}

inline unsigned int warp_id() [[hc]]
{
    return flat_thread_id()/::rocprim::warp_size();
}

inline void sync_all_threads() [[hc]]
{
    hc_barrier(CLK_LOCAL_MEM_FENCE);
}

} // end detail namespace

END_ROCPRIM_NAMESPACE

#endif // ROCPRIM_INTRINSICS_THREAD_HPP_
