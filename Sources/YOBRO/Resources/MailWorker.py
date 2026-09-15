"""YOBRO's local IMAP/SMTP transport. Secrets arrive only through stdin, never argv.

Python standard library only. TLS certificate verification is always enabled.
"""
import email
import base64
from email import policy
from email.message import EmailMessage
from email.utils import getaddresses, parsedate_to_datetime
import imaplib
import json
import re
import smtplib
import ssl
import sys
import time
from urllib.parse import unquote
from html.parser import HTMLParser

MAX_BODY = 64 * 1024 * 1024


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
    return german if __import__('os').environ.get('YOBRO_LANGUAGE', 'de') == 'de' else english


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


def incoming(account, password):
    connection = imaplib.IMAP4_SSL(account['imapHost'], int(account['imapPort']), ssl_context=ssl.create_default_context(), timeout=20)
    try:
        connection.login(account['username'], password)
        return connection
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
        connection.login(account.get('smtpUsername') or account['username'], password)
        return connection
    except Exception:
        connection.close(); raise


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


def folders(connection):
    status, rows = connection.list()
    if status != 'OK': raise ValueError(localized('Ordner konnten nicht geladen werden.', 'Could not load folders.'))
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
        path = name.decode('utf-8', errors='replace')
        separator = delimiter.strip(b'"').decode('utf-8') if delimiter != b'NIL' else ''
        selectable = b'\\noselect' not in flags.lower()
        lowered_flags = flags.lower()
        role = next((role for marker, role in ((b'\\sent', 'sent'), (b'\\trash', 'trash'), (b'\\drafts', 'drafts'), (b'\\junk', 'junk'), (b'\\archive', 'archive')) if marker in lowered_flags), None)
        unread = 0
        if selectable:
            try:
                ok, stats = connection.status(mailbox(path), '(UNSEEN)')
                found = re.search(rb'UNSEEN\s+(\d+)', b' '.join(x for x in stats or [] if isinstance(x, bytes)))
                if ok == 'OK' and found: unread = int(found[1])
            except imaplib.IMAP4.error: pass
        result.append(dict(path=path, title=folder_title(path), delimiter=separator, selectable=selectable, unread=unread, role=role))
    if not any(row['path'].upper() == 'INBOX' for row in result): result.insert(0, dict(path='INBOX',title='Posteingang',delimiter='/',selectable=True,unread=0,role='inbox'))
    return dict(folders=result)


def sent_folder(connection):
    """Return the provider's Sent mailbox, preferring the IMAP SPECIAL-USE flag."""
    status, rows = connection.list()
    if status != 'OK': return None
    fallback = None
    common = {'sent', 'sent mail', 'sent items', 'gesendet', 'gesendete elemente'}
    for row in rows or []:
        literal = row[1] if isinstance(row, tuple) else None
        raw = row[0] if isinstance(row, tuple) else row
        if not isinstance(raw, bytes): continue
        match = re.match(rb'\((.*?)\)\s+(NIL|"(?:[^"\\]|\\.)*")\s+(.*)', raw)
        if not match: continue
        flags, _, name = match.groups()
        if literal is not None: name = literal
        elif name.startswith(b'"') and name.endswith(b'"'): name = re.sub(rb'\\(.)', rb'\1', name[1:-1])
        path = name.decode('utf-8', errors='replace')
        if b'\\sent' in flags.lower(): return path
        leaf = re.split(r'[/\\.]', folder_title(path))[-1].strip().lower()
        if fallback is None and leaf in common: fallback = path
    return fallback


