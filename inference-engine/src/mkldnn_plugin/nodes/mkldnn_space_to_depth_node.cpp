// Copyright (C) 2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "mkldnn_space_to_depth_node.h"

#include <mkldnn_extension_utils.h>
#include <utils/general_utils.h>

#include <cmath>
#include <cpu/x64/jit_generator.hpp>
#include <ngraph/opsets/opset1.hpp>
#include <string>

#include "common/blocked_desc_creator.h"

#define THROW_ERROR IE_THROW() << "SpaceToDepth layer with name '" << getName() << "' "

using namespace MKLDNNPlugin;
using namespace InferenceEngine;
using namespace mkldnn;
using namespace mkldnn::impl;

bool MKLDNNSpaceToDepthNode::isSupportedOperation(const std::shared_ptr<const ngraph::Node>& op,
                                                  std::string& errorMessage) noexcept {
    try {
        const auto spaceToDepth = ov::as_type_ptr<const ngraph::opset1::SpaceToDepth>(op);
        if (!spaceToDepth) {
            errorMessage = "Only opset1 SpaceToDepth operation is supported";
            return false;
        }
        const auto mode = spaceToDepth->get_mode();
        if (!one_of(mode,
                    ngraph::op::v0::SpaceToDepth::SpaceToDepthMode::BLOCKS_FIRST,
                    ngraph::op::v0::SpaceToDepth::SpaceToDepthMode::DEPTH_FIRST)) {
            errorMessage = "Does not support mode: " + ngraph::as_string(mode);
            return false;
        }
    } catch (...) {
        return false;
    }
    return true;
}

MKLDNNSpaceToDepthNode::MKLDNNSpaceToDepthNode(const std::shared_ptr<ngraph::Node>& op,
                                               const mkldnn::engine& eng,
                                               MKLDNNWeightsSharing::Ptr& cache)
    : MKLDNNNode(op, eng, cache) {
    std::string errorMessage;
    if (!isSupportedOperation(op, errorMessage)) {
        IE_THROW(NotImplemented) << errorMessage;
    }
    if (inputShapes.size() != 1 || outputShapes.size() != 1)
        THROW_ERROR << "has incorrect number of input/output edges!";

    auto spaceToDepth = ov::as_type_ptr<const ngraph::opset1::SpaceToDepth>(op);
    if (!spaceToDepth)
        THROW_ERROR << "supports only opset1";

    const auto modeNgraph = spaceToDepth->get_mode();
    if (modeNgraph == ngraph::op::v0::SpaceToDepth::SpaceToDepthMode::BLOCKS_FIRST) {
        attrs.mode = Mode::BLOCKS_FIRST;
    } else if (modeNgraph == ngraph::op::v0::SpaceToDepth::SpaceToDepthMode::DEPTH_FIRST) {
        attrs.mode = Mode::DEPTH_FIRST;
    } else {
        THROW_ERROR << "doesn't support mode: " << ngraph::as_string(modeNgraph);
    }

    attrs.blockSize = spaceToDepth->get_block_size();
    if (attrs.blockSize == 0)
        THROW_ERROR << "has incorrect block_size parameter is zero!";

    const size_t srcRank = getInputShapeAtPort(0).getRank();
    const size_t dstRank = getOutputShapeAtPort(0).getRank();
    if (srcRank < 3)
        THROW_ERROR << "has incorrect number of input dimensions";
    if (srcRank > 5)
        THROW_ERROR << "doesn't support dimensions with rank greater than 5";
    if (srcRank != dstRank)
        THROW_ERROR << "has incorrect number of input/output dimensions";
    attrs.nSpatialDims = srcRank - 2;
    attrs.blockStep = static_cast<size_t>(std::pow(attrs.blockSize, attrs.nSpatialDims));
}

void MKLDNNSpaceToDepthNode::getSupportedDescriptors() {}

