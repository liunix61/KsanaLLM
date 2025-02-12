/* Copyright 2024 Tencent Inc.  All rights reserved.

==============================================================================*/

#include "ksana_llm/batch_manager/batch_scheduler/batch_scheduler.h"

#include <algorithm>
#include <future>
#include <memory>
#include <numeric>
#include <thread>
#include <utility>
#include <vector>

#include "ksana_llm/block_manager/block_manager.h"
#include "ksana_llm/block_manager/memory_block.h"
#include "ksana_llm/profiler/reporter.h"
#include "ksana_llm/runtime/infer_request.h"
#include "ksana_llm/utils/channel.h"
#include "ksana_llm/utils/context.h"
#include "ksana_llm/utils/logger.h"
#include "ksana_llm/utils/memory_utils.h"
#include "ksana_llm/utils/ret_code.h"
#include "ksana_llm/utils/singleton.h"
#include "ksana_llm/utils/status.h"
#include "ksana_llm/utils/string_utils.h"

namespace ksana_llm {

BatchScheduler::BatchScheduler(const BatchSchedulerConfig &batch_scheduler_config, std::shared_ptr<Context> context)
    : batch_scheduler_config_(batch_scheduler_config), context_(context) {
  // Config validation.
  NLLM_CHECK_WITH_INFO(batch_scheduler_config_.max_step_tokens > batch_scheduler_config_.max_token_len,
                       FormatStr("The max_step_tokens must large than max_token_len, %d vs %d.",
                                 batch_scheduler_config_.max_step_tokens, batch_scheduler_config_.max_token_len));

  batch_state_ = std::make_shared<BatchState>(batch_scheduler_config_);
  schedule_strategy_ = ScheduleStrategyFactory::CreateScheduleStrategy(batch_scheduler_config_, context_, batch_state_);
}

Status BatchScheduler::AddInferRequest(std::vector<std::shared_ptr<InferRequest>>& infer_request_group) {
  std::shared_ptr<InferRequest>& infer_request = infer_request_group[0];
  NLLM_LOG_DEBUG << "batch scheduler add infer req " << infer_request->req_id << ", max_new_tokens "
                 << infer_request->sampling_config.max_new_tokens;
  if (CheckWaitingQueueFull(infer_request_group.size())) {
    NLLM_LOG_DEBUG << "waiting queue is full, req " << infer_request->req_id << " failed.";

    infer_request->finish_status = Status(RET_EXCEED_CAPACITY, "waiting queue is full.");
    for (auto& infer_request : infer_request_group) {
      infer_request->finished = true;
    }
    infer_request->Notify();

    return infer_request->finish_status;
  }

  if (CheckRequestExceedLength(infer_request)) {
    NLLM_LOG_DEBUG << "input len is too long, req " << infer_request->req_id << " failed.";

    infer_request->finish_status = Status(RET_EXCEED_LENGTH, "input length exceed max_token_len.");
    for (auto& infer_request : infer_request_group) {
      infer_request->finished = true;
    }
    infer_request->Notify();

    return infer_request->finish_status;
  }

  std::lock_guard<std::mutex> guard(batch_state_->queue_buffer_mutex);
  for (auto& infer_request : infer_request_group){
    batch_state_->waiting_buffer_queue.push_back(infer_request);
  }
  return Status();
}

bool BatchScheduler::WaitingBufferEmpty() {
  std::lock_guard<std::mutex> guard(batch_state_->queue_buffer_mutex);
  return batch_state_->waiting_buffer_queue.empty();
}

bool BatchScheduler::SwappedQueueEmtpy() {
  std::lock_guard<std::mutex> guard(batch_state_->queue_mutex);
  return batch_state_->swapped_queue.empty();
}

bool BatchScheduler::CheckWaitingQueueFull(int num) {
  return batch_state_->waiting_queue.size() + num >= batch_scheduler_config_.max_waiting_queue_len;
}

inline bool BatchScheduler::CheckRequestExceedLength(const std::shared_ptr<InferRequest> req) {
  return req->input_tokens.size() > batch_scheduler_config_.max_token_len;
}

std::vector<std::shared_ptr<InferRequest>> &BatchScheduler::Schedule() {
  NLLM_LOG_DEBUG << "Try scheduler loop.";
  std::lock_guard<std::mutex> guard(batch_state_->queue_mutex);

  schedule_strategy_->Schedule();

  REPORT_METRIC(batch_scheduler_running, batch_state_->running_queue.size());
  REPORT_METRIC(batch_scheduler_waiting, batch_state_->waiting_queue.size());
  REPORT_METRIC(batch_scheduler_swapped, batch_state_->swapped_queue.size());

  REPORT_METRIC(block_manager_free, GetBlockManager()->GetDeviceFreeBlockNumber());
  REPORT_METRIC(block_manager_used, GetBlockManager()->GetDeviceUsedBlockNumber());

  NLLM_LOG_DEBUG << "batch scheduler result: " << batch_state_->running_queue.size();
  return batch_state_->running_queue;
}

}  // namespace ksana_llm
