// Copyright (C) 2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include <ngraph/strides.hpp>
#include <ngraph/node_input.hpp>
#include <ngraph/variant.hpp>
#include <transformations_visibility.hpp>

namespace ov {
class TRANSFORMATIONS_API StridesPropagation : public VariantImpl<ngraph::Strides> {
public:
    OPENVINO_RTTI("strides_propagation", "0");

    StridesPropagation() = default;

    StridesPropagation(const value_type& value) : VariantImpl<value_type>(value) {}
};

TRANSFORMATIONS_API bool has_strides_prop(const ngraph::Input<ngraph::Node>& node);
TRANSFORMATIONS_API ngraph::Strides get_strides_prop(const ngraph::Input<ngraph::Node>& node);
TRANSFORMATIONS_API void insert_strides_prop(ngraph::Input<ngraph::Node>& node, const ngraph::Strides& strides);
} // namespace ov
