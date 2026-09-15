"""Read-only browser import. Invoked only for an explicitly selected profile/file.
Secrets travel over the subprocess pipe, never command arguments. Database snapshots are private and temporary.
"""
import csv
import io
import json
import plistlib
import sqlite3
import tempfile
import os
import time
from contextlib import closing
import struct
import sys
from pathlib import Path
from html.parser import HTMLParser
from urllib.parse import urlparse

MAX_FILE = 100 * 1024 * 1024
KINDS = {'bookmarks', 'history', 'tabs', 'passwords', 'cookies'}


def localized(german, english):
    return german if __import__('os').environ.get('YOBRO_LANGUAGE', 'de') == 'de' else english


def web_url(value):
    if not isinstance(value, str):
        return False
    try:
        parsed = urlparse(value)
        return parsed.scheme in ('http', 'https') and bool(parsed.hostname)
    except ValueError:
        return False


def read_bytes(path):
    if path.stat().st_size > MAX_FILE:
        raise ValueError(localized('Die Datei ist größer als 100 MB.', 'The file exceeds 100 MB.'))
    return path.read_bytes()


def query_connection(path, sql):
    uri = path.resolve().as_uri() + '?mode=ro'
    with closing(sqlite3.connect(uri, uri=True, timeout=.25)) as db:
        db.execute('PRAGMA query_only=ON')
        return db.execute(sql).fetchall()


def fingerprint(paths):
    result = []
    for path in paths:
        try:
            stat = path.stat()
            result.append((stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns))
        except FileNotFoundError: result.append(None)
    return result


def snapshot_query(path, sql):
    # A locked Chromium profile can be read from an independent copy. Include WAL:
    # copying only the database would lose recent committed transactions.
    sources = [path, Path(str(path) + '-wal'), Path(str(path) + '-journal')]
    for attempt in range(3):
        before = fingerprint(sources)
        if before[0] is None: raise FileNotFoundError('Datenbank nicht gefunden.')
        # Never copy an active rollback transaction: its database may contain uncommitted pages.
        journal_active = False
        if before[2] and before[2][2] > 0:
            with sources[2].open('rb') as journal: journal_active = bool(journal.read(28).strip(b'\0'))
        if journal_active:
            raise ValueError(localized('Der Browser schreibt gerade in diese Datenbank. Bitte den Quellbrowser beenden und „Erneut erkennen“ wählen.', 'The browser is writing to this database. Quit the source browser and select “Detect again”.'))
        if sum(item[2] for item in before if item) > 512 * 1024 * 1024:
            raise ValueError(localized('Datenbankkopie größer als 512 MB. Bitte einen Export verwenden.', 'The database copy exceeds 512 MB. Use an export file.'))
        with tempfile.TemporaryDirectory(prefix='yobro-import-') as folder:
            os.chmod(folder, 0o700)
            target = Path(folder) / 'snapshot.sqlite'
            try:
                for source, suffix, stamp in zip(sources[:2], ['', '-wal'], before[:2]):
                    if stamp:
                        destination = Path(str(target) + suffix)
                        with source.open('rb') as original, destination.open('xb') as copied:
                            os.chmod(destination, 0o600)
                            remaining = stamp[2]
                            while remaining:
                                chunk = original.read(min(1024 * 1024, remaining))
                                if not chunk: raise OSError(localized('Datenbank wurde während des Lesens geändert.', 'The database changed while being read.'))
                                copied.write(chunk); remaining -= len(chunk)
                if before != fingerprint(sources): continue
                # SHM is transient and must be rebuilt for the private WAL copy.
                with closing(sqlite3.connect(target, timeout=.25)) as db:
                    db.execute('PRAGMA query_only=ON')
                    if db.execute('PRAGMA quick_check').fetchone() != ('ok',):
                        raise ValueError(localized('Datenbankkopie konnte nicht geprüft werden.', 'Could not verify the database copy.'))
                    return db.execute(sql).fetchall()
            except (OSError, sqlite3.DatabaseError):
                if attempt == 2: raise ValueError(localized('Keine stabile Datenbankkopie möglich. Bitte den Quellbrowser beenden und „Erneut erkennen“ wählen.', 'Could not create a stable database copy. Quit the source browser and select “Detect again”.'))
        time.sleep(.05)
    raise ValueError(localized('Die Datenbank wird laufend geändert. Bitte den Quellbrowser beenden und „Erneut erkennen“ wählen.', 'The database keeps changing. Quit the source browser and select “Detect again”.'))


def query(path, sql):
    path = Path(path)
    try: return query_connection(path, sql)
    except sqlite3.OperationalError as error:
        if 'locked' not in str(error).lower() and 'busy' not in str(error).lower(): raise
        return snapshot_query(path, sql)


