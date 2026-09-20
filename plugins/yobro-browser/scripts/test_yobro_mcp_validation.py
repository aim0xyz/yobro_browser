"""Validation must stop malformed tool calls before they reach the browser."""
import errno
import unittest
from unittest.mock import patch
import yobro_mcp as m

class ValidationTests(unittest.TestCase):
    def test_keyboard_mapping(self):
        args = {'ref': 'e1', 'document': 'doc', 'key': 'Enter'}
        self.assertEqual(m.tool_payload('press_key', args), {'command': 'press', **args})

    def test_invalid_calls_never_reach_socket(self):
        cases = [
            ('navigate', {'direction': 'close'}),
            ('navigate', {'direction': 'back', 'command': 'close'}),
            ('click', {'ref': 'e1'}),
            ('fill', {'ref': 'e1', 'document': 'doc', 'value': None}),
            ('scroll', {'amount': True}),
            ('scroll', {'amount': 5001}),
            ('press_key', {'ref': 'e1', 'document': 'doc', 'key': 'invalid'}),
            ('status', {'unexpected': 'value'}),
        ]
        with patch.object(m, 'yobro_request') as request:
            for name, args in cases:
                with self.subTest(name=name, args=args):
                    self.assertTrue(m.call_tool(name, args)['isError'])
            request.assert_not_called()

    def test_permission_error_is_not_misdiagnosed_as_stopped_browser(self):
        with patch.object(m.socket, 'socket', side_effect=PermissionError(errno.EPERM, 'Operation not permitted')):
            result = m.yobro_request({'command': 'status'})
        self.assertFalse(result['ok'])
        self.assertIn('sandbox', result['hint'])
        self.assertNotIn('Start YOBRO', result['hint'])

if __name__ == '__main__':
    unittest.main()
