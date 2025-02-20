# Copyright (C) 2018-2021 Intel Corporation
# SPDX-License-Identifier: Apache-2.0

import os
import unittest
from fnmatch import fnmatch
from generator import generator, generate

from mo.utils.custom_replacement_config import load_and_validate_json_config
from mo.utils.error import Error
from mo.utils.utils import get_mo_root_dir


def get_json_configs(mo_root_dir):
    config_path = os.path.join(mo_root_dir, 'extensions', 'front')
    pattern = "*.json"
    config_files_list = []
    for path, subdirs, files in os.walk(config_path):
        for name in files:
            if fnmatch(name, pattern):
                config_files_list.append((os.path.join(path, name),))
    return config_files_list

@generator
class TestSchema(unittest.TestCase):
    base_dir = get_mo_root_dir()
    schema_file = os.path.join(base_dir, 'mo', 'utils', 'schema.json')
    transformation_configs = get_json_configs(base_dir)
    test_json1 = '[{"id": "", "match_kind": "general", "custom_attributes": {}}]'
    test_json2 = '[{"id": "someid", "match_kind": "abc", "custom_attributes": {}}]'

    @generate(*transformation_configs)
    def test_schema_file(self, transformation_config):
        self.assertTrue(load_and_validate_json_config(transformation_config))

    def test_schema_id_empty(self):
        self.assertRaises(Error, load_and_validate_json_config, self.test_json1)

    def test_schema_match_kind_wrong(self):
        self.assertRaises(Error, load_and_validate_json_config, self.test_json2)
