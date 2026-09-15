import sys
sys.dont_write_bytecode = True
import base64
from email.message import EmailMessage
import importlib.util
from pathlib import Path
import ssl
import unittest
from unittest.mock import MagicMock, patch

spec = importlib.util.spec_from_file_location('mailworker', Path(__file__).parents[1] / 'Sources/YOBRO/Resources/MailWorker.py')
worker = importlib.util.module_from_spec(spec); spec.loader.exec_module(worker)
ACCOUNT = dict(address='laura@example.org', username='laura@example.org', imapHost='imap.example.org', imapPort=993, smtpHost='smtp.example.org', smtpPort=465, smtpSecurity='tls')
RAW = b'''From: Alice <alice@example.org>\r
To: Laura <laura@example.org>\r
Subject: =?UTF-8?B?R3LDvMOfZSBhdXMgWU9CUk8=?=\r
Date: Sat, 5 Sep 2026 12:00:00 +0200\r
Message-ID: <test@example.org>\r
MIME-Version: 1.0\r
Content-Type: multipart/mixed; boundary=outer\r
\r
--outer\r
Content-Type: text/plain; charset=utf-8\r
Content-Transfer-Encoding: base64\r
\r
SGFsbG8gTGF1cmEsIGRpZXMgaXN0IGVpbiBUZXN0Lg==\r
--outer\r
Content-Type: application/pdf\r
Content-Disposition: attachment; filename="report.pdf"\r
\r
PDF\r
--outer--\r
'''


