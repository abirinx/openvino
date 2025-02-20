﻿// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "low_precision/eltwise_base_transformation.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "low_precision/network_helper.hpp"

using namespace ngraph;
using namespace ngraph::pass;
using namespace ngraph::pass::low_precision;

bool EltwiseBaseTransformation::isBroadcasted(const PartialShape& shape) noexcept {
    const auto rank = shape.rank();
    if (rank.is_dynamic()) {
        return false;
    }

    const size_t rankValue = rank.get_length();
    const size_t spatialIndex = rankValue == 1 ? 0ul : (rankValue == 2ul ? 1ul : 2ul);
    for (size_t i = spatialIndex; i < rankValue; ++i) {
        if (shape[i].is_dynamic() || shape[i].get_length() != 1ul) {
            return false;
        }
    }

    return true;
}

bool EltwiseBaseTransformation::canBeTransformed(const TransformationContext& context, std::shared_ptr<Node> operation) const {
    if (!LayerTransformation::canBeTransformed(context, operation)) {
        return false;
    }

    if (operation->get_input_size() != 2ul) {
        return false;
    }

    FakeQuantizeDequantization dequantization1 = pass::low_precision::NetworkHelper::getDequantization(operation, 0ul);
    FakeQuantizeDequantization dequantization2 = pass::low_precision::NetworkHelper::getDequantization(operation, 1ul);
    if ((dequantization1.empty() || ((dequantization1.multiply != nullptr) && !FakeQuantizeDequantization::checkElementwise(dequantization1.multiply))) &&
        (dequantization2.empty() || ((dequantization2.multiply != nullptr) && !FakeQuantizeDequantization::checkElementwise(dequantization2.multiply)))) {
        return false;
    }

    // at least one branch quantization is mandatory
    if ((dequantization1.data.get_node() == nullptr) ||
        (dequantization2.data.get_node() == nullptr) ||
        (dequantization1.empty() && dequantization2.empty())) {
        return false;
    }

    return true;
}

static bool isTargetType(const std::shared_ptr<Node> node) {
    return ov::is_type<opset1::Convolution>(node) ||
           ov::is_type<opset1::GroupConvolution>(node) ||
           ov::is_type<opset1::MatMul>(node);
}

static std::shared_ptr<Node> getDataParent(const std::shared_ptr<Node> branchData) {
    std::shared_ptr<Node> parent = branchData;
    while (ov::is_type<opset1::FakeQuantize>(parent)) {
        parent = parent->get_input_node_shared_ptr(0);
    }

    if (ov::is_type<opset1::Add>(parent) && isTargetType(parent->get_input_node_shared_ptr(0))) {
        return parent->get_input_node_shared_ptr(0);
    }
    return parent;
}

static bool isBranchHaveMultipleConsumers(const std::shared_ptr<Node> branchData, const std::shared_ptr<Node> branchDataParent) {
    auto parent = branchData;
    while (parent != branchDataParent) {
        if ((parent->get_output_size() != 1ul) || (parent->get_output_target_inputs(0).size() != 1ul)) {
            return true;
        }
        parent = parent->get_input_node_shared_ptr(0);
    }
    return (parent->get_output_size() != 1ul) || (parent->get_output_target_inputs(0).size() != 1ul);
}

