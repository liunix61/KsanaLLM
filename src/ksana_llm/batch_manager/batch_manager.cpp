/* Copyright 2023 Tencent Inc.  All rights reserved.

==============================================================================*/

#include "ksana_llm/batch_manager/batch_manager.h"
#include "ksana_llm/runtime/infer_request.h"
#include "ksana_llm/utils/logger.h"
#include "ksana_llm/utils/request.h"
#include "ksana_llm/utils/tensor.h"
#include "ksana_llm/utils/waiter.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

namespace ksana_llm {

BatchManager::BatchManager(const BatchManagerConfig &batch_manager_config, std::shared_ptr<Context> context) {
  batch_manager_config_ = batch_manager_config;
  context_ = context;

  Initialize();
}

Status BatchManager::Initialize() {
  batch_scheduler_ = std::make_shared<BatchScheduler>(batch_manager_config_.batch_scheduler_config, context_);

  context_caching_ = std::make_shared<ContextCaching>(batch_manager_config_.context_caching_config);

  lora_coordinator_ = std::make_shared<LoraCoordinator>(batch_manager_config_.lora_coordinator_config);

  request_batching_ = std::make_shared<RequestBatching>(batch_manager_config_.request_batching_config);

  llm_runtime_ = std::make_shared<LlmRuntime>(context_);

  queue_waiter_ = std::make_shared<Waiter>(1);

  return Status();
}

Status BatchManager::RegisterModelInstance(const std::shared_ptr<ModelInstance> &model_instance) {
  NLLM_LOG_DEBUG << "register model instance " << model_instance->name << " : " << model_instance.get();
  model_instances_[model_instance->name] = model_instance;
  return Status();
}

Status BatchManager::Enqueue(std::shared_ptr<Request> &req) {
  NLLM_LOG_DEBUG << "batch manager enqueue req id " << req->req_id;

  Status enqueue_status = Status(RetCode::RET_SUCCESS);

  std::shared_ptr<InferRequest> infer_req = std::make_shared<InferRequest>(req);

  infer_req->kv_cache_blocks.resize(context_->GetTensorParallelSize());
  infer_req->block_size = GetBlockManager()->GetBlockSize();

  infer_req->model_instance = model_instances_[req->model_name];
  infer_req->infer_stage = InferStage::STAGE_CONTEXT;
  infer_req->step = 0;

  enqueue_status = batch_scheduler_->AddInferRequest(infer_req);
  if (enqueue_status.OK()) {
    NLLM_LOG_DEBUG << "batch schdule add req id " << req->req_id << " and " << infer_req->input_tokens.size()
                   << " tokens";
  } else {
    NLLM_LOG_ERROR << "batch schdule add req id " << req->req_id << " and " << infer_req->input_tokens.size()
                   << " tokens failed, message: " << enqueue_status.ToString();
  }

  queue_waiter_->Notify();
  return Status();
}

Status BatchManager::WaitAllDone() { return Status(); }

Status BatchManager::Process() {
  // uint64_t scheduling_time_ns = 0;
  // uint64_t generate_time_ns = 0;
  while (!terminated_) {
    std::vector<std::shared_ptr<InferRequest>> scheduled_reqs;

    // auto start_time = std::chrono::high_resolution_clock::now();
    scheduled_reqs = batch_scheduler_->Schedule();
    if (scheduled_reqs.empty()) {
      queue_waiter_->Wait();
      queue_waiter_->Reset(1);
      continue;
    }

    NLLM_LOG_INFO << "batch scheduler result " << scheduled_reqs.size();
                  // << ", scheduling/generate:" << scheduling_time_ns << "/" << generate_time_ns;
    // auto ready_time = std::chrono::high_resolution_clock::now();
    llm_runtime_->Step(scheduled_reqs);
    // auto finish_time = std::chrono::high_resolution_clock::now();

    // Skip empty scheduling time.
    // scheduling_time_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(ready_time - start_time).count();
    // generate_time_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(finish_time - ready_time).count();
  }

  return Status();
}

Status BatchManager::Start() {
  batch_manager_thread_ = std::unique_ptr<std::thread>(new std::thread(&BatchManager::Process, this));

  return Status();
}

Status BatchManager::Stop() {
  NLLM_LOG_DEBUG << "Stop batch manager.";

  terminated_ = true;

  // Break process loop.
  queue_waiter_->Notify();

  if (batch_manager_thread_ && batch_manager_thread_->joinable()) {
    batch_manager_thread_->join();
  }

  NLLM_LOG_DEBUG << "batch manager stopped.";
  return Status();
}

}  // namespace ksana_llm
