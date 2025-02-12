/*
 * Copyright 2024 Tencent Inc.  All rights reserved.
 */

#pragma once

#include "acl/acl.h"
#include "acl/acl_op_compiler.h"
#include "aclnn/acl_meta.h"

namespace llm_kernels {
namespace ascend {

aclError MatMul(const aclTensor* input, const aclTensor* weight, const int8_t matmulCubeMathType, aclTensor** output,
                aclrtStream& stream, void (*ws_func)(size_t, void**));

}  // namespace ascend
}  // namespace llm_kernels
