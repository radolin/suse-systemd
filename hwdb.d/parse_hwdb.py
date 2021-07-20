#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# This file is distributed under the MIT license, see below.
#
# The MIT License (MIT)
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

import glob
import string
import sys
import os

try:
    from pyparsing import (Word, White, Literal, ParserElement, Regex, LineEnd,
                           OneOrMore, Combine, Or, Optional, Suppress, Group,
                           nums, alphanums, printables,
                           stringEnd, pythonStyleComment,
                           ParseBaseException, __diag__)
except ImportError:
    print('pyparsing is not available')
    sys.exit(77)

try:
    from evdev.ecodes import ecodes
except ImportError:
    ecodes = None
    print('WARNING: evdev is not available')

try:
    from functools import lru_cache
except ImportError:
    # don't do caching on old python
    lru_cache = lambda: (lambda f: f)

__diag__.warn_multiple_tokens_in_named_alternation = True
__diag__.warn_ungrouped_named_tokens_in_collection = True
__diag__.warn_name_set_on_empty_Forward = True
__diag__.warn_on_multiple_string_args_to_oneof = True
__diag__.enable_debug_on_named_expressions = True

EOL = LineEnd().suppress()
EMPTYLINE = LineEnd()
COMMENTLINE = pythonStyleComment + EOL
INTEGER = Word(nums)
REAL = Combine((INTEGER + Optional('.' + Optional(INTEGER))) ^ ('.' + INTEGER))
SIGNED_REAL = Combine(Optional(Word('-+')) + REAL)
UDEV_TAG = Word(string.ascii_uppercase, alphanums + '_')

# Those patterns are used in type-specific matches
TYPES = {'mouse':    ('usb', 'bluetooth', 'ps2', '*'),
         'evdev':    ('name', 'atkbd', 'input'),
         'fb':       ('pci'),
         'id-input': ('modalias'),
         'touchpad': ('i8042', 'rmi', 'bluetooth', 'usb'),
         'joystick': ('i8042', 'rmi', 'bluetooth', 'usb'),
         'keyboard': ('name', ),
         'sensor':   ('modalias', ),
        }

# Patterns that are used to set general properties on a device
GENERAL_MATCHES = {'acpi',
                   'bluetooth',
                   'usb',
                   'pci',
                   'sdio',
                   'vmbus',
                   'OUI',
                   }

def upperhex_word(length):
    return Word(nums + 'ABCDEF', exact=length)

@lru_cache()
def hwdb_grammar():
    ParserElement.setDefaultWhitespaceChars('')

    prefix = Or(category + ':' + Or(conn) + ':'
                for category, conn in TYPES.items())

    matchline_typed = Combine(prefix + Word(printables + ' ' + '®'))
    matchline_general = Combine(Or(GENERAL_MATCHES) + ':' + Word(printables + ' ' + '®'))
    matchline = (matchline_typed | matchline_general) + EOL

    propertyline = (White(' ', exact=1).suppress() +
                    Combine(UDEV_TAG - '=' - Optional(Word(alphanums + '_=:@*.!-;, "/'))
                            - Optional(pythonStyleComment)) +
                    EOL)
    propertycomment = White(' ', exact=1) + pythonStyleComment + EOL

    group = (OneOrMore(matchline('MATCHES*') ^ COMMENTLINE.suppress()) -
             OneOrMore(propertyline('PROPERTIES*') ^ propertycomment.suppress()) -
             (EMPTYLINE ^ stringEnd()).suppress())
    commentgroup = OneOrMore(COMMENTLINE).suppress() - EMPTYLINE.suppress()

    grammar = OneOrMore(Group(group)('GROUPS*') ^ commentgroup) + stringEnd()

    return grammar

