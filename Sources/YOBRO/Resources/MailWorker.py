"""YOBRO's local IMAP/SMTP transport. Secrets arrive only through stdin, never argv.

Python standard library only. TLS certificate verification is always enabled.
"""
import base64
import email
from email import policy
from email.message import EmailMessage
from email.utils import getaddresses, parsedate_to_datetime
import imaplib
import json
import os
import re
import smtplib
import ssl
import sys
import time
from urllib.parse import unquote
from html.parser import HTMLParser

MAX_BODY = 64 * 1024 * 1024
TRASH_NAMES = {'trash', 'deleted', 'deleted items', 'papierkorb', 'gelöscht', 'gelöschte elemente'}
SENT_NAMES = {'sent', 'sent mail', 'sent items', 'gesendet', 'gesendete elemente'}


class AuthError(Exception):
    """The server rejected the credentials, as opposed to any other command."""


class TextHTML(HTMLParser):
    def __init__(self):
        super().__init__(); self.parts = []; self.hidden = 0
    def handle_starttag(self, tag, attrs):
        if tag in ('script', 'style', 'head'): self.hidden += 1
        if tag in ('p', 'br', 'div', 'li', 'tr') and not self.hidden: self.parts.append('\n')
    def handle_endtag(self, tag):
        if tag in ('script', 'style', 'head'): self.hidden = max(0, self.hidden - 1)
    def handle_data(self, data):
        if not self.hidden: self.parts.append(data)


def localized(german, english):
    return german if os.environ.get('YOBRO_LANGUAGE', 'de') == 'de' else english


def clean_header(value):
    value = str(value)
    if '\r' in value or '\n' in value: raise ValueError(localized('Header darf keinen Zeilenumbruch enthalten.', 'Headers must not contain line breaks.'))
    return value.strip()


def recipients(value):
    value = clean_header(value)
    addresses = getaddresses([value])
    if not addresses or any(not re.fullmatch(r'[^\s<>@,;]+@[^\s<>@,;]+\.[^\s<>@,;]+', addr) for _, addr in addresses):
        raise ValueError(localized('Bitte gültige E-Mail-Adressen eingeben (mit Komma trennen).', 'Enter valid email addresses separated by commas.'))
    return [address for _, address in addresses]


def config(request):
    account = request['account']
    for key in ('imapHost', 'smtpHost'):
        host = account[key].strip()
        if not host or any(c.isspace() for c in host) or '/' in host: raise ValueError(localized('Ungültiger Servername.', 'Invalid server name.'))
    for key in ('imapPort', 'smtpPort'):
        if not 1 <= int(account[key]) <= 65535: raise ValueError(localized('Ungültiger Port.', 'Invalid port.'))
    if len(recipients(account['address'])) != 1: raise ValueError(localized('Genau eine Konto-Adresse erforderlich.', 'Exactly one account address is required.'))
    if account.get('smtpSecurity') not in ('tls', 'starttls'): raise ValueError(localized('SMTP benötigt TLS oder STARTTLS.', 'SMTP requires TLS or STARTTLS.'))
    if not request.get('password'): raise ValueError(localized('Passwort fehlt.', 'Password is missing.'))
    return account


def plain_credentials(username, password):
    """SASL PLAIN in UTF-8. LOGIN in imaplib and smtplib only accepts ASCII passwords."""
    return b'\0' + username.encode('utf-8') + b'\0' + password.encode('utf-8')


def incoming(account, password):
    connection = imaplib.IMAP4_SSL(account['imapHost'], int(account['imapPort']), ssl_context=ssl.create_default_context(), timeout=20)
    try:
        if password.isascii(): connection.login(account['username'], password)
        else: connection.authenticate('PLAIN', lambda _: plain_credentials(account['username'], password))
        return connection
    except imaplib.IMAP4.error as error:
        try: connection.logout()
        except Exception: pass
        raise AuthError() from error
    except Exception:
        try: connection.logout()
        except Exception: pass
        raise