def trash_folder(connection):
    status, rows = connection.list()
    if status != 'OK': return None
    fallback = None
    common = {'trash', 'deleted', 'deleted items', 'papierkorb', 'gelöscht', 'gelöschte elemente'}
    for row in rows or []:
        literal = row[1] if isinstance(row, tuple) else None
        raw = row[0] if isinstance(row, tuple) else row
        if not isinstance(raw, bytes): continue
        match = re.match(rb'\((.*?)\)\s+(NIL|"(?:[^"\\]|\\.)*")\s+(.*)', raw)
        if not match: continue
        flags, _, name = match.groups()
        if literal is not None: name = literal
        elif name.startswith(b'"') and name.endswith(b'"'): name = re.sub(rb'\\(.)', rb'\1', name[1:-1])
        path = name.decode('utf-8', errors='replace')
        if b'\\trash' in flags.lower(): return path
        leaf = re.split(r'[/\\.]', folder_title(path))[-1].strip().lower()
        if fallback is None and leaf in common: fallback = path
    return fallback


def change_messages(connection, source, uids, target=None):
    sequence = ','.join(uids)
    if target is not None:
        status, _ = connection.uid('MOVE', sequence, mailbox(target))
        if status == 'OK': return
        status, _ = connection.uid('COPY', sequence, mailbox(target))
        if status != 'OK': raise ValueError(localized('Nachrichten konnten nicht verschoben werden.', 'Messages could not be moved.'))
        status, _ = connection.uid('STORE', sequence, '+FLAGS.SILENT', '(\\Deleted)')
        if status != 'OK': raise ValueError(localized('Nachrichten wurden kopiert, konnten aber nicht aus dem ursprünglichen Ordner entfernt werden.', 'Messages were copied but could not be removed from the original folder.'))
        return
    status, _ = connection.uid('STORE', sequence, '+FLAGS.SILENT', '(\\Deleted)')
    if status != 'OK': raise ValueError(localized('Nachrichten konnten nicht gelöscht werden.', 'Messages could not be deleted.'))
    status, _ = connection.uid('EXPUNGE', sequence)
    if status != 'OK': connection.expunge()


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
    message = email.message_from_bytes(raw, policy=policy.default)
    return message


def summary(message, uid, flags):
    try: timestamp = parsedate_to_datetime(str(message.get('Date', ''))).timestamp()
    except (ValueError, TypeError, OverflowError): timestamp = 0
    return dict(uid=uid, subject=str(message.get('Subject', '(Ohne Betreff)')),
                sender=str(message.get('From', 'Unbekannt')), to=str(message.get('To', '')),
                replyTo=str(message.get('Reply-To', message.get('From', ''))), date=timestamp,
                unread=b'\\Seen' not in flags, messageID=str(message.get('Message-ID', '')))


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
            if part.get_content_type() in ('image/png','image/jpeg','image/gif','image/webp') and len(payload) <= 8*1024*1024:
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
    def cid(match): return inline.get(unquote(match.group(1).strip('<>')).lower(), '')
    markup = re.sub(r'cid:([^"\s<>]+)', cid, markup, flags=re.I)
    for location, data_url in inline_locations.items(): markup = markup.replace(location, data_url)
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
        children = list(part.iter_parts()); index = int(number)-1
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


