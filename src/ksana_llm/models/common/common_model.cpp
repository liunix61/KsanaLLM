/* Copyright 2024 Tencent Inc.  All rights reserved.

==============================================================================*/
#include "ksana_llm/models/common/common_model.h"

#include <memory>
#include <vector>

#include "ksana_llm/utils/common_device.h"
#include "ksana_llm/utils/logger.h"
#include "ksana_llm/utils/memory_utils.h"
#include "ksana_llm/utils/singleton.h"

namespace ksana_llm {

template <typename T>
CommonModel<T>::CommonModel(const ModelConfig& model_config, const int rank, std::shared_ptr<Context> context) {
  model_config_ = model_config;
  context_ = context;
  rank_ = rank;
}

template <typename T>
CommonModel<T>::~CommonModel() {
  STATUS_CHECK_FAILURE(DestroyTensor(tensor_buffer_0_, rank_));
  STATUS_CHECK_FAILURE(DestroyTensor(tensor_buffer_1_, rank_))
  STATUS_CHECK_FAILURE(DestroyTensor(tensor_buffer_2_, rank_));
  STATUS_CHECK_FAILURE(DestroyTensor(up_matmul_tensor_buffer_, rank_));
  STATUS_CHECK_FAILURE(DestroyTensor(cos_sin_cache_tensor_, rank_));
  STATUS_CHECK_FAILURE(DestroyTensor(forward_shape_, rank_));

#ifdef ENABLE_ACL
  STATUS_CHECK_FAILURE(DestroyTensor(ascend_buffer_0_, rank_));
  STATUS_CHECK_FAILURE(DestroyTensor(ascend_buffer_1_, rank_));
  STATUS_CHECK_FAILURE(DestroyTensor(ascend_buffer_2_, rank_));
  STATUS_CHECK_FAILURE(DestroyTensor(ascend_buffer_3_, rank_));
  STATUS_CHECK_FAILURE(DestroyTensor(ascend_buffer_4_, rank_));

  for (int idx = 0; idx < num_layer_; ++idx) {
    STATUS_CHECK_FAILURE(DestroyTensor(ascend_key_caches_[idx], rank_));
    STATUS_CHECK_FAILURE(DestroyTensor(ascend_val_caches_[idx], rank_));
  }
#endif
}

template <typename T>
void CommonModel<T>::InitRunConfig(const ModelRunConfig& model_run_config) {
  GetBlockManager()->SetDeviceId(rank_);

  num_layer_ = model_config_.num_layer;
  size_t vocab_size_pad_ =
    DivRoundUp(model_config_.vocab_size, model_config_.tensor_para_size) * model_config_.tensor_para_size;

  int head_num = model_config_.head_num;
  int size_per_head = model_config_.size_per_head;
  int hidden_units = size_per_head * head_num;
  int tensor_para_size = model_config_.tensor_para_size;
  int rotary_embedding = model_config_.rotary_embedding;
  int head_num_per_tp = head_num / tensor_para_size;
  int num_kv_heads_per_tp = model_config_.num_key_value_heads / tensor_para_size;
  int stride_size = (head_num_per_tp + num_kv_heads_per_tp * 2) * size_per_head;
  int max_position_embeddings = model_config_.max_position_embeddings;
  float rope_theta = model_config_.rope_theta;

  bool is_alibi = (model_run_config.position_encoding == PositionEncoding::ALIBI);
  BlockManagerConfig block_manager_config;
  STATUS_CHECK_FAILURE(Singleton<Environment>::GetInstance()->GetBlockManagerConfig(block_manager_config));

  size_t max_token_num = model_config_.max_scheduler_token_num;
  qkv_add_bias_ = model_run_config.qkv_add_bias;
  NLLM_LOG_DEBUG << fmt::format("Max_Batch_Size = {}, Max Seq Len = {}, Max Token Num = {}",
                                model_config_.max_batch_size, model_config_.max_token_num, max_token_num);

  int inter_size_per_tp = model_config_.inter_size / tensor_para_size;
  int max_dim =
    std::max(std::max((head_num_per_tp + 2 * num_kv_heads_per_tp) * size_per_head, hidden_units), inter_size_per_tp);
  size_t tensor_buffer_1_size = std::max(model_config_.max_batch_size * vocab_size_pad_ * sizeof(float),
                                         max_token_num * max_dim * GetTypeSize(model_config_.weight_data_type)) /
                                GetTypeSize(model_config_.weight_data_type);
  size_t up_matmul_tensor_buffer_size = max_token_num * std::max(static_cast<int>(inter_size_per_tp), hidden_units * 2);

  // TODO(karlluo): all create tensor used dynamic memory pool
  STATUS_CHECK_FAILURE(CreateBufferTensor(tensor_buffer_0_, {max_token_num, max_dim}, model_config_.weight_data_type));
  STATUS_CHECK_FAILURE(CreateBufferTensor(tensor_buffer_1_, {tensor_buffer_1_size}, model_config_.weight_data_type))
  STATUS_CHECK_FAILURE(CreateBufferTensor(tensor_buffer_2_, {max_token_num, max_dim}, model_config_.weight_data_type));
  STATUS_CHECK_FAILURE(
    CreateBufferTensor(up_matmul_tensor_buffer_, {up_matmul_tensor_buffer_size}, model_config_.weight_data_type));
  STATUS_CHECK_FAILURE(CreateBufferTensor(cos_sin_cache_tensor_, {rotary_embedding, max_position_embeddings},
                                          model_config_.weight_data_type));
#ifdef ENABLE_ACL
  STATUS_CHECK_FAILURE(CreateBufferTensor(ascend_buffer_0_, {max_token_num, hidden_units}, TYPE_FP16));
  STATUS_CHECK_FAILURE(CreateBufferTensor(ascend_buffer_1_, {max_token_num, hidden_units}, TYPE_FP16));
  STATUS_CHECK_FAILURE(CreateBufferTensor(ascend_buffer_2_, {max_token_num, hidden_units}, TYPE_FP16));
  STATUS_CHECK_FAILURE(CreateBufferTensor(ascend_buffer_3_, {max_token_num, hidden_units}, TYPE_FP16));
  STATUS_CHECK_FAILURE(CreateBufferTensor(ascend_buffer_4_, {max_token_num, hidden_units}, TYPE_FP16));

  for (int idx = 0; idx < num_layer_; ++idx) {
    Tensor key_cache;
    Tensor val_cache;
    STATUS_CHECK_FAILURE(CreateBufferTensor(key_cache, {max_token_num, hidden_units}, TYPE_FP16));
    STATUS_CHECK_FAILURE(CreateBufferTensor(val_cache, {max_token_num, hidden_units}, TYPE_FP16));

    ascend_key_caches_.push_back(key_cache);
    ascend_val_caches_.push_back(val_cache);
  }

#endif
  // TODO(karlluo): we needn't tensor's shape to transfer attribute
  STATUS_CHECK_FAILURE(CreateBufferTensor(forward_shape_, {1}, TYPE_INT32));

  NLLM_LOG_DEBUG << "Total buffer tensors memory used: " << (GetBufferTensorsMemoryUsed() >> 20) << " MB";

  // 初始化各层实例
  emb_lookup_layer_ = std::make_shared<EmbLookupLayer<T>>();
  emb_lookup_layer_->Init({}, context_, rank_);

  cpu_emb_lookup_layer_ = std::make_shared<CpuEmbLookupLayer<T>>();
  cpu_emb_lookup_layer_->Init({}, context_, rank_);

  layernorm_layer_ = std::make_shared<LayernormLayer<T>>();
  layernorm_layer_->Init({model_config_.layernorm_eps}, context_, rank_);

  add_layer_ = std::make_shared<AddLayer<T>>();
  add_layer_->Init({}, context_, rank_);

  silu_mul_layer_ = std::make_shared<SiluMulLayer<T>>();
  silu_mul_layer_->Init({}, context_, rank_);

  matmul_layer_ = std::make_shared<MatMulLayer<T>>();
  matmul_layer_->Init({}, context_, rank_);

  assemble_last_token_layer_ = std::make_shared<AssembleLastTokenLayer<T>>();
  assemble_last_token_layer_->Init({}, context_, rank_);

  cast_layer_ = std::make_shared<CastLayer<T>>();
  cast_layer_->Init({}, context_, rank_);

  model_input_ = std::make_shared<ModelInput>(model_config_, rank_, context_);

  if (Singleton<Environment>::GetInstance()->EmbedTokensUseCpu()) {
    CreateTensor(cpu_input_tokens_tensor_, model_input_->input_ids.shape, model_input_->input_ids.dtype, rank_,
                 MemoryDevice::MEMORY_HOST);
    CreateTensor(cpu_tokens_emb_tensor_, {model_input_->input_ids.shape[0] * hidden_units},
                 model_input_->input_ids.dtype, rank_, MemoryDevice::MEMORY_HOST);
  }
  model_output_ = std::make_shared<ModelOutput>(model_config_.max_batch_size, vocab_size_pad_, rank_, context_);
  model_communicator_ = std::make_shared<ModelCommunicator<T>>(&tensor_buffer_0_, &tensor_buffer_2_, rank_, context_);

  flash_attention_layers_.resize(num_layer_);
  paged_attention_layers_.resize(num_layer_);
  void* cos_sin_cache_ptr = cos_sin_cache_tensor_.GetPtr<void>();
  for (int idx = 0; idx < num_layer_; ++idx) {
    flash_attention_layers_[idx] = std::make_shared<FlashAttentionLayer<T>>();
    paged_attention_layers_[idx] = std::make_shared<PagedAttentionLayer<T>>();
    // NOTE(karlluo): acsends's image g++ is 9.4.0, it do not support convert from ‘<brace-enclosed initializer list>’
    // to ‘std::vector<std::any>’ so we use push back to make it work.
    std::vector<std::any> attention_param;
    attention_param.push_back(idx);
    attention_param.push_back(max_position_embeddings);
    attention_param.push_back(head_num_per_tp);
    attention_param.push_back(num_kv_heads_per_tp);
    attention_param.push_back(size_per_head);
    attention_param.push_back(stride_size);
    attention_param.push_back(tensor_para_size);
    attention_param.push_back(rotary_embedding);
    attention_param.push_back(rope_theta);
    attention_param.push_back(true);
    attention_param.push_back(is_alibi);
    attention_param.push_back(std::any(cos_sin_cache_ptr));
    attention_param.push_back(model_config_.rope_scaling_factor_config);

    flash_attention_layers_[idx]->Init(attention_param, context_, rank_);
    paged_attention_layers_[idx]->Init(attention_param, context_, rank_);
  }
}

template <typename T>
float* CommonModel<T>::GetLogitsPtr() {
  GetBlockManager()->SetDeviceId(rank_);
  return model_output_->logits_tensor.GetPtr<float>();
}

template <typename T>
Status CommonModel<T>::LlamaAttention(const int layer_idx, std::shared_ptr<ksana_llm::BaseWeight>& base_weight,
                                      Tensor& hidden_states, std::vector<Tensor>& temp_buffer_0,
                                      std::vector<Tensor>& temp_buffer_1, std::vector<Tensor>& temp_buffer_2,
                                      const bool is_context_stage) {
  // Attn proj MatMul
  Tensor attn_proj_weight =
    base_weight->GetModelWeights(fmt::format("model.layers.{}.self_attn.query_key_value.weight", layer_idx));
  std::vector<Tensor>& attn_proj_output = temp_buffer_2;
  STATUS_CHECK_RETURN(matmul_layer_->Forward({hidden_states, attn_proj_weight}, attn_proj_output));
  if (qkv_add_bias_) {
    Tensor attn_proj_bias =
      base_weight->GetModelWeights(fmt::format("model.layers.{}.self_attn.query_key_value.bias", layer_idx));
    STATUS_CHECK_RETURN(add_layer_->Forward({attn_proj_output[0], attn_proj_bias}, attn_proj_output));
  }

  // MMHA Flash/Paged Attention
  std::vector<Tensor>& mmha_attention_output = temp_buffer_1;
  if (layer_idx == 0) {
    // only need sync in the first layer
    StreamWaitEvent(context_->GetComputeStreams()[rank_], model_input_->kvcache_offset_event);
    StreamWaitEvent(context_->GetComputeStreams()[rank_], model_input_->rotary_embedding_event);
  }

  if (is_context_stage) {
    STATUS_CHECK_RETURN(flash_attention_layers_[layer_idx]->Forward(
      {attn_proj_output[0], model_input_->input_offset_uint64_tensor, model_input_->kv_list,
       model_input_->kv_cache_offset_tensor, model_input_->rotary_embedding_pos, forward_shape_
#ifdef ENABLE_ACL
       ,
       ascend_buffer_0_, ascend_buffer_1_, ascend_buffer_2_, ascend_buffer_3_, ascend_buffer_4_,
       ascend_key_caches_[layer_idx], ascend_val_caches_[layer_idx]
#endif
      },
      mmha_attention_output));
  } else {
    STATUS_CHECK_RETURN(paged_attention_layers_[layer_idx]->Forward(
      {attn_proj_output[0], model_input_->input_tokens_int32_tensor, model_input_->kv_list,
       model_input_->kv_cache_offset_tensor, model_input_->rotary_embedding_pos, model_input_->kv_cache_buffer,
       forward_shape_, up_matmul_tensor_buffer_
#ifdef ENABLE_ACL
       ,
       ascend_buffer_0_, ascend_buffer_1_, ascend_buffer_2_, ascend_buffer_3_, ascend_buffer_4_,
       ascend_key_caches_[layer_idx], ascend_val_caches_[layer_idx]
#endif
      },
      mmha_attention_output));
  }

  // Attn o_proj MatMul
  Tensor attn_o_proj_weight =
    base_weight->GetModelWeights(fmt::format("model.layers.{}.self_attn.o_proj.weight", layer_idx));
  std::vector<Tensor>& attn_o_proj_output = temp_buffer_2;
  STATUS_CHECK_RETURN(matmul_layer_->Forward({mmha_attention_output[0], attn_o_proj_weight}, attn_o_proj_output));

  // NOTE(karlluo): multiple event in nccl will cause preformance regression
  // nccl multiple event just enable when context.IsRunContextDecodeAndDecodeSerially() == false
  if (!context_->IsRunContextDecodeAndDecodeSerially()) {
    EventRecord(model_output_->compute_ready_event, context_->GetComputeStreams()[rank_]);
    StreamWaitEvent(context_->GetNCCLStreams()[rank_], model_output_->compute_ready_event);
  }
  // Attn NcclAllReduceSum
  std::vector<Tensor>& attn_all_reduce_sum_output = temp_buffer_1;
  model_communicator_->ReduceSum(attn_o_proj_output, attn_all_reduce_sum_output, is_context_stage, true);

  return Status();
}

template <typename T>
Status CommonModel<T>::LlamaMlp(const int layer_idx, std::shared_ptr<ksana_llm::BaseWeight>& base_weight,
                                Tensor& post_layernorm_output, std::vector<Tensor>& temp_buffer_0,
                                std::vector<Tensor>& temp_buffer_1, std::vector<Tensor>& temp_buffer_2) {
  // Mlp gate_proj MatMul
  Tensor gate_proj_weight =
    base_weight->GetModelWeights(fmt::format("model.layers.{}.mlp.gate_proj.weight", layer_idx));
  std::vector<Tensor>& gate_matmul_output = temp_buffer_0;
  STATUS_CHECK_RETURN(matmul_layer_->Forward({post_layernorm_output, gate_proj_weight}, gate_matmul_output));

  // Mlp up_proj MatMul 由于 gate_proj 与 up_proj 为并行关系,因此此处使用额外空间存储 matmul 结果
  Tensor up_proj_weight = base_weight->GetModelWeights(fmt::format("model.layers.{}.mlp.up_proj.weight", layer_idx));
  std::vector<Tensor> up_matmul_output = {up_matmul_tensor_buffer_};
  STATUS_CHECK_RETURN(matmul_layer_->Forward({post_layernorm_output, up_proj_weight}, up_matmul_output));

  std::vector<Tensor>& silu_mul_output = temp_buffer_1;
  STATUS_CHECK_RETURN(silu_mul_layer_->Forward({gate_matmul_output[0], up_matmul_output[0]}, silu_mul_output));

  // Mlp down_proj MatMul
  Tensor down_proj_weight =
    base_weight->GetModelWeights(fmt::format("model.layers.{}.mlp.down_proj.weight", layer_idx));
  std::vector<Tensor>& down_proj_output = temp_buffer_0;
  STATUS_CHECK_RETURN(matmul_layer_->Forward({silu_mul_output[0], down_proj_weight}, down_proj_output));

  // NOTE(karlluo): multiple event in nccl will cause preformance regression
  // nccl multiple event just enable when context.IsRunContextDecodeAndDecodeSerially() == false
  if (!context_->IsRunContextDecodeAndDecodeSerially()) {
    EventRecord(model_output_->compute_ready_event, context_->GetComputeStreams()[rank_]);
    StreamWaitEvent(context_->GetNCCLStreams()[rank_], model_output_->compute_ready_event);
  }
  // Mlp NcclAllReduceSum
  std::vector<Tensor>& mlp_all_reduce_sum_output = temp_buffer_1;
  model_communicator_->ReduceSum({down_proj_output[0]}, mlp_all_reduce_sum_output, false, false);

  return Status();
}

template <typename T>
Status CommonModel<T>::LlamaDecoder(const int layer_idx, std::shared_ptr<ksana_llm::BaseWeight>& base_weight,
                                    std::vector<Tensor>& temp_buffer_0, std::vector<Tensor>& temp_buffer_1,
                                    std::vector<Tensor>& temp_buffer_2, const bool is_context_stage) {
  // input layernorm
  Tensor input_layernorm_weight =
    base_weight->GetModelWeights(fmt::format("model.layers.{}.input_layernorm.weight", layer_idx));
  // input_layernorm_input = layer_idx == 0 ? emb_lookup_output : mlp_add_output
  // Since emb_lookup_output and mlp_add_output point to the same memory address, we implement it as follow:
  std::vector<Tensor>& input_layernorm_input = temp_buffer_0;
  std::vector<Tensor>& input_layernorm_output = temp_buffer_1;
  STATUS_CHECK_RETURN(
    layernorm_layer_->Forward({input_layernorm_input[0], input_layernorm_weight}, input_layernorm_output));

  STATUS_CHECK_RETURN(LlamaAttention(layer_idx, base_weight, input_layernorm_output[0], temp_buffer_0, temp_buffer_1,
                                     temp_buffer_2, is_context_stage));

  // Attn Add
  std::vector<Tensor>& attn_all_reduce_sum_output = temp_buffer_1;
  std::vector<Tensor>& attn_add_output = temp_buffer_2;
  STATUS_CHECK_RETURN(add_layer_->Forward({input_layernorm_input[0], attn_all_reduce_sum_output[0]}, attn_add_output));

  // post_attention_layernorm
  Tensor post_layernorm_weight =
    base_weight->GetModelWeights(fmt::format("model.layers.{}.post_attention_layernorm.weight", layer_idx));
  std::vector<Tensor>& post_layernorm_output = temp_buffer_1;
  STATUS_CHECK_RETURN(layernorm_layer_->Forward({attn_add_output[0], post_layernorm_weight}, post_layernorm_output));

  STATUS_CHECK_RETURN(
    LlamaMlp(layer_idx, base_weight, post_layernorm_output[0], temp_buffer_0, temp_buffer_1, temp_buffer_2));

  // Mlp Add
  std::vector<Tensor>& mlp_all_reduce_sum_output = temp_buffer_1;
  std::vector<Tensor>& mlp_add_output = temp_buffer_0;
  STATUS_CHECK_RETURN(add_layer_->Forward({mlp_all_reduce_sum_output[0], attn_add_output[0]}, mlp_add_output));
  return Status();
}

template <typename T>
Status CommonModel<T>::EmbedTokensUseCpu(Tensor& embedding_weight, std::vector<ForwardRequest>& forward_reqs,
                                         const bool is_context_stage, std::vector<Tensor>& temp_buffer_0) {
  auto batch_size = forward_reqs.size();
  void* input_tokens_ptr = cpu_input_tokens_tensor_.GetPtr<void>();
  size_t index = 0;
  for (size_t idx = 0; idx < batch_size; ++idx) {
    std::vector<int>* req_input = forward_reqs[idx].output_tokens;
    if (is_context_stage) {
      size_t copy_len = req_input->size() * sizeof(int);
      memcpy(input_tokens_ptr + index, req_input->data(), copy_len);
      index += copy_len;
    } else {
      memcpy(input_tokens_ptr + index, &req_input->back(), sizeof(int));
      index += sizeof(int);
    }
  }
  cpu_input_tokens_tensor_.shape = {index / sizeof(int)};
  cpu_emb_lookup_layer_->Forward({cpu_input_tokens_tensor_, cpu_tokens_emb_tensor_, embedding_weight}, temp_buffer_0);
  return Status();
}

template <typename T>
Status CommonModel<T>::LlamaForward(std::shared_ptr<ksana_llm::BaseWeight>& base_weight,
                                    std::vector<ForwardRequest>& forward_reqs, const bool is_context_stage) {
  GetBlockManager()->SetDeviceId(rank_);

  // 推理前准备三块循环使用的推理时临时空间, 用于暂存各层输出结果
  std::vector<Tensor> temp_buffer_0{tensor_buffer_0_};
  std::vector<Tensor> temp_buffer_1{tensor_buffer_1_};
  std::vector<Tensor> temp_buffer_2{tensor_buffer_2_};
  Tensor embedding_weight = base_weight->GetModelWeights("model.embed_tokens.weight");
  if (embedding_weight.device == MemoryDevice::MEMORY_HOST) {
    EmbedTokensUseCpu(embedding_weight, forward_reqs, is_context_stage, temp_buffer_0);
  }

  model_input_->ParseFromRequests(forward_reqs, is_context_stage);

  // create forward shape tensor
  forward_shape_.shape = {model_input_->batch_size, model_input_->max_tokens,
                          model_input_->kv_cache_offset_list.back()};

  std::vector<Tensor>& emb_lookup_output = temp_buffer_0;
  StreamWaitEvent(context_->GetComputeStreams()[rank_], model_input_->input_ids_event);
  if (embedding_weight.device == MemoryDevice::MEMORY_DEVICE) {
    STATUS_CHECK_RETURN(emb_lookup_layer_->Forward(
      {model_input_->input_ids, model_input_->input_offset_uint64_tensor, embedding_weight}, emb_lookup_output));

    // NOTE(karlluo): multiple event in nccl will cause preformance regression
    // nccl multiple event just enable when context.IsRunContextDecodeAndDecodeSerially() == false
    if (!context_->IsRunContextDecodeAndDecodeSerially()) {
      EventRecord(model_output_->compute_ready_event, context_->GetComputeStreams()[rank_]);
      StreamWaitEvent(context_->GetNCCLStreams()[rank_], model_output_->compute_ready_event);
    }

    model_communicator_->AllGather({emb_lookup_output[0], temp_buffer_1[0]}, emb_lookup_output);
  }
  // LlamaDecoder
  for (int layer_idx = 0; layer_idx < num_layer_; ++layer_idx) {
    STATUS_CHECK_RETURN(
      LlamaDecoder(layer_idx, base_weight, temp_buffer_0, temp_buffer_1, temp_buffer_2, is_context_stage));
  }

  // final norm
  Tensor final_layernorm_weight = base_weight->GetModelWeights("model.norm.weight");
  std::vector<Tensor>& final_layernorm_input = temp_buffer_0;
  std::vector<Tensor>& final_layernorm_output = temp_buffer_1;
  STATUS_CHECK_RETURN(
    layernorm_layer_->Forward({final_layernorm_input[0], final_layernorm_weight}, final_layernorm_output));

  // assemble last token
  std::vector<Tensor>& assemble_last_token_output = temp_buffer_2;
  STATUS_CHECK_RETURN(assemble_last_token_layer_->Forward(
    {final_layernorm_output[0], model_input_->input_offset_uint64_tensor}, assemble_last_token_output));

  // lm_head
  Tensor lm_head_weight = base_weight->GetModelWeights("lm_head.weight");
  std::vector<Tensor>& lm_head_output = temp_buffer_0;
  STATUS_CHECK_RETURN(matmul_layer_->Forward({assemble_last_token_output[0], lm_head_weight}, lm_head_output));

  // NOTE(karlluo): multiple event in nccl will cause preformance regression
  // nccl multiple event just enable when context.IsRunContextDecodeAndDecodeSerially() == false
  if (!context_->IsRunContextDecodeAndDecodeSerially()) {
    EventRecord(model_output_->compute_ready_event, context_->GetComputeStreams()[rank_]);
    StreamWaitEvent(context_->GetNCCLStreams()[rank_], model_output_->compute_ready_event);
  }

  model_communicator_->AllGather({lm_head_output[0], temp_buffer_1[0]}, lm_head_output);

  // Cast to float
  std::vector<Tensor>& logits_float = temp_buffer_1;
  logits_float[0].dtype = TYPE_FP32;
  STATUS_CHECK_RETURN(cast_layer_->Forward(lm_head_output, logits_float));
  model_output_->CopyToLogistBuffer(model_input_->batch_size, forward_reqs, logits_float);
  return Status();
}

template <typename T>
Status CommonModel<T>::ContextDecode(std::shared_ptr<ksana_llm::BaseWeight>& base_weight,
                                     std::vector<ForwardRequest>& forward_reqs) {
  return LlamaForward(base_weight, forward_reqs, /*is_context_stage*/ true);
}

template <typename T>
Status CommonModel<T>::Decode(std::shared_ptr<ksana_llm::BaseWeight>& base_weight,
                              std::vector<ForwardRequest>& forward_reqs) {
  return LlamaForward(base_weight, forward_reqs, /*is_context_stage*/ false);
}

template class CommonModel<float>;
template class CommonModel<float16>;
#ifdef ENABLE_BFLOAT16
template class CommonModel<bfloat16>;
#endif

}  // namespace ksana_llm
