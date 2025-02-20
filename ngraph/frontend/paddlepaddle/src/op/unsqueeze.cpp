// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <node_context.hpp>

#include "openvino/opsets/opset6.hpp"

namespace ov {
namespace frontend {
namespace pdpd {
namespace op {
NamedOutputs unsqueeze(const NodeContext& node) {
    auto data = node.get_ng_input("X");
    Output<Node> axesNode;
    if (node.has_ng_input("AxesTensor")) {
        axesNode = node.get_ng_input("AxesTensor");
    } else if (node.has_ng_input("AxesTensorList")) {
        auto inputs = node.get_ng_inputs("AxesTensorList");
        axesNode = std::make_shared<ov::opset6::Concat>(inputs, 0);
    } else {
        auto axes = node.get_attribute<std::vector<int32_t>>("axes");
        axesNode = ov::opset6::Constant::create(ov::element::i32, {axes.size()}, axes);
    }
    return node.default_single_output_mapping({std::make_shared<ov::opset6::Unsqueeze>(data, axesNode)}, {"Out"});
}
}  // namespace op
}  // namespace pdpd
}  // namespace frontend
}  // namespace ov