def execute(request):
    account = config(request); password = request['password']; action = request['action']
    if action == 'send':
        # The UI invokes this only after the user's Send button. No automatic retries.
        to = recipients(request['to'])
        sender = recipients(account['address'])
        if len(sender) != 1: raise ValueError(localized('Ein Absender erforderlich.', 'A sender is required.'))
        message = EmailMessage()
        message['From'] = account['address']; message['To'] = clean_header(request['to'])
        message['Subject'] = clean_header(request['subject'])
        message['Date'] = email.utils.formatdate(localtime=True)
        message['Message-ID'] = email.utils.make_msgid()
        if request.get('replyID'): message['In-Reply-To'] = clean_header(request['replyID'])
        message.set_content(request['text'])
        if request.get('html'):
            message.add_alternative(request['html'], subtype='html')
        connection = outgoing(account, password)
        try:
            refused = connection.send_message(message, from_addr=sender[0], to_addrs=to)
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
    connection = incoming(account, password)
    try:
        if action == 'test':
            selected(connection)
            smtp = outgoing(account, password); smtp.close()
            return dict(connected=True)
        if action == 'folders': return folders(connection)
        folder = request.get('folder', 'INBOX')
        validity = selected(connection, folder, readonly=action not in ('body', 'seen', 'move', 'delete'))
        if action == 'list':
            status, data = connection.uid('search', None, 'UNDELETED')
            if status != 'OK': raise ValueError(localized('Nachrichten konnten nicht gesucht werden.', 'Could not search messages.'))
            all_uids = (data[0] or b'').split()
            limit = min(5000, max(1, int(request.get('limit', 100))))
            uids = all_uids[-limit:]
            messages = []
            if uids:
                status, rows = connection.uid('fetch', b','.join(uids), '(UID FLAGS BODY.PEEK[HEADER.FIELDS (SUBJECT FROM TO REPLY-TO DATE MESSAGE-ID)])')
                if status != 'OK': raise ValueError(localized('Nachrichtenköpfe konnten nicht geladen werden.', 'Could not load message headers.'))
                for row in rows or []:
                    if not isinstance(row, tuple): continue
                    uid = re.search(rb'UID\s+(\d+)', row[0])
                    if uid: messages.append(summary(decode_message(row[1]), uid[1].decode(), row[0]))
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
                trash_names = {'trash', 'deleted', 'deleted items', 'papierkorb', 'gelöscht', 'gelöschte elemente'}
                if source_leaf not in trash_names:
                    raise ValueError(localized('Der Anbieter meldet keinen Papierkorb. Es wurde nichts gelöscht.', 'The provider does not report a Trash folder. Nothing was deleted.'))
            if action == 'move':
                if not isinstance(target, str) or target == folder: raise ValueError(localized('Zielordner wählen.', 'Choose a destination folder.'))
            change_messages(connection, folder, uids, target)
            return dict(changed=len(uids), target=target or '')
        if action in ('body', 'attachment', 'seen'):
            uid = str(request['uid'])
            if not uid.isdigit() or request.get('validity') != validity: raise ValueError(localized('Postfach hat sich geändert. Bitte aktualisieren.', 'Mailbox changed. Please refresh.'))
            if action == 'attachment': return attachment_data(fetch_message(connection, uid), request['attachment'])
            result = body(fetch_message(connection, uid)) if action == 'body' else {}
            status, _ = connection.uid('store', uid, '+FLAGS.SILENT', '(\\Seen)')
            result['seen'] = status == 'OK'
            if status != 'OK': result['warning'] = 'Die Nachricht konnte beim Anbieter nicht als gelesen markiert werden.'
            return result
        raise ValueError(localized('Unbekannte Mail-Aktion.', 'Unknown mail action.'))
    finally:
        try: connection.logout()
        except Exception: pass


def main():
    request = json.load(sys.stdin)
    try:
        print(json.dumps(dict(ok=True, result=execute(request)), ensure_ascii=False))
    except (imaplib.IMAP4.error, smtplib.SMTPAuthenticationError):
        print(json.dumps(dict(ok=False, error=localized('Anmeldung fehlgeschlagen. Serverdaten und App-Passwort prüfen. Der Anbieter benötigt möglicherweise OAuth.', 'Sign-in failed. Check server settings and app password. The provider may require OAuth.'))))
    except Exception as error:
        # Do not return server transcripts: they may echo authentication material.
        detail = str(error) if isinstance(error, ValueError) else (
            localized('Versandstatus unklar. Bitte beim Anbieter prüfen, bevor du erneut sendest.', 'Delivery status is unclear. Check with your provider before sending again.') if request.get('action') == 'send'
            else type(error).__name__ + localized(': Verbindung oder Übertragung fehlgeschlagen.', ': Connection or transfer failed.'))
        print(json.dumps(dict(ok=False, error=detail)))


if __name__ == '__main__': main()