class MailTests(unittest.TestCase):
    def request(self, action, **extra):
        return dict(account=ACCOUNT.copy(), password='TEST-ONLY', action=action, **extra)

    def imap(self):
        connection = MagicMock()
        connection.select.return_value = ('OK', [b'1'])
        connection.response.return_value = ('UIDVALIDITY', [b'42'])
        return connection

    def test_mime(self):
        result = worker.body(RAW)
        self.assertEqual(result['text'], 'Hallo Laura, dies ist ein Test.')
        self.assertEqual([a['name'] for a in result['attachments']], ['report.pdf'])
        self.assertEqual(base64.b64decode(worker.attachment_data(RAW, result['attachments'][0]['id'])['data']), b'PDF')
        summary = worker.summary(worker.decode_message(RAW), '7', b'FLAGS (\\Seen)')
        self.assertEqual(summary['subject'], 'Grüße aus YOBRO')
        self.assertFalse(summary['unread'])

    def test_html_no_remote_or_script(self):
        raw = b'Content-Type: text/html; charset=utf-8\r\n\r\n<head><style>evil</style></head><p>Hello</p><script>secret()</script><img src="https://tracker.invalid/pixel"><p>World &amp; friends</p>'
        text = worker.body(raw)['text']
        self.assertIn('Hello', text); self.assertIn('World & friends', text)
        self.assertNotIn('secret', text); self.assertNotIn('tracker', text); self.assertNotIn('evil', text)

    def test_headers_reject_injection(self):
        with self.assertRaises(ValueError): worker.recipients('a@example.org\r\nBcc: victim@example.org')
        with self.assertRaises(ValueError): worker.clean_header('subject\nBcc: x')
        self.assertEqual(worker.recipients('Alice <alice@example.org>, bob@example.org'), ['alice@example.org', 'bob@example.org'])

    def test_only_verified_tls(self):
        with patch.object(worker.imaplib, 'IMAP4_SSL') as ctor:
            worker.incoming(ACCOUNT, 'TEST-ONLY')
            context = ctor.call_args.kwargs['ssl_context']
            self.assertTrue(context.check_hostname); self.assertEqual(context.verify_mode, ssl.CERT_REQUIRED)
        with patch.object(worker.smtplib, 'SMTP_SSL') as ctor:
            worker.outgoing(ACCOUNT, 'TEST-ONLY')
            self.assertTrue(ctor.call_args.kwargs['context'].check_hostname)
        account = dict(ACCOUNT, smtpPort=587, smtpSecurity='starttls')
        with patch.object(worker.smtplib, 'SMTP') as ctor:
            worker.outgoing(account, 'TEST-ONLY')
            names = [c[0] for c in ctor.return_value.method_calls]
            self.assertLess(names.index('starttls'), names.index('login'))

    def test_list_uses_uid_and_peek(self):
        connection = self.imap()
        def uid(command, *args):
            if command == 'search': return 'OK', [b'7 8']
            self.assertIn('BODY.PEEK', args[1])
            return 'OK', [(b'1 (UID 8 FLAGS ())', RAW), b')', (b'2 (UID 7 FLAGS (\\Seen))', RAW), b')']
        connection.uid.side_effect = uid
        with patch.object(worker, 'incoming', return_value=connection):
            result = worker.execute(self.request('list'))
        self.assertEqual(result['validity'], '42')
        self.assertEqual([m['uid'] for m in result['messages']], ['8', '7'])
        connection.select.assert_called_once_with('"INBOX"', readonly=True)
        connection.logout.assert_called_once()

    def test_stale_and_oversized_body(self):
        connection = self.imap()
        with patch.object(worker, 'incoming', return_value=connection):
            with self.assertRaisesRegex(ValueError, 'geändert'): worker.execute(self.request('body', uid='7', validity='41'))
        connection.uid.return_value = ('OK', [b'1 (RFC822.SIZE 99999999)'])
        with patch.object(worker, 'incoming', return_value=connection):
            with self.assertRaisesRegex(ValueError, '64 MB'): worker.execute(self.request('body', uid='7', validity='42'))

    def test_body_fetch(self):
        connection = self.imap()
        connection.uid.side_effect = [('OK', [b'1 (RFC822.SIZE 800)']), ('OK', [(b'1 BODY', RAW)]), ('OK', [])]
        with patch.object(worker, 'incoming', return_value=connection):
            result = worker.execute(self.request('body', uid='7', validity='42'))
        self.assertIn('Hallo Laura', result['text'])
        self.assertEqual(connection.uid.call_args_list[1].args[-1], '(BODY.PEEK[])')
        self.assertEqual(connection.uid.call_args.args, ('store', '7', '+FLAGS.SILENT', '(\\Seen)'))
        self.assertTrue(result['seen'])

    def test_empty_plain_falls_back_to_html(self):
        message = EmailMessage(); message.set_content('')
        message.add_alternative('<h1>Hello HTML</h1>', subtype='html')
        result = worker.body(message.as_bytes())
        self.assertIn('<h1>Hello HTML</h1>', result['html'])
        self.assertIn('Hello HTML', result['text'])

    def test_unknown_charset_and_text_attachment(self):
        self.assertIn('café', worker.body(b'Content-Type: text/plain; charset=unknown\r\n\r\ncaf\xe9')['text'])
        message = EmailMessage(); message.set_content('Main body')
        message.add_attachment('Private attachment', filename='note.txt')
        result = worker.body(message.as_bytes())
        self.assertNotIn('Private attachment', result['text'])
        self.assertEqual(result['attachments'][0]['name'], 'note.txt')

    def test_inline_image_and_attachment_download(self):
        message = EmailMessage(); message.set_content('<img src="cid:logo">', subtype='html')
        message.add_related(b'PNG-fixture', maintype='image', subtype='png', cid='<logo>')
        result = worker.body(message.as_bytes())
        self.assertIn('data:image/png;base64,', result['html'])
        self.assertEqual(base64.b64decode(worker.attachment_data(message.as_bytes(), result['attachments'][0]['id'])['data']), b'PNG-fixture')
        with self.assertRaises(ValueError): worker.attachment_data(message.as_bytes(), '1.0')

    def test_inline_image_content_location_and_encoded_cid(self):
        message = EmailMessage(); message.set_content('<img src="cid:Logo%40Example"><img src="assets/banner.png">', subtype='html')
        message.add_related(b'CID-image', maintype='image', subtype='png', cid='<logo@example>')
        message.add_related(b'Location-image', maintype='image', subtype='jpeg', headers=['Content-Location: assets/banner.png'])
        markup = worker.body(message.as_bytes())['html']
        self.assertNotIn('cid:', markup)
        self.assertNotIn('assets/banner.png', markup)
        self.assertEqual(markup.count('data:image/'), 2)

    def test_all_folders_and_quoted_names(self):
        connection = self.imap()
        connection.list.return_value = ('OK', [b'(\\HasNoChildren) "/" "INBOX"', b'(\\Noselect) "/" "Archive"', b'() "/" "Archive/Old Mail"', b'() "/" "Entw&APw-rfe"'])
        connection.status.return_value = ('OK', [b'"folder" (UNSEEN 3)'])
        with patch.object(worker, 'incoming', return_value=connection): result = worker.execute(self.request('folders'))
        self.assertEqual(len(result['folders']), 4)
        self.assertFalse(result['folders'][1]['selectable'])
        self.assertEqual(result['folders'][3]['title'], 'Entwürfe')
        self.assertEqual(result['folders'][2]['unread'], 3)
        connection.status.assert_any_call('"Archive/Old Mail"', '(UNSEEN)')
        connection.select.assert_not_called()
        with self.assertRaises(ValueError): worker.mailbox('INBOX\r\nLOGOUT')

    def test_attachment_does_not_mark_seen(self):
        connection = self.imap()
        connection.uid.side_effect = [('OK', [b'1 (RFC822.SIZE 800)']), ('OK', [(b'1 BODY', RAW)])]
        with patch.object(worker, 'incoming', return_value=connection):
            result = worker.execute(self.request('attachment', uid='7', validity='42', folder='Archive/Old Mail', attachment='1.2'))
        self.assertEqual(base64.b64decode(result['data']), b'PDF')
        connection.select.assert_called_once_with('"Archive/Old Mail"', readonly=True)
        self.assertTrue(all(call.args[0] != 'store' for call in connection.uid.call_args_list))

    def test_batch_move_uses_uid_move(self):
        connection = self.imap(); connection.uid.return_value = ('OK', [])
        with patch.object(worker, 'incoming', return_value=connection):
            result = worker.execute(self.request('move', folder='INBOX', validity='42', uids=['7', '8'], target='Archive/Trips'))
        self.assertEqual(result['changed'], 2)
        connection.select.assert_called_once_with('"INBOX"', readonly=False)
        connection.uid.assert_called_once_with('MOVE', '7,8', '"Archive/Trips"')

    def test_delete_moves_to_special_use_trash(self):
        connection = self.imap()
        connection.list.return_value = ('OK', [b'(\\Trash) "/" "Deleted Items"'])
        connection.uid.return_value = ('OK', [])
        with patch.object(worker, 'incoming', return_value=connection):
            result = worker.execute(self.request('delete', folder='INBOX', validity='42', uids=['7', '8']))
        self.assertEqual(result['target'], 'Deleted Items')
        connection.uid.assert_called_once_with('MOVE', '7,8', '"Deleted Items"')

    def test_delete_inside_trash_is_permanent(self):
        connection = self.imap()
        connection.list.return_value = ('OK', [b'(\\Trash) "/" "Trash"'])
        connection.uid.side_effect = [('OK', []), ('OK', [])]
        with patch.object(worker, 'incoming', return_value=connection):
            result = worker.execute(self.request('delete', folder='Trash', validity='42', uids=['9']))
        self.assertEqual(result['target'], '')
        self.assertEqual(connection.uid.call_args_list[0].args, ('STORE', '9', '+FLAGS.SILENT', '(\\Deleted)'))
        self.assertEqual(connection.uid.call_args_list[1].args, ('EXPUNGE', '9'))

    def test_delete_without_trash_never_deletes_permanently(self):
        connection = self.imap(); connection.list.return_value = ('OK', [b'() "/" "INBOX"'])
        with patch.object(worker, 'incoming', return_value=connection):
            with self.assertRaisesRegex(ValueError, 'keinen Papierkorb'):
                worker.execute(self.request('delete', folder='INBOX', validity='42', uids=['7']))
        connection.uid.assert_not_called()

    def test_send_builds_message_without_network(self):
        smtp = MagicMock(); smtp.send_message.return_value = {}
        imap = self.imap(); imap.list.return_value = ('OK', [b'(\\Sent) "/" "Sent Items"']); imap.append.return_value = ('OK', [b'1'])
        with patch.object(worker, 'outgoing', return_value=smtp), patch.object(worker, 'incoming', return_value=imap):
            result = worker.execute(self.request('send', to='Alice <alice@example.org>', subject='Grüße', text='Hallo ☀', replyID='<reply@example.org>'))
        message = smtp.send_message.call_args.args[0]
        self.assertEqual(message['Subject'], 'Grüße')
        self.assertIn('Hallo ☀', message.get_content())
        self.assertEqual(message['In-Reply-To'], '<reply@example.org>')
        self.assertTrue(result['sent']); self.assertTrue(result['saved']); smtp.close.assert_called_once()
        self.assertEqual(imap.append.call_args.args[0], '"Sent Items"')
        self.assertIn(b'Hallo', imap.append.call_args.args[3])

    def test_send_success_stays_success_when_sent_copy_fails(self):
        smtp = MagicMock(); smtp.send_message.return_value = {}
        imap = self.imap(); imap.list.return_value = ('OK', [b'() "/" "INBOX"'])
        with patch.object(worker, 'outgoing', return_value=smtp), patch.object(worker, 'incoming', return_value=imap):
            result = worker.execute(self.request('send', to='alice@example.org', subject='Test', text='Sent once'))
        self.assertTrue(result['sent']); self.assertFalse(result['saved']); self.assertIn('kein Gesendet-Ordner', result['warning'])
        smtp.send_message.assert_called_once()

    def test_gmail_does_not_append_duplicate_sent_copy(self):
        smtp = MagicMock(); smtp.send_message.return_value = {}
        account = dict(ACCOUNT, smtpHost='smtp.gmail.com')
        with patch.object(worker, 'outgoing', return_value=smtp), patch.object(worker, 'incoming') as incoming:
            result = worker.execute(dict(self.request('send', to='alice@example.org', subject='Test', text='Hi'), account=account))
        self.assertTrue(result['savedByProvider']); incoming.assert_not_called()

    def test_connection_check_does_not_send(self):
        imap = self.imap(); smtp = MagicMock()
        with patch.object(worker, 'incoming', return_value=imap), patch.object(worker, 'outgoing', return_value=smtp):
            self.assertTrue(worker.execute(self.request('test'))['connected'])
        smtp.send_message.assert_not_called()


if __name__ == '__main__': unittest.main()