def outgoing(account, password):
    context = ssl.create_default_context()
    if account['smtpSecurity'] == 'tls':
        connection = smtplib.SMTP_SSL(account['smtpHost'], int(account['smtpPort']), timeout=20, context=context)
    else:
        connection = smtplib.SMTP(account['smtpHost'], int(account['smtpPort']), timeout=20)
    try:
        connection.ehlo()
        if account['smtpSecurity'] == 'starttls':
            connection.starttls(context=context); connection.ehlo()
        username = account.get('smtpUsername') or account['username']
        try:
            if password.isascii():
                connection.login(username, password)
            else:
                code, _ = connection.docmd('AUTH', 'PLAIN ' + base64.b64encode(plain_credentials(username, password)).decode('ascii'))
                if code != 235: raise smtplib.SMTPAuthenticationError(code, b'')
        except smtplib.SMTPAuthenticationError as error:
            raise AuthError() from error
        return connection
    except Exception:
        connection.close(); raise


def capabilities(connection):
    """Capabilities after login; servers often announce more than before it."""
    try:
        status, data = connection.capability()
        if status == 'OK' and data and isinstance(data[0], bytes): return set(data[0].decode('ascii', 'replace').upper().split())
    except Exception: pass
    return {str(value).upper() for value in getattr(connection, 'capabilities', ())}


def mailbox(value):
    if not isinstance(value, str) or any(c in value for c in ('\r', '\n', '\0')): raise ValueError(localized('Ungültiger Ordner.', 'Invalid folder.'))
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'


def folder_title(value):
    def decode(match):
        value = match.group(1)
        if not value: return '&'
        try: return base64.b64decode(value.replace(',', '/') + '=' * (-len(value) % 4)).decode('utf-16-be')
        except Exception: return match.group(0)
    return re.sub(r'&([^-]*)-', decode, value)


def listed(connection):
    """Rows of LIST as (flags, delimiter, path)."""
    status, rows = connection.list()
    if status != 'OK': return None
    result = []
    for row in rows or []:
        literal = row[1] if isinstance(row, tuple) else None
        raw = row[0] if isinstance(row, tuple) else row
        if not isinstance(raw, bytes): continue
        match = re.match(rb'\((.*?)\)\s+(NIL|"(?:[^"\\]|\\.)*")\s+(.*)', raw)
        if not match: continue
        flags, delimiter, name = match.groups()
        if literal is not None: name = literal
        elif name.startswith(b'"') and name.endswith(b'"'): name = re.sub(rb'\\(.)', rb'\1', name[1:-1])
        result.append((flags.lower(), delimiter, name.decode('utf-8', errors='replace')))
    return result


def folders(connection):
    rows = listed(connection)
    if rows is None: raise ValueError(localized('Ordner konnten nicht geladen werden.', 'Could not load folders.'))
    result = []
    for flags, delimiter, path in rows:
        separator = delimiter.strip(b'"').decode('utf-8') if delimiter != b'NIL' else ''
        selectable = b'\\noselect' not in flags
        role = next((role for marker, role in ((b'\\sent', 'sent'), (b'\\trash', 'trash'), (b'\\drafts', 'drafts'), (b'\\junk', 'junk'), (b'\\archive', 'archive')) if marker in flags), None)
        unread = 0
        if selectable:
            try:
                ok, stats = connection.status(mailbox(path), '(UNSEEN)')
                found = re.search(rb'UNSEEN\s+(\d+)', b' '.join(x for x in stats or [] if isinstance(x, bytes)))
                if ok == 'OK' and found: unread = int(found[1])
            except imaplib.IMAP4.error: pass
        result.append(dict(path=path, title=folder_title(path), delimiter=separator, selectable=selectable, unread=unread, role=role))
    if not any(row['path'].upper() == 'INBOX' for row in result): result.insert(0, dict(path='INBOX', title='Posteingang', delimiter='/', selectable=True, unread=0, role='inbox'))
    return dict(folders=result)


def special_folder(connection, flag, names):
    """The folder with an IMAP special-use flag, or one with a well-known name."""
    rows = listed(connection)
    if rows is None: return None
    fallback = None
    for flags, _, path in rows:
        if flag in flags: return path
        leaf = re.split(r'[/\\.]', folder_title(path))[-1].strip().lower()
        if fallback is None and leaf in names: fallback = path
    return fallback