void MKLDNNSpaceToDepthNode::initSupportedPrimitiveDescriptors() {
    if (!supportedPrimitiveDescriptors.empty())
        return;

    InferenceEngine::Precision precision = getOriginalInputPrecisionAtPort(0);

    impl_desc_type impl_type = impl_desc_type::ref;
    if (cpu::x64::mayiuse(impl::cpu::x64::avx512_common)) {
        impl_type = impl_desc_type::jit_avx512;
    } else if (cpu::x64::mayiuse(cpu::x64::avx2)) {
        impl_type = impl_desc_type::jit_avx2;
    } else if (cpu::x64::mayiuse(cpu::x64::sse41)) {
        impl_type = impl_desc_type::jit_sse42;
    }

    NodeConfig config;
    config.dynBatchSupport = true;
    config.inConfs.resize(1);
    config.outConfs.resize(1);
    config.inConfs[0].inPlace = -1;
    config.inConfs[0].constant = false;
    config.outConfs[0].inPlace = -1;
    config.outConfs[0].constant = false;

    const auto& inputDataShape = getInputShapeAtPort(0);
    const auto& outputDataShape = getOutputShapeAtPort(0);

    std::vector<LayoutType> supportedTypes;
    if (inputDataShape.getRank() > 2) {
        const auto& srcDims = inputDataShape.getDims();
        auto canUseBlocked = [=](const size_t block) {
            return srcDims[1] != Shape::UNDEFINED_DIM && srcDims[1] % block == 0 &&
                   (attrs.mode == Mode::DEPTH_FIRST ? block % attrs.blockStep == 0 : true);
        };

        supportedTypes.push_back(LayoutType::nspc);
        if (canUseBlocked(8lu))
            supportedTypes.push_back(LayoutType::nCsp8c);
        if (canUseBlocked(16lu))
            supportedTypes.push_back(LayoutType::nCsp16c);
    }
    supportedTypes.push_back(LayoutType::ncsp);
    auto creators = BlockedDescCreator::getCommonCreators();
    auto range = BlockedDescCreator::makeFilteredRange(creators, inputDataShape.getRank(), supportedTypes);

    for (auto itr = range.first; itr != range.second; ++itr) {
        config.inConfs[0].desc = itr->second->createSharedDesc(precision, inputDataShape);
        config.outConfs[0].desc = itr->second->createSharedDesc(precision, outputDataShape);
        supportedPrimitiveDescriptors.emplace_back(config, impl_type);
    }
}

void MKLDNNSpaceToDepthNode::createPrimitive() {
    auto& dstMemPtr = getChildEdgeAt(0)->getMemoryPtr();
    auto& srcMemPtr = getParentEdgeAt(0)->getMemoryPtr();
    if (!dstMemPtr || !dstMemPtr->GetPrimitivePtr())
        THROW_ERROR << "has not allocated destination memory";
    if (!srcMemPtr || !srcMemPtr->GetPrimitivePtr())
        THROW_ERROR << "has not allocated input memory";
    if (getSelectedPrimitiveDescriptor() == nullptr)
        THROW_ERROR << "has unidentified preferable primitive descriptor";

    const auto& memoryDesc = srcMemPtr->getDesc();
    attrs.dataSize = memoryDesc.getPrecision().size();
    attrs.layoutType = memoryDesc.hasLayoutType(LayoutType::nCsp16c)
                           ? LayoutType::nCsp16c
                           : memoryDesc.hasLayoutType(LayoutType::nCsp8c)
                                 ? LayoutType::nCsp8c
                                 : memoryDesc.hasLayoutType(LayoutType::nspc) ? LayoutType::nspc : LayoutType::ncsp;

    if (inputShapesDefined()) {
        if (needPrepareParams())
            prepareParams();
        updateLastInputDims();
    }
}

void MKLDNNSpaceToDepthNode::prepareParams() {
    const VectorDims& srcBlockedDims = getParentEdgeAt(0)->getMemoryPtr()->GetDescWithType<BlockedMemoryDesc>()->getBlockDims();
    const VectorDims& dstBlockedDims = getChildEdgeAt(0)->getMemoryPtr()->GetDescWithType<BlockedMemoryDesc>()->getBlockDims();
    execPtr = std::make_shared<SpaceToDepthExecutor>(attrs, srcBlockedDims, dstBlockedDims);
}

