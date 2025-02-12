/* Copyright 2024 Tencent Inc.  All rights reserved.

==============================================================================*/
#pragma once

#include <functional>
#include <utility>

#include "ksana_llm/batch_manager/batch_manager.h"
#include "ksana_llm/utils/channel.h"
#include "ksana_llm/utils/request.h"
#include "ksana_llm/utils/status.h"

namespace ksana_llm {

// The base class of all endpoints.
class BaseEndpoint {
  public:
    BaseEndpoint(const EndpointConfig &endpoint_config,
                 Channel<std::pair<Status, std::shared_ptr<Request>>> &request_queue);

    virtual ~BaseEndpoint() {}

  protected:
    // The channel used to pass request from endpoint.
    Channel<std::pair<Status, std::shared_ptr<Request>>> &request_queue_;

    // The endpoint config.
    EndpointConfig endpoint_config_;
};

// The base class of rpc endpoints, such as http/trpc.
class RpcEndpoint : public BaseEndpoint {
  public:
    RpcEndpoint(const EndpointConfig &endpoint_config,
                Channel<std::pair<Status, std::shared_ptr<Request>>> &request_queue);

    virtual ~RpcEndpoint() override {}

    // Listen at specific socket.
    virtual Status Start() = 0;

    // Close the listening socket.
    virtual Status Stop() = 0;
};

}  // namespace ksana_llm