def sent_folder(connection): return special_folder(connection, b'\\sent', SENT_NAMES)
def trash_folder(connection): return special_folder(connection, b'\\trash', TRASH_NAMES)


def expunge(connection, sequence, caps):
    # Without UIDPLUS, a plain EXPUNGE would also remove messages that other
    # clients flagged as deleted. Those stay hidden here and on the server.
    if 'UIDPLUS' in caps: connection.uid('EXPUNGE', sequence)


def change_messages(connection, source, uids, target=None):
    sequence = ','.join(uids)
    caps = capabilities(connection)
    if target is not None:
        if 'MOVE' in caps:
            status, _ = connection.uid('MOVE', sequence, mailbox(target))
            if status == 'OK': return
        status, _ = connection.uid('COPY', sequence, mailbox(target))
        if status != 'OK': raise ValueError(localized('Nachrichten konnten nicht verschoben werden.', 'Messages could not be moved.'))
        status, _ = connection.uid('STORE', sequence, '+FLAGS.SILENT', '(\\Deleted)')
        if status != 'OK': raise ValueError(localized('Nachrichten wurden kopiert, konnten aber nicht aus dem ursprünglichen Ordner entfernt werden.', 'Messages were copied but could not be removed from the original folder.'))
        expunge(connection, sequence, caps)
        return
    status, _ = connection.uid('STORE', sequence, '+FLAGS.SILENT', '(\\Deleted)')
    if status != 'OK': raise ValueError(localized('Nachrichten konnten nicht gelöscht werden.', 'Messages could not be deleted.'))
    if 'UIDPLUS' in caps: connection.uid('EXPUNGE', sequence)
    else: connection.expunge()


def provider_saves_sent(account):
    host = account.get('smtpHost', '').strip().lower()
    return host == 'smtp.gmail.com' or host.endswith('.smtp.gmail.com') or host == 'smtp.googlemail.com'


def save_sent_copy(account, password, raw):
    connection = incoming(account, password)
    try:
        target = sent_folder(connection)
        if not target:
            return False, localized('Die Nachricht wurde versendet, aber kein Gesendet-Ordner wurde gefunden.', 'The message was sent, but no Sent folder was found.')
        status, _ = connection.append(mailbox(target), '(\\Seen)', imaplib.Time2Internaldate(time.time()), raw)
        if status != 'OK':
            return False, localized('Die Nachricht wurde versendet, aber die Kopie konnte nicht in Gesendet gespeichert werden.', 'The message was sent, but its copy could not be saved to Sent.')
        return True, None
    finally:
        try: connection.logout()
        except Exception: pass


def selected(connection, folder='INBOX', readonly=True):
    status, _ = connection.select(mailbox(folder), readonly=readonly)
    if status != 'OK': raise ValueError(localized('Mail-Ordner konnte nicht geöffnet werden.', 'Could not open mail folder.'))
    _, validity = connection.response('UIDVALIDITY')
    if not validity or not validity[0]: raise ValueError(localized('Der Server liefert keine stabile Postfach-ID.', 'The server does not provide a stable mailbox ID.'))
    return validity[0].decode('ascii')


def decode_message(raw):
    return email.message_from_bytes(raw, policy=policy.default)


def header(message, name, fallback=''):
    """A header as text. A malformed header must not hide the whole message."""
    try:
        value = message.get(name)
        return fallback if value is None else str(value)
    except Exception:
        try: return str(message.get_all(name, [fallback])[0])
        except Exception: return fallback


def summary(message, uid, flags):
    timestamp = 0
    try: timestamp = parsedate_to_datetime(header(message, 'Date')).timestamp()
    except (ValueError, TypeError, OverflowError, IndexError): pass
    if not timestamp:
        # No usable Date header: fall back to when the server received it.
        received = imaplib.Internaldate2tuple(flags)
        if received: timestamp = time.mktime(received)
    sender = header(message, 'From', localized('Unbekannt', 'Unknown'))
    return dict(uid=uid, subject=header(message, 'Subject'), sender=sender, to=header(message, 'To'),
                replyTo=header(message, 'Reply-To') or sender, date=timestamp,
                unread=b'\\Seen' not in flags, messageID=header(message, 'Message-ID'),
                references=header(message, 'References'))


