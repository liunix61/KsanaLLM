/* Copyright 2023 Tencent Inc.  All rights reserved.

==============================================================================*/

#include <unistd.h>

#include "ksana_llm/kernels/nvidia/kernel_wrapper.h"
#include "ksana_llm/layers/custom_all_reduce_sum_layer.h"
#include "ksana_llm/utils/logger.h"

namespace ksana_llm {

Status CustomAllReduceSumLayer::Init(const std::vector<std::any>& parameters, std::shared_ptr<Context> context,
                                     int rank) {
  context_ = context;
  rank_ = rank;
  int parameter_index = 0;
  void* meta = std::any_cast<void*>(parameters[parameter_index++]);
  buffer_ = std::any_cast<void*>(parameters[parameter_index++]);
  buffer_size_ = std::any_cast<size_t>(parameters[parameter_index++]);
  rank_data_ = std::any_cast<void*>(parameters[parameter_index++]);
  rank_data_sz_ = std::any_cast<size_t>(parameters[parameter_index++]);
  void* input = std::any_cast<void*>(parameters[parameter_index++]);
  int input_index = std::any_cast<int>(parameters[parameter_index++]);

  int tp_size = context_->GetTensorParallelSize();

  data_handles_ = context_->GetCustomAllReduceBuffers();
  data_handles_[rank_] = buffer_;

  metas_ = context_->GetCustomAllReduceMetas();
  metas_[rank_] = meta;

  // init inputs for custom reduce sum
  input_handles_ = context_->GetCustomAllReduceInputs(input_index);
  input_handles_[rank_] = input;

  for (int i = 0; i < tp_size; ++i) {
    if (i != rank_) {
      cudaMemPool_t mempool;
      cudaDeviceGetDefaultMemPool(&mempool, i);
      cudaMemAccessDesc desc = {};
      desc.location.type = cudaMemLocationTypeDevice;
      desc.location.id = rank_;
      desc.flags = cudaMemAccessFlagsProtReadWrite;
      cudaMemPoolSetAccess(mempool, &desc, 1);
    }
  }
  return Status();
}

Status CustomAllReduceSumLayer::Forward(const std::vector<Tensor>& input_tensors, std::vector<Tensor>& output_tensors) {
  cudaStream_t* stream;
  if (context_->IsRunContextDecodeAndDecodeSerially()) {
    stream = &(context_->GetComputeStreams()[rank_]);
  } else {
    stream = &(context_->GetNCCLStreams()[rank_]);
  }
  if (context_->GetTensorParallelSize() > 1) {
    void* input = input_tensors[0].GetPtr<void>();
    void* result = output_tensors[0].GetPtr<void>();
    int data_size = input_tensors[0].GetElementNumber();
    int tp_size = context_->GetTensorParallelSize();
    if (!is_init_) {
      CustomAllReduceInit(&reduce_op_, input, metas_, rank_data_, data_handles_, input_handles_, data_size,
                          rank_data_sz_, tp_size, rank_, *stream);
      is_init_ = true;
    }
    CustomAllReduceRun(reduce_op_, input, result, data_size, *stream);
  } else {
    void* src = input_tensors[0].GetPtr<void>();
    void* dst = output_tensors[0].GetPtr<void>();
    CUDA_CHECK(cudaMemcpyAsync(dst, src, input_tensors[0].GetTotalBytes(), cudaMemcpyDeviceToDevice, *stream));
  }
  output_tensors[0].shape = input_tensors[0].shape;
  output_tensors[0].dtype = input_tensors[0].dtype;
  return Status();
}
}  // namespace ksana_llm