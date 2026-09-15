import sys
sys.dont_write_bytecode = True
import importlib.util
import json
import plistlib
import sqlite3
import struct
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('importer', Path(__file__).resolve().parents[1]/'Sources/YOBRO/Resources/BrowserImport.py')
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

class BrowserImportTests(unittest.TestCase):
    def setUp(self): self.tmp = tempfile.TemporaryDirectory(); self.root = Path(self.tmp.name)
    def tearDown(self): self.tmp.cleanup()
    def file(self, name, value):
        p = self.root/name; p.write_text(value); return p
    def test_html_folders_entities_and_unsafe_urls(self):
        p = self.file('bookmarks.html', '<DL><DT><H3>Work &amp; Fun</H3><DL><DT><A HREF="https://example.org/?a=1&amp;b=2">A &amp; B</A></DL><DT><A HREF="javascript:alert(1)">Bad</A><DT><A HREF="https://outside.org">Outside</A></DL>')
        entries = m.file_data('bookmarks', p)['bookmarks']
        self.assertEqual(len(entries), 2); self.assertEqual(entries[0]['folder'], 'Work & Fun'); self.assertEqual(entries[1]['folder'], '')
        self.assertEqual(entries[0]['url'], 'https://example.org/?a=1&b=2')
    def test_password_csv_formats_and_quoting(self):
        for browser, header in [('Chrome','name,url,username,password'),('Safari','Title,URL,Username,Password'),('Firefox','name,url,username,password')]:
            p = self.file(browser+'.csv', header+'\nExample,https://example.org,a,"comma, and\nnewline"\n')
            row = m.file_data('passwords',p)['passwords'][0]
            self.assertEqual(row['password'], 'comma, and\nnewline')
    def test_chromium_history_epoch_and_source_unchanged(self):
        p = self.root/'History'
        with sqlite3.connect(p) as db:
            db.execute('CREATE TABLE urls(url,title,last_visit_time,visit_count)')
            db.execute('INSERT INTO urls VALUES(?,?,?,?)',('https://example.org','Test',11644473600000000+1700000000000000,7))
        before=p.read_bytes()
        result=m.profile_data('Chrome',self.root,{'history'})
        self.assertEqual(result['history'][0]['timestamp'],1700000000)
        self.assertEqual(p.read_bytes(),before)
    def test_firefox_bookmarks_history_and_cookies(self):
        with sqlite3.connect(self.root/'places.sqlite') as db:
            db.executescript('CREATE TABLE moz_places(id,url,title,last_visit_date,visit_count); CREATE TABLE moz_bookmarks(id,parent,title,fk);')
            db.execute('INSERT INTO moz_places VALUES(1,"https://example.org","Page",1700000000000000,3)')
            db.execute('INSERT INTO moz_bookmarks VALUES(1,0,"Folder",NULL)'); db.execute('INSERT INTO moz_bookmarks VALUES(2,1,"Bookmark",1)')
        with sqlite3.connect(self.root/'cookies.sqlite') as db:
            db.execute('CREATE TABLE moz_cookies(name,value,host,path,expiry,isSecure,isHttpOnly,originAttributes)')
            db.execute('INSERT INTO moz_cookies VALUES("session","synthetic","example.org","/",1900000000,1,1,"")')
        result=m.profile_data('Firefox',self.root,{'bookmarks','history','cookies'})
        self.assertEqual(result['bookmarks'][0]['folder'],'Folder'); self.assertEqual(result['history'][0]['visits'],3)
        self.assertTrue(result['cookies'][0]['httpOnly'])
    def test_safari_bookmarks_and_tabs(self):
        (self.root/'Bookmarks.plist').write_bytes(plistlib.dumps({'Children':[{'URLString':'https://example.org','URIDictionary':{'title':'Safari'}}]}))
        (self.root/'LastSession.plist').write_bytes(plistlib.dumps({'SessionWindows':[{'TabStates':[{'TabURL':'https://example.org','TabTitle':'Open'}]}]}))
        result=m.profile_data('Safari',self.root,{'bookmarks','tabs'})
        self.assertEqual(result['bookmarks'][0]['title'],'Safari'); self.assertEqual(result['tabs'][0]['title'],'Open')
    def test_mozlz4_and_current_navigation_only(self):
        raw=json.dumps({'windows':[{'tabs':[{'index':2,'pinned':True,'entries':[{'url':'https://old.org'},{'url':'https://current.org'}]}]}]}).encode()
        # Literal-only valid LZ4 block.
        extra=len(raw)-15; lengths=bytearray()
        while extra>=255: lengths.append(255); extra-=255
        lengths.append(extra)
        compressed=b'mozLz40\0'+struct.pack('<I',len(raw))+b'\xf0'+lengths+raw
        p=self.root/'session.jsonlz4';p.write_bytes(compressed)
        rows=m.file_data('tabs',p)['tabs']; self.assertEqual(len(rows),1);self.assertEqual(rows[0]['url'],'https://current.org');self.assertTrue(rows[0]['pinned'])
        with self.assertRaises((ValueError,IndexError)): m.mozlz4(compressed[:-1])
    def test_netscape_and_json_cookies(self):
        p=self.file('cookies.txt','# Netscape HTTP Cookie File\n#HttpOnly_.example.org\tTRUE\t/\tTRUE\t1900000000\tsession\tsynthetic\n')
        c=m.file_data('cookies',p)['cookies'][0];self.assertTrue(c['httpOnly']);self.assertTrue(c['secure'])
        p=self.file('cookies.json',json.dumps([{'name':'x','value':'y','domain':'example.org','expirationDate':1900000000}]))
        self.assertEqual(m.file_data('cookies',p)['cookies'][0]['expires'],1900000000)
    def test_chromium_tabs_selected_navigation_closed_tabs_and_encryption(self):
        def record(command, payload): return struct.pack('<H',len(payload)+1)+bytes([command])+payload
        def navigation(key,index,url):
            text=url.encode(); title='Title'.encode('utf-16-le')
            body=struct.pack('<iii',key,index,len(text))+text+b'\0'*((-len(text))%4)+struct.pack('<i',5)+title+b'\0'*((-len(title))%4)
            return record(6,struct.pack('<I',len(body))+body)
        data=b'SNSS'+struct.pack('<I',3)
        data+=record(0,struct.pack('<ii',10,1))+navigation(1,0,'https://old.org')+navigation(1,1,'https://current.org')+record(7,struct.pack('<ii',1,1))
        data+=record(0,struct.pack('<ii',10,2))+navigation(2,0,'https://closed.org')+record(16,struct.pack('<iq',2,0))+record(255,b'')
        p=self.root/'Session_1'; p.write_bytes(data)
        self.assertEqual([r['url'] for r in m.chromium_tabs(p)],['https://current.org'])
        p.write_bytes(b'SNSS'+struct.pack('<I',5)+data[8:])
        with self.assertRaises(ValueError): m.chromium_tabs(p)

    def test_arc_sidebar_preserves_spaces_folders_and_pinning(self):
        profile = self.root/'User Data'/'Default'; profile.mkdir(parents=True)
        def tab(identifier, parent, url, title):
            return {'id':identifier,'parentID':parent,'title':None,'childrenIds':[],
                    'data':{'tab':{'savedURL':url,'savedTitle':title}}}
        items = {
            'pinned-root': {'id':'pinned-root','parentID':None,'childrenIds':['direct','folder'],
                            'data':{'itemContainer':{'containerType':{'spaceItems':{'_0':'space-1'}}}}},
            'open-root': {'id':'open-root','parentID':None,'childrenIds':['open'],
                          'data':{'itemContainer':{'containerType':{'spaceItems':{'_0':'space-1'}}}}},
            'folder': {'id':'folder','parentID':'pinned-root','title':'Work','childrenIds':['nested','inside'],'data':{'list':{}}},
            'nested': {'id':'nested','parentID':'folder','title':'Research','childrenIds':[],'data':{'list':{}}},
            'direct': tab('direct','pinned-root','https://pinned.example','Pinned'),
            'inside': tab('inside','folder','https://folder.example','Folder tab'),
            'open': tab('open','open-root','https://open.example','Open'),
        }
        space = {'id':'space-1','title':'Studio','profile':{'default':True},
                 'newContainerIDs':[{'pinned':{}},'pinned-root',{'unpinned':{'_0':{'shared':{}}}},'open-root']}
        flatten = lambda values: [part for key,value in values.items() for part in (key,value)]
        archive = {'sidebar':{'containers':[{'global':{}},{'spaces':['space-1',space],
                   'items':flatten(items),'topAppsContainerIDs':[]} ]}}
        (self.root/'StorableSidebar.json').write_text(json.dumps(archive))
        result = m.profile_data('Arc',profile,{'tabs'})
        self.assertEqual(result['spaces'],[{'sourceID':'space-1','name':'Studio'}])
        self.assertEqual([(f['name'],f['sourceID']) for f in result['tabFolders']],
                         [('Work','space-1:folder'),('Work / Research','space-1:nested')])
        by_url = {row['url']:row for row in result['tabs']}
        self.assertTrue(by_url['https://pinned.example']['pinned'])
        self.assertFalse(by_url['https://folder.example']['pinned'])
        self.assertEqual(by_url['https://folder.example']['tabFolderID'],'space-1:folder')
        self.assertEqual(by_url['https://open.example']['space'],'space-1')

    def test_exclusive_browser_lock_uses_private_copy(self):
        db = sqlite3.connect(self.root/'History')
        try:
            db.execute('PRAGMA locking_mode=EXCLUSIVE')
            db.execute('CREATE TABLE urls(url,title,last_visit_time,visit_count)')
            db.execute('INSERT INTO urls VALUES("https://example.org","Locked",13344473600000000,2)'); db.commit()
            before = (self.root/'History').read_bytes()
            result = m.profile_data('Arc',self.root,{'history'})
            self.assertEqual(result['history'][0]['title'],'Locked')
            self.assertEqual(result['warnings'],[])
            self.assertEqual((self.root/'History').read_bytes(),before)
        finally: db.close()

    def test_snapshot_includes_committed_wal_but_not_uncommitted_rows(self):
        db=sqlite3.connect(self.root/'History')
        try:
            db.execute('PRAGMA journal_mode=WAL'); db.execute('PRAGMA wal_autocheckpoint=0')
            db.execute('CREATE TABLE fixture(value)'); db.commit()
            db.execute('INSERT INTO fixture VALUES("committed")'); db.commit()
            db.execute('INSERT INTO fixture VALUES("uncommitted")')
            self.assertEqual(m.snapshot_query(self.root/'History','SELECT value FROM fixture'), [('committed',)])
        finally: db.close()

    def test_snapshot_rejects_active_rollback_transaction(self):
        db=sqlite3.connect(self.root/'History')
        try:
            db.execute('CREATE TABLE fixture(value)'); db.commit()
            db.execute('BEGIN EXCLUSIVE'); db.execute('INSERT INTO fixture VALUES("uncommitted")')
            with self.assertRaisesRegex(ValueError,'Browser schreibt'):
                m.snapshot_query(self.root/'History','SELECT value FROM fixture')
        finally: db.close()

    def test_snapshot_rejects_changing_source_and_cleans_up(self):
        from unittest.mock import patch
        db=sqlite3.connect(self.root/'History'); db.execute('CREATE TABLE fixture(value)'); db.commit(); db.close()
        original=m.fingerprint
        calls=0
        def changing(paths):
            nonlocal calls
            calls+=1
            result=original(paths)
            result[0]=result[0][:-1]+(calls,)
            return result
        with patch.object(m,'fingerprint',side_effect=changing):
            with self.assertRaisesRegex(ValueError,'laufend geändert'): m.snapshot_query(self.root/'History','SELECT * FROM fixture')

    def test_partial_profile_reports_missing_sources(self):
        result=m.profile_data('Brave',self.root,{'bookmarks','history','passwords','cookies','tabs'})
        self.assertEqual(len(result['warnings']),4); self.assertFalse(result['passwords'])
        self.assertIsNone(result['detectedPasswords'])
        self.assertIn('entsperren', result['availability']['passwords'])
    def test_encrypted_passwords_detect_both_stores_without_decrypting(self):
        for name in ('Login Data', 'Login Data For Account'):
            with sqlite3.connect(self.root/name) as db:
                db.execute('CREATE TABLE logins(origin_url, username_value, password_value, blacklisted_by_user)')
                db.execute('INSERT INTO logins VALUES(?,?,?,0)', ('https://example.invalid','test',b'v10synthetic'))
                db.execute('INSERT INTO logins VALUES(?,?,?,1)', ('https://blocked.invalid','test',b'ignored'))
        preview = m.profile_data('Chrome', self.root, {'passwords'})
        self.assertEqual(preview['detectedPasswords'], 2)
        self.assertEqual(preview['passwords'], [])
        self.assertEqual(len(m.chromium_passwords(self.root)), 2)

    def test_tabs_reject_local_and_script_urls(self):
        p=self.file('tabs.txt','file:///etc/passwd\njavascript:alert(1)\nhttps://example.org\n')
        self.assertEqual(len(m.file_data('tabs',p)['tabs']),1)

if __name__=='__main__': unittest.main()