def part_text(part):
    payload = part.get_payload(decode=True)
    if payload is None:
        value = part.get_payload()
        return value if isinstance(value, str) else ''
    encodings = [part.get_content_charset(), 'utf-8', 'windows-1252', 'latin-1']
    for encoding in encodings:
        if not encoding: continue
        try: return payload.decode(encoding)
        except (LookupError, UnicodeError): continue
    return payload.decode('utf-8', errors='replace')


def body(raw):
    message = decode_message(raw)
    plain = []; html = []; attachments = []; inline = {}; inline_locations = {}
    def visit(part, key='1'):
        # Attached emails and multipart attachments are files, not the parent message body.
        attachment = part.get_content_disposition() == 'attachment' or bool(part.get_filename())
        if part.get_content_type() == 'message/rfc822': attachment = True
        if attachment or not part.is_multipart() and part.get_content_maintype() != 'text':
            payload = part.as_bytes() if part.is_multipart() else part.get_payload(decode=True) or b''
            name = str(part.get_filename() or ('Nachricht.eml' if part.get_content_type() == 'message/rfc822' else 'Anhang'))
            attachments.append(dict(id=key, name=name, size=len(payload), mime=part.get_content_type()))
            cid = str(part.get('Content-ID', '')).strip('<>')
            if part.get_content_type() in ('image/png', 'image/jpeg', 'image/gif', 'image/webp') and len(payload) <= 8 * 1024 * 1024:
                data_url = 'data:' + part.get_content_type() + ';base64,' + base64.b64encode(payload).decode('ascii')
                if cid: inline[unquote(cid).lower()] = data_url
                location = str(part.get('Content-Location', '')).strip()
                if location: inline_locations[location] = data_url
            return
        if part.is_multipart():
            for index, child in enumerate(part.iter_parts(), 1): visit(child, key + '.' + str(index))
        elif part.get_content_type() == 'text/html':
            value = part_text(part)
            if value.strip(): html.append(value)
        elif part.get_content_type() == 'text/plain':
            value = part_text(part)
            if value.strip(): plain.append(value)
    visit(message)
    markup = '\n'.join(html)
    def cid(match): return inline.get(unquote(match.group(1)).lower(), '')
    # Quotes and a closing parenthesis end the reference: src='cid:…' and url(cid:…) occur too.
    markup = re.sub(r'cid:([^"\'\s<>)]+)', cid, markup, flags=re.I)
    for location, data_url in inline_locations.items():
        # Only image references, never the same text in the message itself.
        markup = re.sub(r'((?:src|background)\s*=\s*["\']?)' + re.escape(location) + r'(?=["\'\s>])', lambda m: m.group(1) + data_url, markup, flags=re.I)
    text = '\n\n'.join(plain)
    if not text and markup:
        parser = TextHTML(); parser.feed(markup); text = ''.join(parser.parts)
    return dict(text=text[:2000000], html=markup[:16000000], attachments=attachments)


def attachment_data(raw, identifier):
    part = decode_message(raw)
    pieces = str(identifier).split('.')
    if not pieces or pieces[0] != '1': raise ValueError(localized('Ungültiger Anhang.', 'Invalid attachment.'))
    for number in pieces[1:]:
        if not number.isdigit() or not part.is_multipart(): raise ValueError(localized('Anhang nicht gefunden.', 'Attachment not found.'))
        children = list(part.iter_parts()); index = int(number) - 1
        if index < 0 or index >= len(children): raise ValueError(localized('Anhang nicht gefunden.', 'Attachment not found.'))
        part = children[index]
    payload = part.as_bytes() if part.is_multipart() else part.get_payload(decode=True) or b''
    return dict(data=base64.b64encode(payload).decode('ascii'))