@lru_cache()
def property_grammar():
    ParserElement.setDefaultWhitespaceChars(' ')

    dpi_setting = Group(Optional('*')('DEFAULT') + INTEGER('DPI') + Suppress('@') + INTEGER('HZ'))('SETTINGS*')
    mount_matrix_row = SIGNED_REAL + ',' + SIGNED_REAL + ',' + SIGNED_REAL
    mount_matrix = Group(mount_matrix_row + ';' + mount_matrix_row + ';' + mount_matrix_row)('MOUNT_MATRIX')
    xkb_setting = Optional(Word(alphanums + '+-/@._'))

    props = (('MOUSE_DPI', Group(OneOrMore(dpi_setting))),
             ('MOUSE_WHEEL_CLICK_ANGLE', INTEGER),
             ('MOUSE_WHEEL_CLICK_ANGLE_HORIZONTAL', INTEGER),
             ('MOUSE_WHEEL_CLICK_COUNT', INTEGER),
             ('MOUSE_WHEEL_CLICK_COUNT_HORIZONTAL', INTEGER),
             ('ID_AUTOSUSPEND', Or((Literal('0'), Literal('1')))),
             ('ID_INPUT', Or((Literal('0'), Literal('1')))),
             ('ID_INPUT_ACCELEROMETER', Or((Literal('0'), Literal('1')))),
             ('ID_INPUT_JOYSTICK', Or((Literal('0'), Literal('1')))),
             ('ID_INPUT_KEY', Or((Literal('0'), Literal('1')))),
             ('ID_INPUT_KEYBOARD', Or((Literal('0'), Literal('1')))),
             ('ID_INPUT_MOUSE', Or((Literal('0'), Literal('1')))),
             ('ID_INPUT_POINTINGSTICK', Or((Literal('0'), Literal('1')))),
             ('ID_INPUT_SWITCH', Or((Literal('0'), Literal('1')))),
             ('ID_INPUT_TABLET', Or((Literal('0'), Literal('1')))),
             ('ID_INPUT_TABLET_PAD', Or((Literal('0'), Literal('1')))),
             ('ID_INPUT_TOUCHPAD', Or((Literal('0'), Literal('1')))),
             ('ID_INPUT_TOUCHSCREEN', Or((Literal('0'), Literal('1')))),
             ('ID_INPUT_TRACKBALL', Or((Literal('0'), Literal('1')))),
             ('POINTINGSTICK_SENSITIVITY', INTEGER),
             ('POINTINGSTICK_CONST_ACCEL', REAL),
             ('ID_INPUT_JOYSTICK_INTEGRATION', Or(('internal', 'external'))),
             ('ID_INPUT_TOUCHPAD_INTEGRATION', Or(('internal', 'external'))),
             ('XKB_FIXED_LAYOUT', xkb_setting),
             ('XKB_FIXED_VARIANT', xkb_setting),
             ('XKB_FIXED_MODEL', xkb_setting),
             ('KEYBOARD_LED_NUMLOCK', Literal('0')),
             ('KEYBOARD_LED_CAPSLOCK', Literal('0')),
             ('ACCEL_MOUNT_MATRIX', mount_matrix),
             ('ACCEL_LOCATION', Or(('display', 'base'))),
             ('PROXIMITY_NEAR_LEVEL', INTEGER),
             ('ID_TAG_MASTER_OF_SEAT', Literal('1')),
            )
    fixed_props = [Literal(name)('NAME') - Suppress('=') - val('VALUE')
                   for name, val in props]
    kbd_props = [Regex(r'KEYBOARD_KEY_[0-9a-f]+')('NAME')
                 - Suppress('=') -
                 ('!' ^ (Optional('!') - Word(alphanums + '_')))('VALUE')
                ]
    abs_props = [Regex(r'EVDEV_ABS_[0-9a-f]{2}')('NAME')
                 - Suppress('=') -
                 Word(nums + ':')('VALUE')
                ]

    grammar = Or(fixed_props + kbd_props + abs_props) + EOL

    return grammar

ERROR = False
def error(fmt, *args, **kwargs):
    global ERROR
    ERROR = True
    print(fmt.format(*args, **kwargs))

def convert_properties(group):
    matches = [m[0] for m in group.MATCHES]
    props = [p[0] for p in group.PROPERTIES]
    return matches, props

def parse(fname):
    grammar = hwdb_grammar()
    try:
        with open(fname, 'r', encoding='UTF-8') as f:
            parsed = grammar.parseFile(f)
    except ParseBaseException as e:
        error('Cannot parse {}: {}', fname, e)
        return []
    return [convert_properties(g) for g in parsed.GROUPS]