def profiles(home=None):
    home = Path(home or Path.home())
    support = home / 'Library/Application Support'
    result = []
    roots = [('Chrome', support / 'Google/Chrome'), ('Brave', support / 'BraveSoftware/Brave-Browser'), ('Arc', support / 'Arc/User Data'), ('Firefox', support / 'Firefox/Profiles')]
    safari = home / 'Library/Safari'
    result.append({'id': str(safari), 'browser': 'Safari', 'name': 'Safari', 'path': str(safari)})
    for browser, root in roots:
        if not root.exists():
            continue
        try:
            dirs = sorted(p for p in root.iterdir() if p.is_dir() and (browser == 'Firefox' or p.name == 'Default' or p.name.startswith('Profile ')))
            for path in dirs:
                name = path.name
                preferences = path/'Preferences'
                if preferences.exists():
                    try: name = json.loads(read_bytes(preferences)).get('profile', {}).get('name') or name
                    except (OSError,ValueError): pass
                result.append({'id': str(path), 'browser': browser, 'name': name, 'path': str(path)})
        except OSError:
            continue
    return result


class BookmarksHTML(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.entries = []; self.href = None; self.text = []; self.folder = []; self.heading = False; self.heading_text = []; self.next_folder = None; self.levels = []
    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == 'h3': self.heading = True; self.heading_text = []
        if tag == 'dl':
            self.levels.append(self.next_folder is not None)
            if self.next_folder is not None: self.folder.append(self.next_folder)
            self.next_folder = None
        if tag == 'a': self.href = attrs.get('href'); self.text = []
    def handle_data(self, data):
        if self.href is not None: self.text.append(data)
        if self.heading: self.heading_text.append(data)
    def handle_endtag(self, tag):
        if tag == 'h3': self.heading = False; self.next_folder = ''.join(self.heading_text).strip()
        if tag == 'dl' and self.levels:
            if self.levels.pop() and self.folder: self.folder.pop()
        if tag == 'a':
            if web_url(self.href): self.entries.append({'title': ''.join(self.text).strip() or self.href, 'url': self.href, 'folder': ' / '.join(self.folder)})
            self.href = None


def chromium_bookmarks(path):
    value = json.loads(read_bytes(path))
    entries = []
    def visit(node, folders):
        if node.get('type') == 'url' and web_url(node.get('url')):
            entries.append({'title': node.get('name') or node['url'], 'url': node['url'], 'folder': ' / '.join(folders)})
        for child in node.get('children', []): visit(child, folders + ([node.get('name')] if node.get('name') else []))
    for root in value.get('roots', {}).values():
        if isinstance(root, dict): visit(root, [])
    return entries


def safari_bookmarks(path):
    entries = []
    def visit(node, folders):
        url = node.get('URLString')
        if web_url(url): entries.append({'title': node.get('URIDictionary', {}).get('title') or url, 'url': url, 'folder': ' / '.join(folders)})
        for child in node.get('Children', []): visit(child, folders + ([node['Title']] if node.get('Title') else []))
    visit(plistlib.loads(read_bytes(path)), [])
    return entries


def mozlz4(data):
    if not data.startswith(b'mozLz40\0'): return data
    if len(data) < 12: raise ValueError(localized('Unvollständige Firefox-Sitzung.', 'Incomplete Firefox session.'))
    size = struct.unpack_from('<I', data, 8)[0]
    if size > MAX_FILE: raise ValueError(localized('Firefox-Sitzung ist zu groß.', 'The Firefox session is too large.'))
    source = memoryview(data)[12:]; out = bytearray(); pos = 0
    def length(n):
        nonlocal pos
        if n == 15:
            while True:
                extra = source[pos]; pos += 1; n += extra
                if extra != 255: break
        return n
    while pos < len(source):
        token = source[pos]; pos += 1
        literals = length(token >> 4)
        if pos + literals > len(source): raise ValueError(localized('Ungültiger Firefox-Block.', 'Invalid Firefox block.'))
        out.extend(source[pos:pos+literals]); pos += literals
        if len(out) > size: raise ValueError(localized('Ungültige Firefox-Größe.', 'Invalid Firefox size.'))
        if pos == len(source): break
        distance = source[pos] | source[pos+1] << 8; pos += 2
        count = length(token & 15) + 4
        if distance == 0 or distance > len(out) or len(out) + count > size: raise ValueError(localized('Ungültiger Firefox-Block.', 'Invalid Firefox block.'))
        for _ in range(count): out.append(out[-distance])
    if len(out) != size: raise ValueError(localized('Unvollständige Firefox-Sitzung.', 'Incomplete Firefox session.'))
    return bytes(out)


def firefox_tabs(path):
    session = json.loads(mozlz4(read_bytes(path)))
    result = []
    for window in session.get('windows', []):
        for tab in window.get('tabs', []):
            entries = tab.get('entries', [])
            index = tab.get('index', len(entries)) - 1
            if 0 <= index < len(entries):
                entry = entries[index]
                if web_url(entry.get('url')): result.append({'url': entry['url'], 'title': entry.get('title') or entry['url'], 'pinned': bool(tab.get('pinned'))})
    return result


def chromium_tabs(path):
    """Replay unencrypted Session commands; never scrape navigation strings heuristically."""
    data = read_bytes(path)
    if len(data) < 8 or data[:4] != b'SNSS': raise ValueError(localized('Ungültige Chromium-Sitzung.', 'Invalid Chromium session.'))
    version = struct.unpack_from('<I', data, 4)[0]
    if version not in (1, 3): raise ValueError(localized('Verschlüsselte oder unbekannte Sitzung. Bitte Tabs als URL-Liste exportieren.', 'Encrypted or unknown session. Export tabs as a URL list.'))
    tabs = {}; position = 8; marker = version == 1
    def tab(key): return tabs.setdefault(key, {'navigations':{}, 'selected':0, 'window':None, 'position':0, 'pinned':False})
    while position < len(data):
        if position + 2 > len(data): raise ValueError(localized('Unvollständige Sitzung. Bitte den Quellbrowser schließen und erneut versuchen.', 'Incomplete session. Close the source browser and try again.'))
        size = struct.unpack_from('<H', data, position)[0]; position += 2
        if size < 1 or position + size > len(data): raise ValueError(localized('Unvollständige Sitzung. Bitte den Quellbrowser schließen und erneut versuchen.', 'Incomplete session. Close the source browser and try again.'))
        command = data[position]; payload = data[position+1:position+size]; position += size
        if command == 255: marker = True; continue
        def integer(offset=0): return struct.unpack_from('<i', payload, offset)[0]
        if command == 0: tab(integer(4))['window'] = integer()
        elif command == 2: tab(integer())['position'] = integer(4)
        elif command == 6:
            # A Pickle starts with payload size, followed by tab id, navigation index, URL and UTF-16 title.
            cursor = 4
            key = integer(cursor); cursor += 4
            index = integer(cursor); cursor += 4
            def string(width, encoding):
                nonlocal cursor
                length = integer(cursor); cursor += 4
                count = length * width
                if length < 0 or cursor + count > len(payload): raise ValueError(localized('Ungültiger Navigationseintrag.', 'Invalid navigation entry.'))
                value = payload[cursor:cursor+count].decode(encoding, errors='replace')
                cursor += (count + 3) & ~3
                return value
            url = string(1, 'utf-8'); title = string(2, 'utf-16-le')
            tab(key)['navigations'][index] = {'url':url, 'title':title or url}
        elif command == 7: tab(integer())['selected'] = integer(4)
        elif command == 12: tab(integer())['pinned'] = bool(payload[4])
        elif command in (3, 16): tabs.pop(integer(), None)
        elif command in (4, 17):
            window = integer(); tabs = {key:value for key,value in tabs.items() if value['window'] != window}
        elif command in (5, 11, 24):
            value = tab(integer()); start = integer(4) if command != 11 else 0
            count = integer(8) if command == 24 else integer(4) if command == 11 else 2**31
            value['navigations'] = {(i-count if i >= start+count else i):n for i,n in value['navigations'].items() if not start <= i < start+count}
            selected = value['selected']
            if selected >= start: value['selected'] = max(start-1, selected-count)
    if not marker: raise ValueError(localized('Die Sitzung wurde noch nicht vollständig gespeichert.', 'The session has not been fully saved yet.'))
    result = []
    for value in sorted(tabs.values(), key=lambda t:(t['window'] or 0,t['position'])):
        entries = value['navigations']
        if not entries or value['window'] is None: continue
        index = min(entries, key=lambda i:abs(i-value['selected']))
        entry = entries[index]
        if web_url(entry['url']): result.append(dict(entry, pinned=value['pinned']))
    return result


def arc_sidebar(path):
    """Read Arc's sidebar archive and preserve spaces, folders and tab placement."""
    path = Path(path)
    candidates = [
        path/'StorableSidebar.json',
        path.parent/'StorableSidebar.json',
        path.parent.parent/'StorableSidebar.json',
    ]
    source = next((candidate for candidate in candidates if candidate.is_file()), None)
    if source is None:
        raise FileNotFoundError(localized('Arc-Sidebar nicht gefunden.', 'Arc sidebar not found.'))
    value = json.loads(read_bytes(source))

    def pairs(encoded):
        if isinstance(encoded, dict):
            return dict(encoded)
        result = {}
        if not isinstance(encoded, list):
            return result
        for index in range(0, len(encoded) - 1, 2):
            key, item = encoded[index:index + 2]
            if isinstance(key, str): result[key] = item
            elif isinstance(item, dict) and isinstance(item.get('id'), str): result[item['id']] = item
        for item in encoded:
            if isinstance(item, dict) and isinstance(item.get('id'), str): result.setdefault(item['id'], item)
        return result

    def variant(value):
        return json.dumps(value, sort_keys=True, separators=(',', ':'))

    def container_ids(space):
        result = {}
        encoded = space.get('newContainerIDs') or space.get('containerIDs') or []
        if isinstance(encoded, dict):
            encoded = [part for pair in encoded.items() for part in pair]
        for index in range(0, len(encoded) - 1, 2):
            key, identifier = encoded[index:index + 2]
            if isinstance(key, dict) and key: key = next(iter(key))
            if isinstance(key, str) and isinstance(identifier, str): result[key] = identifier
        return result

    sidebar = value.get('sidebar', value)
    containers = sidebar.get('containers', []) if isinstance(sidebar, dict) else []
    workspace = next((entry for entry in containers if isinstance(entry, dict) and entry.get('spaces') is not None and entry.get('items') is not None), None)
    if workspace is None:
        raise ValueError(localized('Unbekanntes Arc-Sidebarformat.', 'Unknown Arc sidebar format.'))
    spaces_by_id = pairs(workspace.get('spaces'))
    items = pairs(workspace.get('items'))
    spaces = []
    folders = []
    tabs = []

    for source_id, space in spaces_by_id.items():
        if not isinstance(space, dict): continue
        name = str(space.get('title') or localized('Arc Space', 'Arc Space')).strip()
        spaces.append({'sourceID': source_id, 'name': name})

    def walk(space_id, item_id, folder_path=(), pinned=False, visited=None):
        visited = set() if visited is None else visited
        if item_id in visited: return
        visited.add(item_id)
        item = items.get(item_id)
        if not isinstance(item, dict): return
        data = item.get('data') or {}
        tab = data.get('tab')
        if isinstance(tab, dict) and web_url(tab.get('savedURL')):
            tabs.append({
                'url': tab['savedURL'],
                'title': item.get('title') or tab.get('savedTitle') or tab['savedURL'],
                'pinned': bool(pinned and not folder_path),
                'space': space_id,
                'tabFolderID': folder_path[-1][0] if folder_path else None,
            })
            return
        next_path = folder_path
        if 'list' in data:
            title = str(item.get('title') or localized('Arc-Ordner', 'Arc folder')).strip()
            folder_id = f'{space_id}:{item_id}'
            next_path = folder_path + ((folder_id, title),)
            folders.append({'sourceID': folder_id, 'name': ' / '.join(part[1] for part in next_path), 'space': space_id})
        for child_id in item.get('childrenIds') or []:
            if isinstance(child_id, str): walk(space_id, child_id, next_path, pinned, visited)

    for source_id, space in spaces_by_id.items():
        if not isinstance(space, dict): continue
        roots = container_ids(space)
        for kind in ('pinned', 'unpinned'):
            root = items.get(roots.get(kind))
            if not isinstance(root, dict): continue
            for child_id in root.get('childrenIds') or []:
                if isinstance(child_id, str): walk(source_id, child_id, pinned=(kind == 'pinned'))

    # Arc favorites (top apps) are shared by every space using the same Arc profile.
    top_apps = workspace.get('topAppsContainerIDs') or []
    for index in range(0, len(top_apps) - 1, 2):
        profile, root_id = top_apps[index:index + 2]
        root = items.get(root_id)
        if not isinstance(root, dict): continue
        matching = [source_id for source_id, space in spaces_by_id.items() if isinstance(space, dict) and variant(space.get('profile')) == variant(profile)]
        for space_id in matching:
            for child_id in root.get('childrenIds') or []:
                if isinstance(child_id, str): walk(space_id, child_id, pinned=True)

    # Keep the first occurrence while retaining Arc's sidebar order.
    unique_folders = {folder['sourceID']: folder for folder in folders}
    seen = set(); unique_tabs = []
    for tab in tabs:
        key = (tab['space'], tab['url'], tab.get('tabFolderID'))
        if key not in seen: seen.add(key); unique_tabs.append(tab)
    return {'spaces': spaces, 'tabFolders': list(unique_folders.values()), 'tabs': unique_tabs}


def saved_chromium_tabs(path):
    path = Path(path)
    candidates = sorted((path/'Sessions').glob('Session_*'), reverse=True)
    if not candidates: candidates = [candidate for candidate in [path/'Current Session', path/'Last Session'] if candidate.exists()]
    if not candidates:
        raise ValueError(localized('Keine gespeicherte Sitzung gefunden. Bitte eine URL-Liste als TXT oder JSON hinzufügen.', 'No saved session found. Add a URL list as TXT or JSON.'))
    return chromium_tabs(candidates[0])


def profile_data(browser, path, kinds):
    path = Path(path); result = {k: [] for k in KINDS}; result['spaces'] = []; result['tabFolders'] = []; result['warnings'] = []; result['availability'] = {}
    if not path.is_dir(): raise ValueError(localized('Profilordner nicht gefunden. Bitte den Ordner auswählen oder eine Exportdatei verwenden.', 'Profile folder not found. Select the folder or use an export file.'))
    for kind in kinds:
        try:
            if kind == 'passwords':
                count = None
                if browser in ('Chrome', 'Brave', 'Arc'):
                    databases = [path/name for name in ('Login Data', 'Login Data For Account') if (path/name).exists()]
                    if databases:
                        count = sum(query(db, 'SELECT COUNT(*) FROM logins WHERE blacklisted_by_user = 0 AND length(password_value) > 0')[0][0] for db in databases)
                elif browser == 'Firefox' and (path/'logins.json').exists():
                    count = len(json.loads(read_bytes(path/'logins.json')).get('logins', []))
                result['detectedPasswords'] = count
                result['availability'][kind] = localized(f'{count} gespeicherte Konten erkannt · Zum Übernehmen entsperren', f'{count} saved accounts detected · Unlock to transfer') if count is not None else localized('Zum Prüfen und Übernehmen entsperren', 'Unlock to check and transfer')
                if browser == 'Safari':
                    result['availability'][kind] = localized('Lokaler macOS-Schlüsselbund; iCloud bei Bedarf per Export', 'Local macOS Keychain; use an export for iCloud if needed')
            elif kind == 'bookmarks':
                if browser == 'Firefox':
                    rows = query(path/'places.sqlite', 'SELECT b.id,b.parent,b.title,p.url FROM moz_bookmarks b LEFT JOIN moz_places p ON p.id=b.fk')
                    nodes = {r[0]: r for r in rows}
                    for key, parent, title, url in rows:
                        if not web_url(url): continue
                        folders = []; seen = set()
                        while parent in nodes and parent not in seen:
                            seen.add(parent); node = nodes[parent]
                            if node[2]: folders.insert(0, node[2])
                            parent = node[1]
                        result[kind].append({'url': url, 'title': title or url, 'folder': ' / '.join(folders)})
                elif browser == 'Safari': result[kind] = safari_bookmarks(path/'Bookmarks.plist')
                else:
                    result[kind] = chromium_bookmarks(path/'Bookmarks')
            elif kind == 'history':
                if browser == 'Firefox': rows = query(path/'places.sqlite', 'SELECT url, title, last_visit_date/1000000.0, visit_count FROM moz_places WHERE last_visit_date IS NOT NULL ORDER BY last_visit_date DESC LIMIT 100000')
                elif browser == 'Safari': rows = query(path/'History.db', 'SELECT i.url, v.title, MAX(v.visit_time)+978307200, i.visit_count FROM history_items i JOIN history_visits v ON v.history_item=i.id GROUP BY i.id ORDER BY MAX(v.visit_time) DESC LIMIT 100000')
                else: rows = query(path/'History', 'SELECT url,title,last_visit_time/1000000.0-11644473600,visit_count FROM urls ORDER BY last_visit_time DESC LIMIT 100000')
                result[kind] = [{'url': u, 'title': t or u, 'timestamp': d or 0, 'visits': max(1, n or 1)} for u,t,d,n in rows if web_url(u)]
            elif kind == 'tabs':
                if browser == 'Firefox':
                    candidates = [path/'sessionstore-backups/recovery.jsonlz4', path/'sessionstore.jsonlz4', path/'sessionstore-backups/previous.jsonlz4']
                    source = next((p for p in candidates if p.exists()), None)
                    if source is None: raise ValueError(localized('Keine gespeicherte Firefox-Sitzung gefunden.', 'No saved Firefox session found.'))
                    result[kind] = firefox_tabs(source)
                elif browser == 'Safari':
                    value = plistlib.loads(read_bytes(path/'LastSession.plist'))
                    for window in value.get('SessionWindows', []):
                        for tab in window.get('TabStates', []):
                            u = tab.get('TabURL')
                            if web_url(u): result[kind].append({'url':u,'title':tab.get('TabTitle') or u,'pinned':False})
                elif browser == 'Arc':
                    try:
                        sidebar = arc_sidebar(path)
                        result[kind] = sidebar['tabs']; result['spaces'] = sidebar['spaces']; result['tabFolders'] = sidebar['tabFolders']
                        result['availability'][kind] = localized(
                            f"{len(result[kind])} Tabs in {len(result['spaces'])} Spaces und {len(result['tabFolders'])} Ordnern erkannt",
                            f"{len(result[kind])} tabs in {len(result['spaces'])} spaces and {len(result['tabFolders'])} folders detected")
                    except FileNotFoundError:
                        result[kind] = saved_chromium_tabs(path)
                        result['warnings'].append(localized('Arc-Sidebar nicht gefunden; offene Tabs wurden aus der Chromium-Sitzung gelesen.', 'Arc sidebar not found; open tabs were read from the Chromium session.'))
                else:
                    result[kind] = saved_chromium_tabs(path)
            elif kind == 'cookies':
                if browser == 'Firefox':
                    columns = {r[1] for r in query(path/'cookies.sqlite', 'PRAGMA table_info(moz_cookies)')}
                    same_site = 'sameSite' if 'sameSite' in columns else 'NULL'
                    rows = query(path/'cookies.sqlite', f'SELECT name,value,host,path,expiry,isSecure,isHttpOnly,{same_site} FROM moz_cookies WHERE originAttributes = "" LIMIT 100000')
                    result[kind] = [{'name':n,'value':v,'domain':h,'path':p,'expires':e,'secure':bool(s),'httpOnly':bool(o),'sameSite':{0:'none',1:'lax',2:'strict'}.get(site)} for n,v,h,p,e,s,o,site in rows]
                else:
                    result['availability'][kind] = localized('Cookie-Speicher erkannt · Export zum Übertragen erforderlich', 'Cookie store detected · Export required for transfer') if any((path/p).exists() for p in ['Cookies','Network/Cookies','Cookies.binarycookies']) else localized('Cookie-Export erforderlich', 'Cookie export required')
                    result['warnings'].append(localized(f'{browser}: Cookies bitte als Netscape-TXT oder JSON hinzufügen. Verschlüsselte Cookies und Safari-Cookiecontainer werden nicht direkt importiert.', f'{browser}: add cookies as Netscape TXT or JSON. Encrypted cookies and Safari cookie containers are not imported directly.'))
        except (OSError, ValueError, sqlite3.Error, KeyError, IndexError, struct.error) as error:
            title = {'bookmarks':localized('Lesezeichen', 'Bookmarks'),'history':localized('Verlauf', 'History'),'tabs':localized('Offene Tabs', 'Open tabs'),'passwords':localized('Passwörter', 'Passwords'),'cookies':'Cookies'}.get(kind,kind)
            result['warnings'].append(f'{title}: {error}')
    for kind in kinds:
        if kind not in result['availability']: result['availability'][kind] = localized(f'{len(result[kind])} Einträge automatisch erkannt', f'{len(result[kind])} items detected automatically') if result[kind] else localized('Keine direkt lesbaren Einträge · Exportdatei möglich', 'No directly readable items · Export file supported')
    return result


def file_data(kind, path):
    path = Path(path); data = read_bytes(path); text = data.decode('utf-8-sig') if path.suffix not in ('.jsonlz4',) else ''
    result = {k: [] for k in KINDS}; result['spaces'] = []; result['tabFolders'] = []; result['warnings'] = []; result['availability'] = {}
    if kind == 'passwords':
        reader = csv.DictReader(io.StringIO(text))
        for row in reader:
            row = {k.lower().strip(): v for k,v in row.items() if k}
            url = row.get('url') or row.get('website') or row.get('login_uri')
            if web_url(url) and row.get('password'):
                result[kind].append({'url': url, 'username': row.get('username') or row.get('login_username') or '', 'password': row['password']})
    elif kind == 'bookmarks':
        if path.suffix.lower() == '.json': result[kind] = chromium_bookmarks(path)
        elif path.suffix.lower() == '.plist': result[kind] = safari_bookmarks(path)
        else:
            parser = BookmarksHTML(); parser.feed(text); result[kind] = parser.entries
    elif kind == 'tabs':
        if path.suffix.lower() == '.jsonlz4': result[kind] = firefox_tabs(path)
        elif path.suffix.lower() == '.json':
            value = json.loads(text)
            if isinstance(value, dict) and 'windows' in value: result[kind] = firefox_tabs(path)
            else:
                for item in value if isinstance(value,list) else value.get('tabs',[]):
                    if isinstance(item,str): item = {'url':item}
                    if web_url(item.get('url')): result[kind].append({'url':item['url'],'title':item.get('title') or item['url'],'pinned':bool(item.get('pinned'))})
        else: result[kind] = [{'url':u.strip(),'title':u.strip(),'pinned':False} for u in text.splitlines() if web_url(u.strip())]
    elif kind == 'history':
        value = json.loads(text)
        rows = value if isinstance(value,list) else value.get('history', value.get('Browser History', []))
        for row in rows:
            if web_url(row.get('url')): result[kind].append({'url':row['url'],'title':row.get('title') or row['url'],'timestamp':row.get('timestamp', row.get('time_usec',0)/1000000),'visits':row.get('visits',1)})
    elif kind == 'cookies':
        if path.suffix.lower() == '.json':
            value = json.loads(text)
            for c in value if isinstance(value,list) else value.get('cookies',[]):
                if c.get('partitionKey') or c.get('firstPartyDomain'):
                    result['warnings'].append(localized('Ein partitioniertes Cookie wurde übersprungen, da sein Isolationskontext nicht übertragen werden kann.', 'A partitioned cookie was skipped because its isolation context cannot be transferred.')); continue
                result[kind].append({'name':c['name'],'value':c['value'],'domain':c['domain'],'path':c.get('path','/'),'expires':c.get('expirationDate',c.get('expires',0)) or 0,'secure':bool(c.get('secure')),'httpOnly':bool(c.get('httpOnly')),'sameSite':{'no_restriction':'none','unspecified':None}.get(c.get('sameSite'),c.get('sameSite'))})
        else:
            for line in text.splitlines():
                http_only = line.startswith('#HttpOnly_')
                if http_only: line = line[len('#HttpOnly_'):]
                if not line or line.startswith('#'): continue
                fields = line.split('\t')
                if len(fields) != 7: raise ValueError(localized('Ungültiges Netscape-Cookieformat: sieben tabgetrennte Felder erwartet.', 'Invalid Netscape cookie format: expected seven tab-separated fields.'))
                domain, _, p, secure, expires, name, value = fields
                result[kind].append({'name':name,'value':value,'domain':domain,'path':p,'expires':float(expires),'secure':secure.upper()=='TRUE','httpOnly':http_only})
    if not result[kind]: result['warnings'].append(localized('Keine unterstützten Einträge in dieser Datei gefunden.', 'No supported items found in this file.'))
    return result


def chromium_passwords(path):
    """Read encrypted rows only; Keychain authorization and decryption stay native."""
    import base64
    rows = []
    found = False
    for name in ('Login Data', 'Login Data For Account'):
        database = Path(path) / name
        if not database.exists():
            continue
        found = True
        for url, username, encrypted in query(database, 'SELECT origin_url, username_value, password_value FROM logins WHERE blacklisted_by_user = 0'):
            if web_url(url) and encrypted:
                rows.append(dict(url=url, username=username or '', encrypted=base64.b64encode(encrypted).decode('ascii')))
    if not found:
        raise ValueError(localized('In diesem Profil wurde keine Passwortdatenbank gefunden. Wähle ein anderes Profil oder eine Exportdatei.', 'No password database found in this profile. Choose another profile or an export file.'))
    return rows


def firefox_passwords(path, primary_password=''):
    """Use Firefox's NSS in a short-lived process, with a read-only profile."""
    import base64
    import ctypes as c
    candidates = [Path('/Applications/Firefox.app/Contents/MacOS'), Path.home()/'Applications/Firefox.app/Contents/MacOS', Path('/Applications/Firefox Developer Edition.app/Contents/MacOS')]
    library = next((p for p in candidates if (p/'libnss3.dylib').is_file()), None)
    if library is None:
        raise ValueError(localized('Für die direkte Übernahme muss Firefox auf diesem Mac installiert sein. Alternativ eine Passwort-CSV verwenden.', 'Firefox must be installed on this Mac for direct transfer. Alternatively, use a password CSV.'))
    try:
        if (library/'libmozglue.dylib').exists():
            c.CDLL(str(library/'libmozglue.dylib'), mode=c.RTLD_GLOBAL)
        nss = c.CDLL(str(library/'libnss3.dylib'))
    except OSError:
        raise ValueError(localized('Die Passwortbibliothek von Firefox konnte nicht geladen werden. Verwende einen CSV-Export.', 'Could not load the Firefox password library. Use a CSV export.')) from None

    class SECItem(c.Structure):
        _fields_ = [('type', c.c_uint), ('data', c.POINTER(c.c_ubyte)), ('len', c.c_uint)]
    nss.NSS_InitReadOnly.argtypes = [c.c_char_p]; nss.NSS_InitReadOnly.restype = c.c_int
    nss.NSS_Shutdown.argtypes = []; nss.NSS_Shutdown.restype = c.c_int
    nss.PK11_GetInternalKeySlot.argtypes = []; nss.PK11_GetInternalKeySlot.restype = c.c_void_p
    nss.PK11_FreeSlot.argtypes = [c.c_void_p]; nss.PK11_FreeSlot.restype = None
    nss.PK11_CheckUserPassword.argtypes = [c.c_void_p, c.c_char_p]; nss.PK11_CheckUserPassword.restype = c.c_int
    nss.PK11SDR_Decrypt.argtypes = [c.POINTER(SECItem), c.POINTER(SECItem), c.c_void_p]; nss.PK11SDR_Decrypt.restype = c.c_int
    nss.SECITEM_FreeItem.argtypes = [c.POINTER(SECItem), c.c_int]; nss.SECITEM_FreeItem.restype = None
    value = json.loads(read_bytes(Path(path)/'logins.json'))
    if nss.NSS_InitReadOnly(('sql:' + str(Path(path).resolve())).encode()) != 0:
        raise ValueError(localized('Der Firefox-Passwortspeicher konnte nicht geöffnet werden. Schließe Firefox und versuche es erneut.', 'Could not open the Firefox password store. Close Firefox and try again.'))
    slot = None
    try:
        slot = nss.PK11_GetInternalKeySlot()
        if not slot:
            raise ValueError(localized('Firefox-Passwortschlüssel nicht gefunden.', 'Firefox password key not found.'))
        if nss.PK11_CheckUserPassword(slot, primary_password.encode('utf-8')) != 0:
            return dict(passwords=[], warnings=[], needsPrimaryPassword=True)
        def decrypt(encoded):
            data = base64.b64decode(encoded, validate=True)
            buffer = (c.c_ubyte * len(data)).from_buffer_copy(data)
            source = SECItem(0, buffer, len(data)); output = SECItem()
            try:
                if nss.PK11SDR_Decrypt(c.byref(source), c.byref(output), None) != 0:
                    raise ValueError('NSS decrypt failed')
                return c.string_at(output.data, output.len).decode('utf-8')
            finally:
                if output.data:
                    nss.SECITEM_FreeItem(c.byref(output), 0)
        rows, skipped = [], 0
        for item in value.get('logins', []):
            if not web_url(item.get('hostname')):
                skipped += 1; continue
            try:
                password = decrypt(item['encryptedPassword'])
                if password:
                    rows.append(dict(url=item['hostname'], username=decrypt(item['encryptedUsername']), password=password))
            except (ValueError, KeyError, UnicodeError):
                skipped += 1
        warnings = [localized(f'{skipped} Firefox-Konten konnten nicht übernommen werden. Verwende dafür einen CSV-Export.', f'Could not transfer {skipped} Firefox accounts. Use a CSV export for these accounts.')] if skipped else []
        return dict(passwords=rows, warnings=warnings)
    finally:
        if slot:
            nss.PK11_FreeSlot(slot)
        nss.NSS_Shutdown()


def main():
    try:
        request = json.load(sys.stdin)
        if request['action'] == 'chromium_passwords': result = chromium_passwords(request['path'])
        elif request['action'] == 'firefox_passwords': result = firefox_passwords(request['path'], request.get('primaryPassword', ''))
        elif request['action'] == 'profiles': result = profiles(request.get('home'))
        elif request['action'] == 'profile':
            kinds = set(request['kinds'])
            if not kinds <= KINDS: raise ValueError(localized('Unbekannte Datenart', 'Unknown data type'))
            result = profile_data(request['browser'], request['path'], kinds)
        elif request['action'] == 'file' and request['kind'] in KINDS: result = file_data(request['kind'], request['path'])
        else: raise ValueError(localized('Unbekannte Importaktion', 'Unknown import action'))
        json.dump({'ok':True,'result':result}, sys.stdout, ensure_ascii=False)
    except Exception as error:
        # Never echo source rows or secret values in errors.
        json.dump({'ok':False,'error':str(error) if isinstance(error,(OSError,ValueError,sqlite3.Error)) else 'Die Datei konnte nicht gelesen werden.'}, sys.stdout)

if __name__ == '__main__': main()