def fetch_message(connection, uid):
    status, sizes = connection.uid('fetch', uid, '(RFC822.SIZE)')
    size_line = b' '.join(x for x in sizes or [] if isinstance(x, bytes))
    match = re.search(rb'RFC822.SIZE\s+(\d+)', size_line)
    if status != 'OK' or not match: raise ValueError(localized('Nachricht nicht mehr vorhanden.', 'Message is no longer available.'))
    if int(match[1]) > MAX_BODY: raise ValueError(localized('Diese Nachricht ist größer als 64 MB. Bitte beim Anbieter öffnen.', 'This message exceeds 64 MB. Open it with your provider.'))
    status, rows = connection.uid('fetch', uid, '(BODY.PEEK[])')
    if status != 'OK': raise ValueError(localized('Nachricht konnte nicht geladen werden.', 'Could not load message.'))
    for row in rows or []:
        if isinstance(row, tuple): return row[1]
    raise ValueError(localized('Nachricht nicht mehr vorhanden.', 'Message is no longer available.'))


def send(request, account, password):
    # The UI invokes this only after the user's Send button. No automatic retries.
    to = recipients(request['to'])
    cc = recipients(request['cc']) if str(request.get('cc') or '').strip() else []
    sender = recipients(account['address'])
    if len(sender) != 1: raise ValueError(localized('Ein Absender erforderlich.', 'A sender is required.'))
    message = EmailMessage()
    message['From'] = account['address']; message['To'] = clean_header(request['to'])
    if cc: message['Cc'] = clean_header(request['cc'])
    message['Subject'] = clean_header(request['subject'])
    message['Date'] = email.utils.formatdate(localtime=True)
    message['Message-ID'] = email.utils.make_msgid()
    if request.get('replyID'):
        message['In-Reply-To'] = clean_header(request['replyID'])
        # References keeps the whole thread together in other mail programs.
        message['References'] = clean_header(' '.join(filter(None, [request.get('references', ''), request['replyID']])))
    message.set_content(request['text'])
    if request.get('html'):
        message.add_alternative(request['html'], subtype='html')
    connection = outgoing(account, password)
    try:
        refused = connection.send_message(message, from_addr=sender[0], to_addrs=to + cc)
    finally:
        # A failed QUIT after successful DATA must not cause a duplicate resend.
        connection.close()
    if provider_saves_sent(account):
        return dict(sent=True, saved=True, savedByProvider=True, refused=list(refused), messageID=str(message['Message-ID']))
    try:
        saved, warning = save_sent_copy(account, password, message.as_bytes(policy=policy.SMTP))
    except Exception:
        saved, warning = False, localized('Die Nachricht wurde versendet, aber die Kopie konnte nicht in Gesendet gespeichert werden.', 'The message was sent, but its copy could not be saved to Sent.')
    return dict(sent=True, saved=saved, warning=warning, refused=list(refused), messageID=str(message['Message-ID']))


