#!/usr/bin/env python
# Copyright (c) 2012 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import cStringIO
import difflib
import os
import sys
import unittest

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
BUILD_TOOLS_DIR = os.path.dirname(SCRIPT_DIR)

sys.path.append(BUILD_TOOLS_DIR)
import easy_template

class EasyTemplateTestCase(unittest.TestCase):
  def _RunTest(self, template, expected, template_dict):
    src = cStringIO.StringIO(template)
    dst = cStringIO.StringIO()
    easy_template.RunTemplate(src, dst, template_dict)
    if dst.getvalue() != expected:
      expected_lines = expected.splitlines(1)
      actual_lines = dst.getvalue().splitlines(1)
      diff = ''.join(difflib.unified_diff(
        expected_lines, actual_lines,
        fromfile='expected', tofile='actual'))
      self.fail('Unexpected output:\n' + diff)

  def testEmpty(self):
    self._RunTest('', '', {})

  def testNewlines(self):
    self._RunTest('\n\n', '\n\n', {})

  def testNoInterpolation(self):
    template = """I love paris in the
    the springtime [don't you?]
    {this is not interpolation}.
    """
    self._RunTest(template, template, {})

  def testSimpleInterpolation(self):
    self._RunTest(
        '{{foo}} is my favorite number',
        '42 is my favorite number',
        {'foo': 42})

  def testLineContinuations(self):
    template = "Line 1 \\\nLine 2\n"""
    self._RunTest(template, template, {})

  def testIfStatement(self):
    template = r"""
[[if foo:]]
  foo
[[else:]]
  not foo
[[]]"""
    self._RunTest(template, "\n  foo\n", {'foo': True})
    self._RunTest(template, "\n  not foo\n", {'foo': False})

  def testForStatement(self):
    template = r"""[[for beers in [99, 98, 1]:]]
{{beers}} bottle{{(beers != 1) and 's' or ''}} of beer on the wall...
[[]]"""
    expected = r"""99 bottles of beer on the wall...
98 bottles of beer on the wall...
1 bottle of beer on the wall...
"""
    self._RunTest(template, expected, {})

  def testListVariables(self):
    template = r"""
[[for i, item in enumerate(my_list):]]
{{i+1}}: {{item}}
[[]]
"""
    self._RunTest(template, "\n1: Banana\n2: Grapes\n3: Kumquat\n",
        {'my_list': ['Banana', 'Grapes', 'Kumquat']})

  def testListInterpolation(self):
    template = "{{', '.join(growing[0:-1]) + ' and ' + growing[-1]}} grow..."
    self._RunTest(template, "Oats, peas, beans and barley grow...",
        {'growing': ['Oats', 'peas', 'beans', 'barley']})
    self._RunTest(template, "Love and laughter grow...",
        {'growing': ['Love', 'laughter']})

  def testComplex(self):
    template = r"""
struct {{name}} {
[[for field in fields:]]
[[  if field['type'] == 'array':]]
  {{field['basetype']}} {{field['name']}}[{{field['size']}}];
[[  else:]]
  {{field['type']}} {{field['name']}};
[[  ]]
[[]]
};"""
    expected = r"""
struct Foo {
  std::string name;
  int problems[99];
};"""
    self._RunTest(template, expected, {
      'name': 'Foo',
      'fields': [
        {'name': 'name', 'type': 'std::string'},
        {'name': 'problems', 'type': 'array', 'basetype': 'int', 'size': 99}]})


if __name__ == '__main__':
  unittest.main()
