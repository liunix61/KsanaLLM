/* Copyright 2023 Tencent Inc.  All rights reserved.

==============================================================================*/

#include "ksana_llm/block_manager/block_manager.h"

#include <memory>
#include <string>

#include "ATen/core/interned_strings.h"
#include "ksana_llm/block_manager/host_allocator.h"
#include "ksana_llm/block_manager/nvidia_allocator.h"
#include "ksana_llm/utils/logger.h"
#include "ksana_llm/utils/memory_utils.h"
#include "ksana_llm/utils/status.h"

namespace ksana_llm {

BlockManager::BlockManager(const BlockManagerConfig& block_manager_config, std::shared_ptr<Context> context)
    : block_manager_config_(block_manager_config), context_(context) {
  NLLM_CHECK_WITH_INFO(
      block_manager_config.device_allocator_config.block_size == block_manager_config.host_allocator_config.block_size,
      "The block size of host and device must be equal.");
  // Create host allocator
  host_allocator_ = std::make_shared<HostAllocator>(block_manager_config.host_allocator_config, context);

  // Create device allocator for every device.
  for (int worker_id = 0; worker_id < context_->GetTensorParallelSize(); ++worker_id) {
    std::shared_ptr<NvidiaDeviceAllocator> device_allocator =
        std::make_shared<NvidiaDeviceAllocator>(block_manager_config.device_allocator_config, context, worker_id);
    device_allocators_.push_back(device_allocator);
  }
}

Status BlockManager::PreAllocateBlocks() {
  host_allocator_->ResetPreAllocatedBlocks(block_manager_config_.host_allocator_config.blocks_num);
  for (auto& allocator : device_allocators_) {
    allocator->ResetPreAllocatedBlocks(block_manager_config_.device_allocator_config.blocks_num);
  }

  return Status();
}

Status BlockManager::ResetPreAllocatedBlocks() {
  size_t host_block_num;
  size_t device_blocks_num;

  Status status = CalculateBlockNumber(device_blocks_num, host_block_num);
  if (!status.OK()) {
    NLLM_LOG_ERROR << "Calculate block num error." << std::endl;
    return status;
  }

  NLLM_LOG_INFO << "Reset device_blocks_num:" << device_blocks_num << ", host_block_num:" << host_block_num
                << std::endl;

  host_allocator_->ResetPreAllocatedBlocks(host_block_num);
  for (auto& allocator : device_allocators_) {
    allocator->ResetPreAllocatedBlocks(device_blocks_num);
  }

  return Status();
}

Status BlockManager::CalculateBlockNumber(size_t& device_blocks_num, size_t& host_block_num) {
  size_t host_total, host_free;
  size_t device_total, device_free;

  Status status = GetDeviceMemoryInfo(&device_free, &device_total);
  if (!status.OK()) {
    return status;
  }

  status = GetHostMemoryInfo(&host_free, &host_total);
  if (!status.OK()) {
    return status;
  }

  NLLM_LOG_INFO << "Get memory info, host_total:" << host_total << ", host_free:" << host_free
                << ", device_total:" << device_total << ", device_free:" << device_free;

  NLLM_CHECK_WITH_INFO(block_manager_config_.reserved_device_memory_ratio > 0.0,
                       "reserved_device_memory_ratio must be large than 0.0");
  NLLM_CHECK_WITH_INFO(block_manager_config_.lora_host_memory_factor > 1.0,
                       "lora_host_memory_factor should large than 1.0");
  NLLM_CHECK_WITH_INFO(block_manager_config_.block_host_memory_factor > 1.0,
                       "block_host_memory_factor should large than 1.0");

  size_t alignment_bytes = 8;
  size_t device_block_memory_size = 0;
  if (block_manager_config_.block_device_memory_ratio >= 0.0) {
    device_block_memory_size = (device_total * block_manager_config_.block_device_memory_ratio) / alignment_bytes;
  } else {
    size_t reserved_memory_size =
        ((device_total * block_manager_config_.reserved_device_memory_ratio) / alignment_bytes + 1) * alignment_bytes;
    device_block_memory_size = ((device_free - reserved_memory_size) / alignment_bytes + 1) * alignment_bytes;
  }

  device_blocks_num = device_block_memory_size / block_manager_config_.device_allocator_config.block_size;
  host_block_num = device_blocks_num * block_manager_config_.block_host_memory_factor;
  NLLM_CHECK_WITH_INFO(host_block_num * block_manager_config_.host_allocator_config.block_size < host_free,
                       "Not enough host free memory");

  return Status();
}

void BlockManager::SetDeviceId(int device_id) { CUDA_CHECK(cudaSetDevice(device_id)); }

int BlockManager::GetDeviceId() {
  int device_id;
  CUDA_CHECK(cudaGetDevice(&device_id));
  return device_id;
}

std::shared_ptr<DeviceAllocator>& BlockManager::GetDeviceAllocator() {
  int device_id = GetDeviceId();
  NLLM_CHECK_WITH_INFO(device_id < device_allocators_.size(), "Invalid device id " + std::to_string(device_id));
  return device_allocators_[device_id];
}

std::shared_ptr<HostAllocator>& BlockManager::GetHostAllocator() { return host_allocator_; }

Status BlockManager::AllocateBlocks(int64_t block_num, std::vector<int>& blocks) {
  return GetDeviceAllocator()->AllocateBlocks(block_num, blocks);
}

Status BlockManager::AllocateContiguous(int64_t size, int& block_id) {
  return GetDeviceAllocator()->AllocateContiguous(size, block_id);
}

Status BlockManager::FreeBlocks(const std::vector<int>& blocks) { return GetDeviceAllocator()->FreeBlocks(blocks); }

Status BlockManager::FreeContiguous(int block_id) { return GetDeviceAllocator()->FreeContiguous(block_id); }

Status BlockManager::GetBlockPtrs(const std::vector<int>& blocks, std::vector<void*>& addrs) {
  return GetDeviceAllocator()->GetBlockPtrs(blocks, addrs);
}

Status BlockManager::GetContiguousPtr(int block_id, void*& addr) {
  return GetDeviceAllocator()->GetContiguousPtr(block_id, addr);
}

int BlockManager::GetFreeBlockNumber() { return GetDeviceAllocator()->GetFreeBlockNumber(); }

int BlockManager::GetUsedBlockNumber() { return GetDeviceAllocator()->GetUsedBlockNumber(); }

Status BlockManager::AllocateHostBlocks(int64_t block_num, std::vector<int>& blocks) {
  return GetHostAllocator()->AllocateBlocks(block_num, blocks);
}

Status BlockManager::AllocateHostContiguous(int64_t size, int& block_id) {
  return GetHostAllocator()->AllocateContiguous(size, block_id);
}

Status BlockManager::FreeHostBlocks(const std::vector<int>& blocks) { return GetHostAllocator()->FreeBlocks(blocks); }

Status BlockManager::FreeHostContiguous(int block_id) { return GetHostAllocator()->FreeContiguous(block_id); }

Status BlockManager::GetHostBlockPtrs(const std::vector<int>& blocks, std::vector<void*>& addrs) {
  return GetHostAllocator()->GetBlockPtrs(blocks, addrs);
}

Status BlockManager::GetHostContiguousPtr(int block_id, void*& addr) {
  return GetHostAllocator()->GetContiguousPtr(block_id, addr);
}

int BlockManager::GetHostFreeBlockNumber() { return GetHostAllocator()->GetFreeBlockNumber(); }

int BlockManager::GetHostUsedBlockNumber() { return GetHostAllocator()->GetUsedBlockNumber(); }

Status BlockManager::SwapOut(const std::vector<int>& device_blocks, std::vector<int>& host_blocks) {
  // Allocate memory on host.
  STATUS_CHECK_RETURN(host_allocator_->AllocateBlocks(device_blocks.size(), host_blocks));

  // Get host and device address.
  std::vector<void*> host_addrs;
  STATUS_CHECK_RETURN(host_allocator_->GetBlockPtrs(host_blocks, host_addrs));

  int device_id = GetDeviceId();
  int block_size = block_manager_config_.device_allocator_config.block_size;

  std::vector<void*> device_addrs;
  STATUS_CHECK_RETURN(device_allocators_[device_id]->GetBlockPtrs(device_blocks, device_addrs));

  cudaStream_t* stream;
  if (context_->IsRunContextDecodeAndDecodeSerially()) {
    stream = &(context_->GetComputeStreams()[device_id]);
  } else {
    // TODO(karlluo): implement multiple thread stream event concurrent.
    throw std::runtime_error("Context decode and decode run in concurrently is unimplemented.");
  }

  // Copy from device to host.
  for (size_t i = 0; i < device_blocks.size(); i++) {
    CUDA_CHECK(cudaMemcpyAsync(host_addrs[i], device_addrs[i], block_size, cudaMemcpyDeviceToHost, (*stream)));
  }

  if (!context_->IsRunContextDecodeAndDecodeSerially()) {
    // TODO(karlluo): implement multiple thread stream event concurrent.
    throw std::runtime_error("Context decode and decode run in concurrently is unimplemented.");
  }

  // Free device blocks.
  device_allocators_[device_id]->FreeBlocks(device_blocks);
  return Status();
}

Status BlockManager::SwapIn(const std::vector<int>& host_blocks, std::vector<int>& device_blocks) {
  int device_id = GetDeviceId();
  int block_size = block_manager_config_.device_allocator_config.block_size;

  // Allocate memory on device.
  STATUS_CHECK_RETURN(device_allocators_[device_id]->AllocateBlocks(host_blocks.size(), device_blocks));

  std::vector<void*> device_addrs;
  STATUS_CHECK_RETURN(GetBlockPtrs(device_blocks, device_addrs));

  std::vector<void*> host_addrs;
  STATUS_CHECK_RETURN(host_allocator_->GetBlockPtrs(host_blocks, host_addrs));

  cudaStream_t* stream;
  if (context_->IsRunContextDecodeAndDecodeSerially()) {
    stream = &(context_->GetComputeStreams()[device_id]);
  } else {
    // TODO(karlluo): implement multiple thread stream event concurrent.
    throw std::runtime_error("Context decode and decode run in concurrently is unimplemented.");
  }

  // Copy from host to device.
  for (size_t i = 0; i < host_blocks.size(); i++) {
    CUDA_CHECK(cudaMemcpyAsync(device_addrs[i], host_addrs[i], block_size, cudaMemcpyHostToDevice, (*stream)));
  }

  if (!context_->IsRunContextDecodeAndDecodeSerially()) {
    // TODO(karlluo): implement multiple thread stream event concurrent.
    throw std::runtime_error("Context decode and decode run in concurrently is unimplemented.");
  }
  // Free host blocks.
  host_allocator_->FreeBlocks(host_blocks);
  return Status();
}

Status BlockManager::SwapDrop(const std::vector<int>& host_blocks) { return host_allocator_->FreeBlocks(host_blocks); }

size_t BlockManager::GetBlockSize() const { return block_manager_config_.device_allocator_config.block_size; }

size_t BlockManager::GetBlockTokenNum() const { return block_manager_config_.device_allocator_config.block_token_num; }

}  // namespace ksana_llm