def execute(request):
    account = config(request); password = request['password']; action = request['action']
    if action == 'send': return send(request, account, password)
    connection = incoming(account, password)
    try:
        if action == 'test':
            selected(connection)
            smtp = outgoing(account, password); smtp.close()
            return dict(connected=True)
        if action == 'folders': return folders(connection)
        folder = request.get('folder', 'INBOX')
        validity = selected(connection, folder, readonly=action not in ('body', 'seen', 'unseen', 'move', 'delete'))
        if action == 'list':
            status, data = connection.uid('search', None, 'UNDELETED')
            if status != 'OK': raise ValueError(localized('Nachrichten konnten nicht gesucht werden.', 'Could not search messages.'))
            all_uids = (data[0] or b'').split()
            limit = min(5000, max(1, int(request.get('limit', 100))))
            uids = all_uids[-limit:]
            messages = []
            if uids:
                status, rows = connection.uid('fetch', b','.join(uids), '(UID FLAGS INTERNALDATE BODY.PEEK[HEADER.FIELDS (SUBJECT FROM TO REPLY-TO DATE MESSAGE-ID REFERENCES)])')
                if status != 'OK': raise ValueError(localized('Nachrichtenköpfe konnten nicht geladen werden.', 'Could not load message headers.'))
                for row in rows or []:
                    if not isinstance(row, tuple): continue
                    uid = re.search(rb'UID\s+(\d+)', row[0])
                    if not uid: continue
                    try: messages.append(summary(decode_message(row[1]), uid[1].decode(), row[0]))
                    except Exception:
                        # One damaged message must not hide the rest of the folder.
                        messages.append(dict(uid=uid[1].decode(), subject=localized('(Nicht lesbarer Betreff)', '(Unreadable subject)'), sender=localized('Unbekannt', 'Unknown'),
                                             to='', replyTo='', date=0, unread=b'\\Seen' not in row[0], messageID='', references=''))
            return dict(validity=validity, messages=messages, total=len(all_uids))
        if action in ('move', 'delete'):
            uids = [str(uid) for uid in request.get('uids', [])]
            if not uids or len(uids) > 1000 or any(not uid.isdigit() for uid in uids):
                raise ValueError(localized('Ungültige Nachrichtenauswahl.', 'Invalid message selection.'))
            if request.get('validity') != validity:
                raise ValueError(localized('Postfach hat sich geändert. Bitte aktualisieren.', 'Mailbox changed. Please refresh.'))
            target = request.get('target') if action == 'move' else trash_folder(connection)
            if action == 'delete' and target and target.casefold() == folder.casefold(): target = None
            if action == 'delete' and target is None:
                source_leaf = re.split(r'[/\\.]', folder_title(folder))[-1].strip().lower()
                if source_leaf not in TRASH_NAMES:
                    raise ValueError(localized('Der Anbieter meldet keinen Papierkorb. Es wurde nichts gelöscht.', 'The provider does not report a Trash folder. Nothing was deleted.'))
            if action == 'move':
                if not isinstance(target, str) or target == folder: raise ValueError(localized('Zielordner wählen.', 'Choose a destination folder.'))
            change_messages(connection, folder, uids, target)
            return dict(changed=len(uids), target=target or '')
        if action in ('body', 'attachment', 'seen', 'unseen'):
            uid = str(request['uid'])
            if not uid.isdigit() or request.get('validity') != validity: raise ValueError(localized('Postfach hat sich geändert. Bitte aktualisieren.', 'Mailbox changed. Please refresh.'))
            if action == 'attachment': return attachment_data(fetch_message(connection, uid), request['attachment'])
            if action == 'unseen':
                status, _ = connection.uid('store', uid, '-FLAGS.SILENT', '(\\Seen)')
                if status != 'OK': raise ValueError(localized('Die Nachricht konnte nicht als ungelesen markiert werden.', 'Could not mark the message as unread.'))
                return dict(unseen=True)
            result = body(fetch_message(connection, uid)) if action == 'body' else {}
            status, _ = connection.uid('store', uid, '+FLAGS.SILENT', '(\\Seen)')
            result['seen'] = status == 'OK'
            if status != 'OK': result['warning'] = localized('Die Nachricht konnte beim Anbieter nicht als gelesen markiert werden.', 'The provider could not mark the message as read.')
            return result
        raise ValueError(localized('Unbekannte Mail-Aktion.', 'Unknown mail action.'))
    finally:
        try: connection.logout()
        except Exception: pass


def main():
    request = json.load(sys.stdin)
    try:
        print(json.dumps(dict(ok=True, result=execute(request)), ensure_ascii=False))
    except AuthError:
        print(json.dumps(dict(ok=False, error=localized('Anmeldung fehlgeschlagen. Serverdaten und App-Passwort prüfen. Der Anbieter benötigt möglicherweise OAuth.', 'Sign-in failed. Check server settings and app password. The provider may require OAuth.'))))
    except imaplib.IMAP4.error:
        # Signed in, but the server refused a command: not a password problem.
        print(json.dumps(dict(ok=False, error=localized('Der Mailserver hat die Aktion abgelehnt. Bitte aktualisieren und erneut versuchen.', 'The mail server refused the action. Refresh and try again.'))))
    except Exception as error:
        # Do not return server transcripts: they may echo authentication material.
        detail = str(error) if isinstance(error, ValueError) else (
            localized('Versandstatus unklar. Bitte beim Anbieter prüfen, bevor du erneut sendest.', 'Delivery status is unclear. Check with your provider before sending again.') if request.get('action') == 'send'
            else type(error).__name__ + localized(': Verbindung oder Übertragung fehlgeschlagen.', ': Connection or transfer failed.'))
        print(json.dumps(dict(ok=False, error=detail)))


if __name__ == '__main__': main()
