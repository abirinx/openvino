# Copyright (C) 2018-2021 Intel Corporation
# SPDX-License-Identifier: Apache-2.0

import numpy as np

from mo.front.common.partial_infer.utils import get_shape_from_slice, shape_array, dynamic_dimension_value, \
    dynamic_dimension, is_dynamic_slice
from mo.graph.graph import Node, Graph
from mo.ops.op import Op
from mo.utils.error import Error

"""
Slicing operations have different semantic or different parameters/inputs in different frameworks. To distinguish them 
several internal operations are introduced. The internal MO Slice operation behaves same as Slice in ONNX opset >= 10. 
A number of transformations take place on the front phase to convert framework slicing:
 - AttributedSlice, TFSlice -> Slice 
 - CaffeSlice -> Split 
 - MXSlice -> StridedSlice 
"""


class AttributedSlice(Op):
    """
    AttributedSlice is used in old versions of ONNX models (opset version < 10).
    Is replaced with internal Slice on the front phase.
    """
    op = 'AttributedSlice'
    enabled = False

    def __init__(self, graph: Graph, attrs: dict):
        super().__init__(graph, {
            'type': None,
            'op': self.op,
            'in_ports_count': 1,
            'out_ports_count': 1,
            'infer': None,
        }, attrs)


class CaffeSlice(Op):
    """
    Slice in Caffe is equivalent to Split operation in OpenVINO.
    https://caffe.berkeleyvision.org/tutorial/layers/slice.html
    Is replaced with Split from opset on the front phase.
    """
    op = 'CaffeSlice'
    enabled = False

    def __init__(self, graph: Graph, attrs: dict):
        super().__init__(graph, {
            'type': None,
            'op': self.op,
            'in_ports_count': 1,
            'out_ports_count': 1,
            'infer': None,
        }, attrs)



class TFSlice(Op):
    """
    TFSlice differs from Slice in ONNX, Caffe and MXNet.
    TFSlice has 'begin' and 'size' inputs while Slice has 'start', 'end', 'step', and 'axis' inputs.
    https://www.tensorflow.org/api_docs/python/tf/slice
    Is replaced with internal Slice op on the front phase.
    """
    op = 'TFSlice'
    enabled = False

    def __init__(self, graph: Graph, attrs: dict):
        super().__init__(graph, {
            'type': None,
            'op': self.op,
            'in_ports_count': 3,
            'out_ports_count': 1,
            'infer': None,
        }, attrs)


class MXSlice(Op):
    """
    Slice operation in MXNet is different from ONNX, Caffe, Tensorflow. It has begin, end & step attributes
    https://mxnet.apache.org/versions/1.6/api/python/docs/api/symbol/op/index.html#mxnet.symbol.op.slice
    Is replaced with the StridedSlice from opset on the front phase.
    """
    op = 'MXSlice'
    enabled = False

    def __init__(self, graph: Graph, attrs: dict):
        super().__init__(graph, {
            'kind': 'op',
            'type': None,
            'op': self.op,
            'in_ports_count': 1,
            'out_ports_count': 1,
            'infer': None
        }, attrs)


class Slice(Op):
    """
    Semantic of Slice is identical to Slice in ONNX opset >= 10.
    It has 'starts', 'ends', 'steps', and 'axes' inputs.
    SliceConverter replaces it with StridedSlice from opset.
    """
    op = 'Slice'
    enabled = False

    def __init__(self, graph: Graph, attrs: dict = None):
        super().__init__(graph, {
            'type': None,
            'op': 'Slice',
            'in_ports_count': 5,
            'out_ports_count': 1,
            'infer': self.infer
        }, attrs)

    @staticmethod
    def infer(node: Node):
        input_value = node.in_port(0).data.get_value()
        input_shape = node.in_port(0).data.get_shape()

        starts = node.in_port(1).data.get_value()
        ends = node.in_port(2).data.get_value()
        if node.is_in_port_connected(4):
            steps = node.in_port(4).data.get_value()
        else:
            steps = np.ones(len(starts), dtype=np.int64)

        if node.is_in_port_connected(3):
            axes = node.in_port(3).data.get_value()
        else:
            axes = [x for x in range(len(starts))]

        if starts is None or ends is None or steps is None or axes is None:
            node.out_port(0).data.set_shape(shape_array([dynamic_dimension_value] * len(input_shape)))
            return

        slice_idx = [slice(0, in_shape, 1) for in_shape in input_shape]
        for i in range(len(axes)):
            # Ranged for output value for specified axis
            slice_idx[axes[i]] = slice(starts[i], ends[i], steps[i])
        if input_value is None or any(is_dynamic_slice(s) for s in slice_idx):
            output_shape = get_shape_from_slice(input_shape, slice_idx)
            node.out_port(0).data.set_shape(output_shape)
        else:
            node.out_port(0).data.set_value(input_value[tuple(slice_idx)])