MKLDNNSpaceToDepthNode::SpaceToDepthExecutor::SpaceToDepthExecutor(const SpaceToDepthAttrs& attrs,
                                                                   const VectorDims& srcBlockedDims,
                                                                   const VectorDims& dstBlockedDims) {
    if (!MKLDNNPlugin::one_of(attrs.layoutType,
                              LayoutType::nCsp16c,
                              LayoutType::nCsp8c,
                              LayoutType::nspc,
                              LayoutType::ncsp))
        IE_THROW() << "DepthToSpace executor supports only 'nCsp16c', 'nCsp8c', "
                      "'nspc' or 'ncsp' layouts.";

    const bool isBlocked = MKLDNNPlugin::one_of(attrs.layoutType, LayoutType::nCsp16c, LayoutType::nCsp8c);
    const bool isChannelsFirst = attrs.layoutType == LayoutType::nspc;

    size_t nDims = srcBlockedDims.size();

    const size_t reshapedRank =
        nDims + attrs.nSpatialDims + static_cast<int>(isBlocked && attrs.mode == Mode::DEPTH_FIRST);
    const size_t lastIdx = reshapedRank - 1;
    size_t firstSpatialOrder = 2;

    PermuteParams params;
    params.data_size = attrs.dataSize;
    params.order.resize(reshapedRank, 0);
    params.src_block_order.resize(reshapedRank);
    params.dst_block_order.resize(reshapedRank);
    params.dst_block_dims.resize(reshapedRank);
    params.src_block_dims.resize(reshapedRank);
    params.src_block_dims[0] = srcBlockedDims[0];

    // reshaping of src dimensions and creating the permutation order for each layout:
    // new shape: [N, C, D1 / block_size, block_size, D2 / block_size, block_size, ... , DK / block_size, block_size]
    // order    : mode = blocks_first : [0,  3, 5, ..., K + (K + 1), 1,  2, 4, ..., K + K]
    //            mode = depth_first  : [0,  1, 3, 5, ..., K + (K + 1),  2, 4, ..., K + K]
    // where `k` is number of spatial dimensions

    auto reshapeAndSetPermOrder =
        [&](const size_t idx1, const size_t idx2, const size_t shift, const SizeVector& dims) {
            for (size_t i = 0; i < attrs.nSpatialDims; i++) {
                params.order[i + idx1] = i * 2 + shift;
                params.order[i + idx2] = i * 2 + shift + 1;

                params.src_block_dims[params.order[i + idx1]] = dims[i + shift];
                params.src_block_dims[params.order[i + idx2]] = attrs.blockSize;
            }
        };

    if (isBlocked) {
        size_t orderShiftForBlocks, orderShiftForDims;
        if (attrs.mode == Mode::BLOCKS_FIRST) {
            orderShiftForBlocks = attrs.nSpatialDims + 2;
            orderShiftForDims = 1;

            params.order[attrs.nSpatialDims + 1] = 1;
            params.order[lastIdx] = lastIdx;

            params.src_block_dims[params.order[attrs.nSpatialDims + 1]] = srcBlockedDims[1];
            params.src_block_dims[params.order[lastIdx]] = srcBlockedDims.back();
        } else {
            orderShiftForBlocks = 3;
            orderShiftForDims = attrs.nSpatialDims + 4;

            size_t extraBlockSize = srcBlockedDims.back() / attrs.blockStep;
            params.src_block_dims[1] = srcBlockedDims[1];
            params.src_block_dims[lastIdx] = extraBlockSize;
            params.src_block_dims[lastIdx - 1] = attrs.blockStep;

            params.order[1] = 1;
            params.order[2] = lastIdx - 1;
            params.order[lastIdx - attrs.nSpatialDims] = lastIdx;
        }

        reshapeAndSetPermOrder(orderShiftForBlocks, orderShiftForDims, firstSpatialOrder, dstBlockedDims);
    } else if (isChannelsFirst) {
        firstSpatialOrder = 1;

        size_t shift = static_cast<size_t>(attrs.mode == DEPTH_FIRST) + attrs.nSpatialDims + 1;
        params.order[attrs.mode == Mode::DEPTH_FIRST ? attrs.nSpatialDims + 1 : lastIdx] = lastIdx;
        params.src_block_dims[lastIdx] = srcBlockedDims.back();

        reshapeAndSetPermOrder(firstSpatialOrder, shift, firstSpatialOrder, dstBlockedDims);
    } else {
        size_t shift = static_cast<size_t>(attrs.mode == DEPTH_FIRST) + 1;
        params.order[attrs.mode == Mode::DEPTH_FIRST ? 1 : attrs.nSpatialDims + 1] = 1;
        params.src_block_dims[1] = srcBlockedDims[1];

        reshapeAndSetPermOrder(attrs.nSpatialDims + firstSpatialOrder, shift, firstSpatialOrder, dstBlockedDims);
    }

    std::iota(params.src_block_order.begin(), params.src_block_order.end(), 0);
    std::iota(params.dst_block_order.begin(), params.dst_block_order.end(), 0);
    for (size_t i = 0; i < reshapedRank; i++)
        params.dst_block_dims[i] = params.src_block_dims[params.order[i]];

    permuteKernel = std::unique_ptr<PermuteKernel>(new PermuteKernel(params));
}

void MKLDNNSpaceToDepthNode::SpaceToDepthExecutor::exec(const uint8_t* srcData, uint8_t* dstData, const int MB) {
    if (!permuteKernel)
        IE_THROW() << "Could not execute. Kernel for Transpose node was not compiled.";
    permuteKernel->execute(srcData, dstData, MB);
}

void MKLDNNSpaceToDepthNode::execute(mkldnn::stream strm) {
    if (!execPtr) {
        THROW_ERROR << "doesn't have a compiled executor.";
    }
    const uint8_t* srcData = reinterpret_cast<const uint8_t *>(getParentEdgeAt(0)->getMemoryPtr()->GetPtr());
    uint8_t* dstData = reinterpret_cast<uint8_t *>(getChildEdgeAt(0)->getMemoryPtr()->GetPtr());
    const int MB = isDynamicNode() ? getParentEdgeAt(0)->getMemoryPtr()->getStaticDims()[0] : batchToProcess();
    execPtr->exec(srcData, dstData, MB);
}

void MKLDNNSpaceToDepthNode::executeDynamicImpl(mkldnn::stream strm) {
    execute(strm);
}

bool MKLDNNSpaceToDepthNode::created() const {
    return getType() == SpaceToDepth;
}
REG_MKLDNN_PRIM_FOR(MKLDNNSpaceToDepthNode, SpaceToDepth);