// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <legacy/graph_tools.hpp>
#include <legacy/details/ie_cnn_network_tools.h>
#include "gna_data_types.hpp"
#include "gna_graph_tools.hpp"
#include "gna_plugin_log.hpp"
#include "gna_upstream_iterator.hpp"
#include "layers/gna_layer_info.hpp"

namespace GNAPluginNS {

/**
 * @brief searchs for a pattern: Permute(0,3,1,2) -> ... -> Convolution -> ... -> Permute(0,2,3,1) or
 *        Reshape -> ... -> Convolution -> ... -> Permute(0,2,3,1) if Convolution has only one input dimension not equal to 1
 * @param layer convolution layer
 * @return the found permutations before and after convolution
 */
inline std::pair<InferenceEngine::CNNLayerPtr, InferenceEngine::CNNLayerPtr> FindPermutationsAroundConvolutionInNHWCModel(
    InferenceEngine::CNNLayerPtr layer) {
    // Skip a convolution which doesn't have previous or next layers
    if (layer->outData.size() != 1) {
        return std::make_pair(nullptr, nullptr);
    }

    if (getInputTo(layer->outData.front()).empty()) {
        return std::make_pair(nullptr, nullptr);
    }

    if (!InferenceEngine::CNNNetHasPrevLayer(layer.get())) {
        return std::make_pair(nullptr, nullptr);
    }

    auto next = getInputTo(layer->outData.front()).begin()->second;
    // Permute is inserted before Reshape by MO in NHWC models, so we need to find either permute, or reshape, or output
    while (!LayerInfo(next).isPermute() && !LayerInfo(next).isNonFunctional() && !LayerInfo(next).isOutput() &&
           next->outData.size() == 1) {
        auto input_to = getInputTo(next->outData.front());
        if (input_to.size() != 1) break;
        next = input_to.begin()->second;
    }

    // Check if the found layer is NCHW to NHWC permute or has 1D data, if it's not just skip this convolution
    if (LayerInfo(next).isPermute()) {
        if (next->outData[0]->getLayout() != InferenceEngine::Layout::NCHW ||
            next->GetParamAsInts("order") != GetPermuteOrder(InferenceEngine::Layout::NCHW, InferenceEngine::Layout::NHWC)) {
            return std::make_pair(nullptr, nullptr);
        }
    } else if (LayerInfo(next).isReshape()) {
        if (next->outData.size() != 1) {
            return std::make_pair(nullptr, nullptr);
        }
        // Check if reshape is expected for this pattern:
        // the next layer has the both, height and width dimensions > 1
        if (next->outData[0]->getDims().size() != 4 ||
            GetDataDimSize(next->insData[0].lock(), InferenceEngine::DataDimName::H) != 1 ||
            GetDataDimSize(next->insData[0].lock(), InferenceEngine::DataDimName::W) != 1) {
            return std::make_pair(nullptr, nullptr);
        }
    } else {
        return std::make_pair(nullptr, nullptr);
    }

    // Permute is inserted after Reshape by MO in NHWC models, so we need to find either permute, or reshape, or input
    auto parent = InferenceEngine::CNNNetPrevLayer(layer);
    auto prev = parent;
    while (!LayerInfo(prev).isPermute() && !LayerInfo(prev).isNonFunctional() && !LayerInfo(prev).isInput() &&
           InferenceEngine::CNNNetHasPrevLayer(prev.get())) {
        prev = InferenceEngine::CNNNetPrevLayer(prev);
    }
    // Check if the found layer is NHWC to NCHW permute or has 1D data, if it's not just skip this convolution
    if (LayerInfo(prev).isPermute()) {
        if (prev->outData[0]->getLayout() != InferenceEngine::Layout::NCHW ||
            prev->GetParamAsInts("order") != GetPermuteOrder(InferenceEngine::Layout::NHWC, InferenceEngine::Layout::NCHW)) {
            return std::make_pair(nullptr, nullptr);
        }
    } else if (LayerInfo(prev).isReshape())  {
        if (parent->outData.size() != 1 || InferenceEngine::getInputTo(parent->outData[0]).size() != 1) {
            return std::make_pair(nullptr, nullptr);
        }
        // Check if reshape is expected for this pattern:
        // the previous layer has number of channels > 1 and one of height/width dimensions is also > 1
        if (parent->insData[0].lock()->getDims().size() != 4 ||
            GetDataDimSize(parent->outData[0], InferenceEngine::DataDimName::C) != 1 &&
            (GetDataDimSize(parent->outData[0], InferenceEngine::DataDimName::H) != 1 ||
             GetDataDimSize(parent->outData[0], InferenceEngine::DataDimName::W) != 1)) {
            return std::make_pair(nullptr, nullptr);
        }
    } else {
        return std::make_pair(nullptr, nullptr);
    }
    return std::make_pair(prev, next);
}

/**
 * @brief searches for a pattern Convolution -> ... -> Permute(0,3,2,1) -> ... -> ScaleShift | FullyConnected
 * @param layer convolution layer
 * @return the found permutation layer
 */
inline InferenceEngine::CNNLayerPtr FindPermutationAfterConvolutionInKaldiModel(InferenceEngine::CNNLayerPtr layer) {
    // Skip a convolution which doesn't have next layers
    if (layer->outData.size() != 1) {
        return nullptr;
    }

    if (getInputTo(layer->outData.front()).empty()) {
        return nullptr;
    }

    /* Permute is inserted between a convolution and a scaleshift|fullyconnected layer by MO in Kaldi models,
     * so we need to fing either permute, or fullyconnected, or scaleshift, or output, or reshape to 2D
     */
    auto next = getInputTo(layer->outData.front()).begin()->second;
    while (!LayerInfo(next).isPermute() && !LayerInfo(next).isFullyConnected() && !LayerInfo(next).isScaleShift() &&
           !LayerInfo(next).isOutput() &&
           (!LayerInfo(next).isNonFunctional() || next->outData[0]->getDims().size() == next->input()->getDims().size())) {
        next = getInputTo(next->outData.front()).begin()->second;
    }

    // Check if the found layer is NCHW to NWHC permute
    if (!LayerInfo(next).isPermute() || next->input()->getLayout() != InferenceEngine::Layout::NCHW ||
        next->GetParamAsInts("order") != std::vector<int>{0, 3, 2, 1}) {
        return nullptr;
    }

    return next;
}

/**
 * @brief identifies if a model must be converted to NHWC, it must not be neither NHWC, nor Kaldi
 * @param layers model sorted layers
 */
inline bool MustBeConvertedFromNCHWToNHWC(const std::vector<InferenceEngine::CNNLayerPtr> &layers) {
    for (auto& l : layers) {
        if (!LayerInfo(l).isConvolution()) continue;

        InferenceEngine::CNNLayerPtr next;
        std::tie(std::ignore, next) = FindPermutationsAroundConvolutionInNHWCModel(l);
        if (next != nullptr) return false;
        // If a convolution has only 1-dimension input and output we should skip it
        auto in_dims = l->insData.begin()->lock()->getDims();
        auto out_dims = l->outData.front()->getDims();
        if (std::count_if(std::begin(in_dims), std::end(in_dims), [](size_t dim) { return dim != 1; }) <= 1 &&
            std::count_if(std::begin(out_dims), std::end(out_dims), [](size_t dim) { return dim != 1; }) <= 1) {
            continue;
        }

        return FindPermutationAfterConvolutionInKaldiModel(l) == nullptr;
    }
    return false;
}

/**
 * @brief returns transposition information for a layer based on the previous convolution or pooling dimensions order
 * @param layer layer from which transposition info search must be started
 * @return bool value which identifies if transposition info is found and transposition information
 */
inline std::vector<TranspositionInfo> FindTranspositionInfoFromPrevLayers(InferenceEngine::CNNLayerPtr layer) {
    std::function<std::vector<TranspositionInfo>(InferenceEngine::CNNLayerPtr)> findTranspositionInfoRecursive =
        [&findTranspositionInfoRecursive](InferenceEngine::CNNLayerPtr layer) -> std::vector<TranspositionInfo> {
        auto getTransposeInfoFromData = [](InferenceEngine::DataPtr data, bool transpose = true) {
            auto rows = InferenceEngine::GetDataDimSize(data, InferenceEngine::DataDimName::C);
            auto columns = InferenceEngine::GetDataDimSize(data, InferenceEngine::DataDimName::H) *
                           InferenceEngine::GetDataDimSize(data, InferenceEngine::DataDimName::W);
            return std::vector<TranspositionInfo>{{transpose, rows, columns}};
        };
        if (LayerInfo(layer).isConvolution() || LayerInfo(layer).isPooling()) {
            return getTransposeInfoFromData(layer->outData[0]);
        }

        /* If a fullyconnected or input layers are reached, it means that transposition isn't needed, but we should keep
         * its output size to skip this part during transposition if transposed layer is a result of concatination */
        if (LayerInfo(layer).isFullyConnected() || LayerInfo(layer).isInput()) {
            auto out_dims = layer->outData[0]->getDims();
            return {{false, 1, InferenceEngine::details::product(std::begin(out_dims), std::end(out_dims))}};
        }

        // If an eltwise is reached we should follow only one not-const direction
        if (LayerInfo(layer).isEltwise()) {
            auto input1 = InferenceEngine::CNNNetPrevLayer(layer, 0);
            auto input2 = InferenceEngine::CNNNetPrevLayer(layer, 1);
            if (LayerInfo(input1).isConst()) return findTranspositionInfoRecursive(input2);
            return findTranspositionInfoRecursive(input1);
        }

        /* If it's a concat along not channel axis and its inputs are transposed the whole concat output must be transposed,
         * otherwise every part corresponding to some input must be transposed separately */
        if (LayerInfo(layer).isConcat() && !layer->insData.empty())  {
            auto concatLayer = LayerInfo(layer).as<InferenceEngine::ConcatLayer*>();
            IE_ASSERT(concatLayer != nullptr);
            if (concatLayer->_axis > 1) {
                for (const auto& input : layer->insData) {
                    auto in_dims = input.lock()->getDims();
                    if (in_dims.size() <= 2) {
                        THROW_GNA_EXCEPTION << layer->name << " Invalid number of input dimensions " << in_dims.size()
                                            << " for a concat with axis=" << concatLayer->_axis;
                    }
                    if (concatLayer->_axis == in_dims.size() - 1 && in_dims[in_dims.size() - 2] > 1) {
                        std::ostringstream in_dims_oss;
                        std::copy(in_dims.begin(), in_dims.end(), std::ostream_iterator<size_t>(in_dims_oss, ","));
                        THROW_GNA_EXCEPTION << layer->name << " Unsupported concatenation axis=" << concatLayer->_axis
                                            << " for input dimensions: " << in_dims_oss.str();
                    }
                }
                // Check if non-const inputs are transposed
                bool transpose = false;
                int nonConstInputIx = 0;
                for (int i = 0; InferenceEngine::CNNNetHasPrevLayer(layer.get(), i); ++i) {
                    auto input = InferenceEngine::CNNNetPrevLayer(layer, i);
                    if (LayerInfo(input).isConst()) continue;
                    auto transpositionInfo = FindTranspositionInfoFromPrevLayers(input);
                    auto partToTranspose = std::find_if(std::begin(transpositionInfo), std::end(transpositionInfo),
                        [](const TranspositionInfo &infoPart) { return infoPart.transpose; });
                    bool inputTranspose = (partToTranspose != std::end(transpositionInfo));
                    if (nonConstInputIx == 0) {
                        transpose = inputTranspose;
                    } else if (inputTranspose != transpose) {
                        THROW_GNA_EXCEPTION << layer->name << " concat has inputs with different layouts";
                    }
                    ++nonConstInputIx;
                }
                return getTransposeInfoFromData(layer->outData[0], transpose);
            }
        }

        std::vector<TranspositionInfo> transpositionInfo;
        for (int idx = 0; idx < layer->insData.size(); ++idx) {
            if (!InferenceEngine::CNNNetHasPrevLayer(layer.get(), idx)) continue;
            auto inputLayer = InferenceEngine::CNNNetPrevLayer(layer, idx);
            if (LayerInfo(inputLayer).isSplit()) {
                // If we found split it's not possible to rotate data
                auto in_dims = layer->insData[idx].lock()->getDims();
                transpositionInfo.push_back({false, 1, InferenceEngine::details::product(std::begin(in_dims), std::end(in_dims))});
            } else if (LayerInfo(layer).isConcat() && LayerInfo(inputLayer).isConst()) {
                auto in_dims = layer->insData[idx].lock()->getDims();
                // We should keep its size to skip this part during transposition
                auto data_size = InferenceEngine::details::product(std::begin(in_dims), std::end(in_dims));
                transpositionInfo.push_back({false, 1, data_size});
            } else {
                std::vector<TranspositionInfo> results = findTranspositionInfoRecursive(inputLayer);
                transpositionInfo.insert(std::end(transpositionInfo), std::begin(results), std::end(results));
            }
        }
        return transpositionInfo;
    };
    return findTranspositionInfoRecursive(layer);
}

/**
 * @brief returns transposition information for a layer based on the next convolution layer dimensions order
 * @param layer layer from which transposition info search must be started
 * @return bool value which identifies if transposition info is found and transposition information
 */
inline std::vector<TranspositionInfo> FindTranspositionInfoFromNextLayers(InferenceEngine::CNNLayerPtr layer) {
    std::function<std::vector<TranspositionInfo>(InferenceEngine::CNNLayerPtr)> findTranspositionInfoRecursive =
        [&findTranspositionInfoRecursive](InferenceEngine::CNNLayerPtr layer) -> std::vector<TranspositionInfo> {
        if (LayerInfo(layer).isConvolution()) {
            auto rows = InferenceEngine::GetDataDimSize(layer->input(), InferenceEngine::DataDimName::C);
            auto columns = InferenceEngine::GetDataDimSize(layer->input(), InferenceEngine::DataDimName::H) *
                           InferenceEngine::GetDataDimSize(layer->input(), InferenceEngine::DataDimName::W);
            return {{true, rows, columns}};
        }

        /* If a fullyconnected or output layers are reached, it means that transposition isn't needed, but we should keep
         * its input size to skip this part during transposition if transposed layer is splitting */
        if (LayerInfo(layer).isFullyConnected() || layer->outData.empty()) {
            auto in_dims = layer->input()->getDims();
            return {{false, 1, InferenceEngine::details::product(std::begin(in_dims), std::end(in_dims))}};
        }

        std::vector<TranspositionInfo> transpositionInfo;
        for (const auto &output : layer->outData) {
            if (getInputTo(output).empty()) {
                auto out_dims = output->getDims();
                transpositionInfo.push_back({false, 1, InferenceEngine::details::product(std::begin(out_dims), std::end(out_dims))});
                continue;
            }
            std::vector<TranspositionInfo> results;
            // Return transposition info from the first branch where convolution is found
            for (const auto &inputTo : getInputTo(output)) {
                if (LayerInfo(inputTo.second).isConcat()) {
                    // If we found concat it's not possible to rotate data
                    auto out_dims = output->getDims();
                    results = {{false, 1, InferenceEngine::details::product(std::begin(out_dims), std::end(out_dims))}};
                } else {
                    results = findTranspositionInfoRecursive(inputTo.second);
                }
                auto found = std::find_if(std::begin(results), std::end(results), [](const TranspositionInfo & result) {
                    return result.transpose;
                });
                if (found != std::end(results)) break;
            }
            if (results.empty()) {
                THROW_GNA_EXCEPTION << layer->name << " Failed to find transposition info";
            }
            transpositionInfo.insert(std::end(transpositionInfo), std::begin(results), std::end(results));
        }

        if (LayerInfo(layer).isCrop()) {
            auto in_dims = layer->input()->getDims();
            auto in_total_size = InferenceEngine::details::product(std::begin(in_dims), std::end(in_dims));
            auto crop_layer = LayerInfo(layer).as<const InferenceEngine::CropLayer*>();
            IE_ASSERT(crop_layer != nullptr);
            size_t crop_offset = 1;
            size_t crop_out_size = 1;
            bool first_cropped_dim = true;
            for (int i = 0; i < crop_layer->axis.size(); ++i) {
                if (crop_layer->offset[i] == 0 && crop_layer->dim[i] == in_dims[i]) continue;
                crop_offset *= first_cropped_dim ? crop_layer->offset[i] : crop_layer->dim[i];
                crop_out_size *= crop_layer->dim[i];
                first_cropped_dim = false;
            }
            auto crop_rest_size = in_total_size - crop_offset - crop_out_size;
            if (crop_offset > 0) {
                transpositionInfo.insert(std::begin(transpositionInfo), {false, 1, crop_offset});
            }
            if (crop_rest_size > 0) {
                transpositionInfo.push_back({false, 1, crop_rest_size});
            }
        }
        return transpositionInfo;
    };

    return findTranspositionInfoRecursive(layer);
}

} // namespace GNAPluginNS