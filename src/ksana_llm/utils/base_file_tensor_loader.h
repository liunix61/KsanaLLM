// Copyright 2024 Tencent Inc.  All rights reserved.
#pragma once

#include "ksana_llm/utils/device_types.h"

#include <caffe2/serialize/inline_container.h>
#include <torch/csrc/jit/serialization/import_read.h>
#include <torch/csrc/jit/serialization/storage_context.h>
#include <torch/script.h>
#include <tuple>

namespace ksana_llm {

// Define a base class named BaseFileTensorLoader
class BaseFileTensorLoader {
  public:
    // Constructor that takes a file name as input
    BaseFileTensorLoader(const std::string& file_name) : file_name_(file_name) {}

    // Pure virtual function to get the list of tensor names
    virtual const std::vector<std::string>& GetTensorNameList() = 0;

    // Pure virtual function to get a tensor by its name
    virtual std::tuple<void*, size_t> GetTensor(const std::string& tensor_name) = 0;

    virtual DataType GetTensorDataType(const std::string& tensor_name) = 0;

    virtual std::vector<std::size_t> GetTensorShape(const std::string& tensor_name) = 0;

  protected:
    std::string file_name_;
    std::vector<std::string> tensor_name_list_;
};

}  // namespace ksana_llm
