/*
 * Copyright 2024 Tencent Inc.  All rights reserved.
 */

#pragma once

#include <vector>

#include "acl/acl.h"
#include "acl/acl_op_compiler.h"
#include "aclnn/acl_meta.h"

namespace llm_kernels {
namespace ascend {

// NOTE(karlluo): perform the same process as Pytorch, just change shape and stride
void Permute(const aclTensor* permute_input, void** permute_input_tensor_addr_ptr, aclTensor** permute_output,
             const std::vector<int64_t>& dims, aclrtStream& stream, void (*ws_func)(size_t, void**));

}  // namespace ascend
}  // namespace llm_kernels