// return branch index with FP32 precision after eltwise transformation
int EltwiseBaseTransformation::getNotEmpty(const std::shared_ptr<Node>& eltwise) const {
    const FakeQuantizeDequantization dequantization1 = pass::low_precision::NetworkHelper::getDequantization(eltwise, 0ul);
    if (ov::as_type<opset1::Constant>(dequantization1.data.get_node())) {
        return -1;
    }

    const FakeQuantizeDequantization dequantization2 = pass::low_precision::NetworkHelper::getDequantization(eltwise, 1ul);
    if (ov::as_type<opset1::Constant>(dequantization2.data.get_node())) {
        return -1;
    }

    if (!dequantization1.empty() && dequantization1.isLowPrecision() && (dequantization2.empty() || !dequantization2.isLowPrecision())) {
        return 1;
    }

    if ((dequantization1.empty() || !dequantization1.isLowPrecision()) && !dequantization2.empty() && dequantization2.isLowPrecision()) {
        return 0;
    }

    if (!updatePrecisions) {
        // If result is still not defined, then handle special cases for updatePrecisions == false, assumption for one branch quantization:
        //    1. branch with dequantization operations is quantized,
        //    2. empty branch is not quantized.
        // As result: move dequantization operations to empty branch.
        // Note: keep comparisions uppper as is: low precision can be used in updatePrecisions == false case
        // if FakeQuantize operations were decomposed before LPT.
        if (!dequantization1.empty() && dequantization2.empty()) {
            return 1;
        }

        if (dequantization1.empty() || !dequantization2.empty()) {
            return 0;
        }
    }

    const std::shared_ptr<opset1::FakeQuantize> fakeQuantize1 =
        ov::as_type_ptr<opset1::FakeQuantize>(dequantization1.data.get_node_shared_ptr());
    const std::shared_ptr<opset1::FakeQuantize> fakeQuantize2 =
        ov::as_type_ptr<opset1::FakeQuantize>(dequantization2.data.get_node_shared_ptr());

    if (fakeQuantize1 && !fakeQuantize2) {
        return 0;
    }

    if (!fakeQuantize1 && fakeQuantize2) {
        return 1;
    }

    if (fakeQuantize1 && fakeQuantize2) {
        size_t children1 = fakeQuantize1->get_output_target_inputs(0).size();
        size_t children2 = fakeQuantize2->get_output_target_inputs(0).size();
        if (children1 == 1 && children2 > 1)
            return 0;
        if (children1 > 1 && children2 == 1)
            return 1;
    }

    if (ov::is_type<opset1::Constant>(dequantization1.data.get_node())) {
        return 0;
    }

    if (ov::is_type<opset1::Constant>(dequantization2.data.get_node())) {
        return 1;
    }

    const std::vector<std::shared_ptr<Node>> parentNodes = {
            getDataParent(dequantization1.data.get_node_shared_ptr()),
            getDataParent(dequantization2.data.get_node_shared_ptr()) };

    const bool allBranchesAreEqual = isTargetType(parentNodes[0]) == isTargetType(parentNodes[1]);
    if (allBranchesAreEqual) {
        for (size_t i = 0; i < parentNodes.size(); ++i) {
             if (isBroadcasted(parentNodes[i]->get_output_partial_shape(0))) {
                return static_cast<int>(i);
            }
        }
    }

    const bool multipleConsumers0 = isBranchHaveMultipleConsumers(dequantization1.data.get_node_shared_ptr(), parentNodes[0]);
    const bool multipleConsumers1 = isBranchHaveMultipleConsumers(dequantization2.data.get_node_shared_ptr(), parentNodes[1]);
    if (multipleConsumers0 && !multipleConsumers1) {
        return 1;
    }
    if (!multipleConsumers0 && multipleConsumers1) {
        return 0;
    }

    if (!allBranchesAreEqual) {
        for (size_t i = 0; i < parentNodes.size(); ++i) {
            if (isTargetType(parentNodes[i])) {
                return static_cast<int>(i);
            }
        }
    }

    return 0;
}

std::pair<int, int> EltwiseBaseTransformation::getMultiplyConstBranch(const std::shared_ptr<Node>& eltwise) const {
    const std::shared_ptr<Node> parent1 = eltwise->get_input_node_shared_ptr(0);
    const auto dequantization1 = NetworkHelper::getDequantization(eltwise, 0);
    const std::shared_ptr<Node> parent2 = eltwise->get_input_node_shared_ptr(1);
    const auto dequantization2 = NetworkHelper::getDequantization(eltwise, 1);

    std::shared_ptr<opset1::Constant> constParent = dequantization1.empty() ?
        ov::as_type_ptr<opset1::Constant>(parent1) :
        ov::as_type_ptr<opset1::Constant>(dequantization1.data.get_node_shared_ptr());
    std::shared_ptr<opset1::Multiply> multiplyParent = ov::as_type_ptr<opset1::Multiply>(parent2);
    int multiplyBranch = 1;


    if (constParent == nullptr || multiplyParent == nullptr) {
        constParent = dequantization2.empty() ?
            ov::as_type_ptr<opset1::Constant>(parent2) :
            ov::as_type_ptr<opset1::Constant>(dequantization2.data.get_node_shared_ptr());
        multiplyParent = ov::as_type_ptr<opset1::Multiply>(parent1);
        multiplyBranch = 0;
    }

    if (constParent == nullptr || multiplyParent == nullptr) {
        return {-1, -1};
    }

    auto multiplyParentParent1 = multiplyParent->get_input_node_shared_ptr(0);
    auto multiplyParentParent2 = multiplyParent->get_input_node_shared_ptr(1);

    auto multiplyParentParent = ov::as_type_ptr<opset1::Multiply>(multiplyParentParent1);
    auto multiplyParentConst = ov::as_type_ptr<opset1::Constant>(multiplyParentParent2);
    int multiplyActBranch = 0;


    if (multiplyParentConst == nullptr) {
        multiplyParentParent = ov::as_type_ptr<opset1::Multiply>(multiplyParentParent2);
        multiplyParentConst = ov::as_type_ptr<opset1::Constant>(multiplyParentParent1);
        multiplyActBranch = 1;
    }

    if (multiplyParentConst == nullptr) {
        return { multiplyBranch, -1 };
    }

    return { multiplyBranch, multiplyActBranch };
}

bool EltwiseBaseTransformation::isPrecisionPreserved(std::shared_ptr<Node> layer) const noexcept {
    return false;
}