def check_matches(groups):
    matches = sum((group[0] for group in groups), [])

    # This is a partial check. The other cases could be also done, but those
    # two are most commonly wrong.
    grammars = { 'usb' : 'v' + upperhex_word(4) + Optional('p' + upperhex_word(4)),
                 'pci' : 'v' + upperhex_word(8) + Optional('d' + upperhex_word(8)),
    }

    for match in matches:
        prefix, rest = match.split(':', maxsplit=1)
        gr = grammars.get(prefix)
        if gr:
            try:
                gr.parseString(rest)
            except ParseBaseException as e:
                error('Pattern {!r} is invalid: {}', rest, e)
                continue
            if rest[-1] not in '*:':
                error('pattern {} does not end with "*" or ":"', match)

    matches.sort()
    prev = None
    for match in matches:
        if match == prev:
            error('Match {!r} is duplicated', match)
        prev = match

def check_one_default(prop, settings):
    defaults = [s for s in settings if s.DEFAULT]
    if len(defaults) > 1:
        error('More than one star entry: {!r}', prop)

def check_one_mount_matrix(prop, value):
    numbers = [s for s in value if s not in {';', ','}]
    if len(numbers) != 9:
        error('Wrong accel matrix: {!r}', prop)
    try:
        numbers = [abs(float(number)) for number in numbers]
    except ValueError:
        error('Wrong accel matrix: {!r}', prop)
    bad_x, bad_y, bad_z = max(numbers[0:3]) == 0, max(numbers[3:6]) == 0, max(numbers[6:9]) == 0
    if bad_x or bad_y or bad_z:
        error('Mount matrix is all zero in {} row: {!r}',
              'x' if bad_x else ('y' if bad_y else 'z'),
              prop)

def check_one_keycode(prop, value):
    if value != '!' and ecodes is not None:
        key = 'KEY_' + value.upper()
        if not (key in ecodes or
                value.upper() in ecodes or
                 # new keys added in kernel 5.5
                'KBD_LCD_MENU' in key):
            error('Keycode {} unknown', key)

def check_wheel_clicks(properties):
    pairs = (('MOUSE_WHEEL_CLICK_COUNT_HORIZONTAL', 'MOUSE_WHEEL_CLICK_COUNT'),
             ('MOUSE_WHEEL_CLICK_ANGLE_HORIZONTAL', 'MOUSE_WHEEL_CLICK_ANGLE'),
             ('MOUSE_WHEEL_CLICK_COUNT_HORIZONTAL', 'MOUSE_WHEEL_CLICK_ANGLE_HORIZONTAL'),
             ('MOUSE_WHEEL_CLICK_COUNT', 'MOUSE_WHEEL_CLICK_ANGLE'))
    for pair in pairs:
        if pair[0] in properties and pair[1] not in properties:
            error('{} requires {} to be specified', *pair)

def check_properties(groups):
    grammar = property_grammar()
    for matches, props in groups:
        seen_props = {}
        for prop in props:
            # print('--', prop)
            prop = prop.partition('#')[0].rstrip()
            try:
                parsed = grammar.parseString(prop)
            except ParseBaseException as e:
                error('Failed to parse: {!r}', prop)
                continue
            # print('{!r}'.format(parsed))
            if parsed.NAME in seen_props:
                error('Property {} is duplicated', parsed.NAME)
            seen_props[parsed.NAME] = parsed.VALUE
            if parsed.NAME == 'MOUSE_DPI':
                check_one_default(prop, parsed.VALUE.SETTINGS)
            elif parsed.NAME == 'ACCEL_MOUNT_MATRIX':
                check_one_mount_matrix(prop, parsed.VALUE)
            elif parsed.NAME.startswith('KEYBOARD_KEY_'):
                val = parsed.VALUE if isinstance(parsed.VALUE, str) else parsed.VALUE[0]
                check_one_keycode(prop, val)

        check_wheel_clicks(seen_props)

def print_summary(fname, groups):
    n_matches = sum(len(matches) for matches, props in groups)
    n_props = sum(len(props) for matches, props in groups)
    print('{}: {} match groups, {} matches, {} properties'
          .format(fname, len(groups), n_matches, n_props))

    if n_matches == 0 or n_props == 0:
        error('{}: no matches or props'.format(fname))

if __name__ == '__main__':
    args = sys.argv[1:] or sorted(glob.glob(os.path.dirname(sys.argv[0]) + '/[67][0-9]-*.hwdb'))

    for fname in args:
        groups = parse(fname)
        print_summary(fname, groups)
        check_matches(groups)
        check_properties(groups)

    sys.exit(ERROR)
