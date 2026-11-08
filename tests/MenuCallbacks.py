#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Ethanfel and contributors
"""Check the callback contracts that can otherwise destroy Titanfall's UI VM.

This is a focused registration check, not a replacement for the game compiler.
The game's compiler requires AddMenuFooterOption parameter 5 to have signature
void functionref(var), whereas ModSettings_AddButton needs void functionref().
"""
from pathlib import Path
import re
import unittest


CONTRACTS = {
    'AddMenuFooterOption': (4, ('var',)),
    'AddButtonEventHandler': (2, ('var',)),
    'AddMenuEventHandler': (2, ()),
    'AddMenu': (2, ()),
    'ModSettings_AddButton': (1, ()),
}


def arguments(tokens, opening):
    """Split one call, preserving nested calls and quoted commas as arguments."""
    depth = 0
    parts, part = [], []
    for token in tokens[opening + 1:]:
        if token == ')' and depth == 0:
            if part:
                parts.append(part)
            return parts
        if token == ',' and depth == 0:
            parts.append(part)
            part = []
            continue
        if token in ('(', '[', '{'):
            depth += 1
        elif token in (')', ']', '}'):
            depth -= 1
        part.append(token)
    raise ValueError('Unterminated function call')


def check(text):
    tokens = [m.group() for m in re.finditer(
        r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/|[A-Za-z_]\w*|[^\s]', text, re.S)
        if not m.group().startswith(('//', '/*'))]
    signatures = {}
    for i, token in enumerate(tokens):
        if token == 'function' and i + 2 < len(tokens) and tokens[i + 2] == '(':
            signatures[tokens[i + 1]] = (tokens[i - 1], tuple(p[0] for p in arguments(tokens, i + 2)))
    errors, checked = [], 0
    for i, token in enumerate(tokens[:-1]):
        if token not in CONTRACTS or tokens[i + 1] != '(':
            continue
        args = arguments(tokens, i + 1)
        slot, expected = CONTRACTS[token]
        if len(args) <= slot or args[slot] == ['null']:
            continue
        if len(args[slot]) != 1 or args[slot][0] not in signatures:
            errors.append(f'{token}: callback is not a declared local function')
            continue
        callback = args[slot][0]
        checked += 1
        if signatures[callback] != ('void', expected):
            errors.append(f'{token}: {callback} has {signatures[callback]}, expected void {expected}')
    return errors, checked


class MenuCallbacks(unittest.TestCase):
    def test_shipped_menu_registration_contracts(self):
        script = Path(__file__).resolve().parents[1] / 'package/mods/Local.TF2VRNeuralRendering/mod/scripts/vscripts/ui/tf2vrnr_settings.nut'
        errors, checked = check(script.read_text())
        self.assertGreaterEqual(checked, 12, 'No meaningful menu registration coverage')
        self.assertEqual(errors, [])

    def test_reproduces_black_screen_footer_error(self):
        broken = '''void function Open() {}
        AddMenuFooterOption(GetMenu("M"), BUTTON_X,
            PrependControllerPrompts(BUTTON_X, "DLSS 5"), "DLSS 5", Open)'''
        errors, checked = check(broken)
        self.assertEqual(checked, 1)
        self.assertEqual(len(errors), 1)
        self.assertIn("expected void ('var',)", errors[0])

    def test_mod_settings_and_footer_require_distinct_signatures(self):
        valid = '''void function Open() {}
        void function OpenFooter(var button) { Open() }
        ModSettings_AddButton("Open, settings", Open)
        AddMenuFooterOption(menu, BUTTON_X, "Open", "Open", OpenFooter)'''
        self.assertEqual(check(valid), ([], 2))
        errors, _ = check(valid.replace('"Open, settings", Open)', '"Open, settings", OpenFooter)'))
        self.assertEqual(len(errors), 1)
        self.assertIn('ModSettings_AddButton', errors[0])

    def test_ignores_comments_and_optional_callbacks(self):
        self.assertEqual(check('''// AddMenuFooterOption(menu, X, "x", "x", Missing)
        AddMenuFooterOption(menu, BUTTON_B, "Back", "Back")
        AddMenuFooterOption(menu, BUTTON_A, "Select", "Select", null)'''), ([], 0))


if __name__ == '__main__':
    unittest.main()
