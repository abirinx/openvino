// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

/**
 * @brief Defines old API map attribute
 * @file old_api_map_attribute.hpp
 */

#pragma once

#include <assert.h>

#include <functional>
#include <memory>
#include <ngraph/attribute_visitor.hpp>
#include <ngraph/node.hpp>
#include <ngraph/variant.hpp>
#include <openvino/core/rtti.hpp>
#include <set>
#include <string>
#include <transformations_visibility.hpp>
#include <utility>

namespace ov {

class OldApiMapOrder;
/**
 * @ingroup ie_runtime_attr_api
 * @brief OldApiMapOrder class represents runtime info attribute that stores
 * order of the transpose that is required for obtaining IR in old API.
 *
 *  OldApiMapOrder stores the following information.
 *  Parameter:
 *  Order of the transpose which should be applied to Parameter with old API layout to
 *  obtain Parameter with new API layout.
 *
 *  Result:
 *  Order of the transpose which should be applied to Result with new API layout to
 *  obtain Result with old API layout.
 */
class TRANSFORMATIONS_API OldApiMapOrder : public VariantImpl<std::vector<uint64_t>> {
public:
    OPENVINO_RTTI("old_api_map_order", "0");

    /**
     * A default constructor
     */
    OldApiMapOrder() = default;

    /**
     * Constructs a new OldApiMapOrder object.
     * @param[in]  value  The object that stores values of OldApiMapOrder.
     */
    OldApiMapOrder(const value_type& value) : VariantImpl<value_type>(value) {}

    bool is_copyable() const override {
        return false;
    }

    bool visit_attributes(AttributeVisitor& visitor) override;
};

inline bool has_old_api_map_order(const std::shared_ptr<Node>& node) {
    const auto& rt_map = node->get_rt_info();
    return rt_map.count(OldApiMapOrder::get_type_info_static());
}

inline OldApiMapOrder get_old_api_map_order(const std::shared_ptr<Node>& node) {
    const auto& rt_map = node->get_rt_info();
    const auto& var = rt_map.at(OldApiMapOrder::get_type_info_static());
    return ngraph::as_type_ptr<OldApiMapOrder>(var)->get();
}

inline void set_old_api_map_order(std::shared_ptr<Node>& node, const OldApiMapOrder& old_api_map) {
    auto& rt_map = node->get_rt_info();
    rt_map[OldApiMapOrder::get_type_info_static()] = std::make_shared<OldApiMapOrder>(old_api_map);
}

}  // namespace ov
