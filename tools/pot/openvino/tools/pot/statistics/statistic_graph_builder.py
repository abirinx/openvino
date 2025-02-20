# Copyright (C) 2020-2021 Intel Corporation
# SPDX-License-Identifier: Apache-2.0

from copy import deepcopy
from mo.ops.op import Op
from mo.ops.result import Result
from mo.front.tf.graph_utils import create_op_node_with_second_input
from mo.front.common.partial_infer.utils import int64_array

try:
    from extensions.back.add_outputs_recursive import AddOutputRecursive
except ImportError:
    pass  # we try to import AddOutputRecursive for subgraphs quantization

from extensions.ops.ReduceOps import ReduceMin, ReduceMax, ReduceMean
from extensions.ops.activation_ops import Abs

from ..graph.model_utils import get_node_by_name
from ..graph.node_utils import get_output_shape, reset_node_fullname
from ..statistics.statistics import Statistic, TensorStatistic, TensorStatisticAxis
from ..statistics.function_selector import ACTIVATIONS, get_stats_function


class StatisticGraphBuilder:
    def insert_statistic(self, model, stats_layout, stat_aliases=None):
        output_to_node_names = {}
        if stat_aliases is None or model is None:
            return model, list(stats_layout.keys()), output_to_node_names
        nodes_names = []
        copy_stat_aliases = deepcopy(stat_aliases)
        for algo_name, node_stats in copy_stat_aliases.items():
            for node_name, stats in node_stats.items():
                node_name_in_graph = node_name[0] if isinstance(node_name, tuple) else node_name
                node_name_in_graph = node_name_in_graph.replace('/pre_fq_input', '')
                node = get_node_by_name(model, node_name_in_graph)
                node_in_main_graph = get_node_by_name(model, node_name_in_graph.split('|')[0])
                model_graph = node_in_main_graph.graph
                for stat, _ in list(stats.items()):
                    if not isinstance(stat, Statistic) or not stat.kwargs.get('inplace_statistics', False):
                        if node_name_in_graph not in nodes_names:
                            nodes_names.append(node_name_in_graph)
                        continue
                    type_stat = stat.kwargs['type']
                    add_output_node, op_name = getattr(self, f'insert_{type_stat}')(model_graph,
                                                                                    node,
                                                                                    type_stat,
                                                                                    node.name,
                                                                                    **stat.kwargs)
                    if add_output_node:
                        if node_name_in_graph not in nodes_names:
                            nodes_names.append(op_name)
                        class_statistic = TensorStatistic if isinstance(stat, TensorStatistic) else TensorStatisticAxis
                        fn = get_stats_function(ACTIVATIONS, type_stat, stat.kwargs.get('granularity'),
                                                'compute_statistic')
                        new_stat = class_statistic(fn,
                                                   channel=stat.kwargs.get('channel', {}),
                                                   inplace_statistics=False,
                                                   granularity=stat.kwargs.get('granularity'),
                                                   type=type_stat,
                                                   layer_stat_name=op_name)
                    else:
                        new_stat = deepcopy(stat)
                        new_stat.kwargs['layer_stat_name'] = op_name

                    del stats_layout[node_name][stat]
                    stats_layout[node_name][new_stat] = new_stat

                    stat_name = stat_aliases[algo_name][node_name][stat]
                    del stat_aliases[algo_name][node_name][stat]
                    stat_aliases[algo_name][node_name][new_stat] = stat_name

                # add output if node in subgraph
                if model_graph != node.graph:
                    if node_name_in_graph in nodes_names:
                        nodes_names.remove(node_name_in_graph)

                    # Don't need adding extra output to the same node, but for another algo
                    if node_name_in_graph in output_to_node_names.values():
                        result_name = next((result for result, node in output_to_node_names.items()
                                            if node == node_name_in_graph))
                    else:
                        model_graph.graph['additional_outputs'] = node_name_in_graph.split('|')
                        results = AddOutputRecursive().find_and_replace_pattern(model_graph)
                        assert len(results) == 1
                        result_name = results[0].name
                    if node_name in stats_layout:
                        stats_layout[result_name] = stats_layout.pop(node_name)
                    stat_aliases[algo_name][result_name] = stat_aliases[algo_name].pop(node_name)
                    output_to_node_names[result_name] = node_name_in_graph

        return model, nodes_names, output_to_node_names

    def insert_reduce(self, model_graph, insert_op, node, granularity, type_stat, node_name, axis=1):
        axis_const = self.find_axis(node, granularity, axis)
        if isinstance(axis_const, str):
            return (True, node.name)
        reduce_op = create_op_node_with_second_input(node.graph, insert_op, int64_array(axis_const),
                                                     dict(name=f'{type_stat}_{node_name}'))
        reduce_op['fullname'] = reset_node_fullname(node.fullname, reduce_op.name)
        if node.graph != model_graph:
            Op.create_data_node(reduce_op.graph, reduce_op, {'shape': [1]})
        node.out_port(0).connect(reduce_op.in_port(0))
        return self.insert_result(model_graph, node, reduce_op, type_stat)

    def insert_min(self, model_graph, node, type_stat, node_name, **kwargs):
        return self.insert_reduce(model_graph, ReduceMin, node, kwargs.get('granularity'), type_stat, node_name)

    def insert_max(self, model_graph, node, type_stat, node_name, **kwargs):
        return self.insert_reduce(model_graph, ReduceMax, node, kwargs.get('granularity'), type_stat, node_name)

    def insert_mean(self, model_graph, node, type_stat, node_name, **kwargs):
        axis_channel = kwargs.get('channel', None).get(node.name, 1)
        return self.insert_reduce(model_graph,
                                  ReduceMean,
                                  node,
                                  kwargs.get('granularity'),
                                  type_stat,
                                  node_name,
                                  axis_channel)

    def insert_abs_max(self, model_graph, node, type_stat, node_name, **kwargs):
        axis_const = self.find_axis(node, kwargs.get('granularity'))
        if isinstance(axis_const, str):
            return (True, node.name)
        abs_node = Abs(node.graph, {"name": f'abs_{node_name}'}).create_node_with_data([node.out_node(0)]).in_node(0)
        max_op = create_op_node_with_second_input(node.graph, ReduceMax, int64_array(axis_const),
                                                  dict(name=f'{type_stat}_{node_name}'))

        if node.graph != model_graph:
            Op.create_data_node(max_op.graph, max_op, {'shape': [1]})
        max_op['fullname'] = reset_node_fullname(node.fullname, max_op.name)
        abs_node.out_port(0).connect(max_op.in_port(0))
        return self.insert_result(model_graph, node, max_op, type_stat)

    @staticmethod
    def insert_result(model_graph, node, child_node, name):
        if node.graph != model_graph:
            model_graph.graph['additional_outputs'] = child_node.fullname.split('|')
            res_op = AddOutputRecursive().find_and_replace_pattern(model_graph)
            ie_result_name = res_op[0].name
        else:
            ie_result_name = f'{name}_{node.name}'
            res_op = Result(node.graph, {'name': f'Result_{ie_result_name}'}).create_node()
            child_node.out_port(0).connect(res_op.in_port(0))
        return (False, ie_result_name)

    @staticmethod
    def find_axis(node, granularity, axis=1):
        shape = len(get_output_shape(node, 0))
        if shape < 3 and granularity == 'perchannel':
            return node.name
        axis_const = list(i for i in range(shape))
        if granularity == 'perchannel':
            axis_const.pop(axis)
            axis_const.pop(0)
        return axis_const
