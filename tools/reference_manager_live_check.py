#!/usr/bin/env python3
"""Check reference lookup, persistence and pane behavior in a real editor."""

import argparse
import json
from pathlib import Path
import tempfile
import zlib

from fold_live_check import Mep, Xvfb


def type_keys(editor, text):
    editor.rpc.call('ui.type_text', {'text': text})
    editor.pump(len(text) + 4)


def choose(editor, query):
    type_keys(editor, query)
    editor.key('Return')


def write_pdf(path, info=b'<< >>', xmp=b''):
    catalog = b'<< /Type /Catalog /Pages 2 0 R'
    if xmp:
        catalog += b' /Metadata 5 0 R'
    objects = [
        catalog + b' >>',
        b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
        b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] >>',
        info,
    ]
    if xmp:
        payload = zlib.compress(xmp)
        objects.append(b'<< /Type /Metadata /Subtype /XML /Filter /FlateDecode /Length '
                       + str(len(payload)).encode() + b' >>\nstream\n'
                       + payload + b'\nendstream')
    pdf = bytearray(b'%PDF-1.4\n')
    offsets = []
    for i, obj in enumerate(objects, 1):
        offsets.append(len(pdf))
        pdf += f'{i} 0 obj\n'.encode() + obj + b'\nendobj\n'
    start = len(pdf)
    pdf += f'xref\n0 {len(objects) + 1}\n0000000000 65535 f \n'.encode()
    for offset in offsets:
        pdf += f'{offset:010d} 00000 n \n'.encode()
    pdf += (f'trailer\n<< /Size {len(objects) + 1} /Root 1 0 R /Info 4 0 R >>\n'
            f'startxref\n{start}\n%%EOF\n').encode()
    path.write_bytes(pdf)


def wait_for_citation(editor, path, key):
    for _ in range(25):
        content = path.read_text()
        if f'\\citation({key},' in content:
            return content
        editor.pump(5)
    raise AssertionError(f'{key} was not imported\n{editor.log_tail()}')


def check(editor, project, library, folder):
    # Record the data the real picker renders, while still rendering it.
    editor.cmd('lua local show = mep.picker_set_preview; '
               'mep.picker_set_preview = function(text, ...) reference_preview = text; show(text, ...) end')
    editor.key('space')
    editor.key('r')
    editor.key('i')
    assert 'Project citation' in editor.lua_value('reference_preview')
    assert editor.lua_value('reference_preview').count('duplicate') == 1
    type_keys(editor, 'zzzz')
    assert editor.lua_value('reference_preview') == ''
    editor.key('BackSpace', 4)
    assert 'Project citation' in editor.lua_value('reference_preview')
    editor.key('Return')
    assert editor.line(1).startswith(r'\cite(duplicate)')
    before = (project / 'citations.mepml').read_text()
    assert before.count(r'\citation(duplicate,') == 2

    editor.cmd('MepRefInsert')
    type_keys(editor, 'projectbib')
    preview = editor.lua_value('reference_preview')
    assert 'Project (Bib' in preview and '10-first.bib' in preview, preview
    assert 'Inherited Journal' in preview, preview
    editor.key('Return')
    assert r'\cite(projectbib)' in editor.line(1)
    saved = (project / 'citations.mepml').read_text()
    assert saved.startswith(before) and 'Project (Bib' in saved and 'Inherited Journal' in saved
    assert len(saved) > len(before)
    editor.cmd('MepRefInsert')
    type_keys(editor, 'globalonly')
    preview = editor.lua_value('reference_preview')
    assert 'Global preferred' in preview and 'library.mepml' in preview, preview
    editor.key('Return')
    assert r'\cite(globalonly)' in editor.line(1)
    saved = (project / 'citations.mepml').read_text()
    assert str(folder / 'paper.txt') in saved, saved
    editor.cmd('MepRefInsert')
    type_keys(editor, 'folderonly')
    preview = editor.lua_value('reference_preview')
    assert 'Folder Bib' in preview and 'sources.bib' in preview, preview
    editor.key('Escape')
    assert saved == (project / 'citations.mepml').read_text()

    editor.cmd('lua mep.pane_open(' + json.dumps(str(project / 'notes.tex')) + ')')
    editor.cmd('lua mep.set_cursor(1,1)')
    editor.cmd('MepRefInsert')
    choose(editor, 'projectbib')
    assert editor.line(1).startswith(r'\cite{projectbib}'), (editor.lua_value('mep.filename()'), editor.line(1))
    assert saved == (project / 'citations.mepml').read_text()

    editor.cmd('lua mep.pane_open(' + json.dumps(str(project / 'citations.mepml')) + ')')
    editor.cmd("lua mep.insert_text('// Unsaved change')")
    editor.cmd('lua mep.pane_open(' + json.dumps(str(project / 'notes.tex')) + ')')
    editor.cmd('MepRefInsert')
    choose(editor, 'folderonly')
    assert r'\cite{folderonly}' not in editor.line(1)
    assert saved == (project / 'citations.mepml').read_text()

    # Save a subscribed-file stub, then check that the collection, edit,
    # and actual pane-opening callbacks are available through the UI.
    editor.cmd('MepRefManager')
    assert editor.lua_value("mep.filename():match('Reference Manager$')") == 'Reference Manager'
    manager_pane = int(editor.lua_value('mep.current_pane_id()'))
    editor.cmd('MepRefEdit')
    choose(editor, 'loose-notes')
    choose(editor, 'save to library')
    global_text = (library / 'library.mepml').read_text()
    assert r'\citation(loose-notes,' in global_text and str(folder / 'loose notes.txt') in global_text
    editor.cmd('MepRefCollectionAdd')
    choose(editor, 'reading')
    config = library / 'config.mepml'
    assert '//? Reference collection: reading' in config.read_text()

    editor.cmd('MepRefCollections')
    choose(editor, 'add reference to collection')
    choose(editor, 'reading')
    choose(editor, 'loose-notes')
    assert '//? Reference key: loose-notes' in config.read_text()
    editor.cmd('MepRefCollectionRename')
    choose(editor, 'reading')
    editor.key('BackSpace', len('reading'))
    choose(editor, 'research')
    assert '//? Reference collection: research' in config.read_text()

    # The promoted file appears just above the final folder BibTeX entry.
    editor.cmd('MepRefRefresh')
    editor.shift_key('g')
    editor.key('k')
    editor.key('Return')
    assert editor.lua_value('mep.filename()') == str(folder / 'loose notes.txt'), editor.log_tail()
    state = editor.rpc.call('state.dump')
    root = state['tabs'][state['active_tab']]['root']
    assert root['dir'] == 'vertical'
    left, right = (child['pane']['id'] for child in root['children'])
    assert left == manager_pane and right == int(editor.lua_value('mep.current_pane_id()'))

    editor.cmd('MepRefUnsubscribe')
    editor.key('Return')
    assert '//? Reference folder:' not in config.read_text()
    nested = folder / 'volumes'
    nested.mkdir()
    write_pdf(nested / 'sample.PDF', b'<< /Title (Less specific title) /Author (PDF Author)'
                                   b' /CreationDate (D:20100101000000) >>',
              b'<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF '
              b'xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">'
              b'<rdf:Description xmlns:dc="http://purl.org/dc/elements/1.1/" '
              b'xmlns:prism="http://prismstandard.org/namespaces/basic/2.0/">'
              b'<dc:title><rdf:Alt><rdf:li xml:lang="x-default">Verified XMP Title</rdf:li>'
              b'</rdf:Alt></dc:title><dc:creator><rdf:Seq><rdf:li>First Author</rdf:li>'
              b'<rdf:li>Second Author</rdf:li></rdf:Seq></dc:creator>'
              b'<prism:publicationDate>2022-10-03</prism:publicationDate>'
              b'<prism:doi>10.1000/example</prism:doi>'
              b'</rdf:Description></rdf:RDF></x:xmpmeta>')
    write_pdf(nested / 'untagged book.pdf', b'<< /Title (Untitled) /CreationDate (D:19990101) >>')
    write_pdf(folder / 'info.pdf', b'<< /Title (PDF Info Title) /Author (Info Author) >>')
    # Browse from the project to its sibling folder, inspect a file,
    # then explicitly subscribe. Opening the file row keeps the picker open.
    editor.cmd("lua local open, preview = mep.picker_open, mep.picker_preview_file; "
               "mep.picker_open = function(title, ...) browser_title = title; open(title, ...) end; "
               "mep.picker_preview_file = function(path, lines, visual) "
               "browser_file = path; browser_visual = visual; preview(path, lines, visual) end")
    editor.cmd('MepRefSubscribe')
    assert editor.lua_value('browser_title') == str(project)
    assert 'notes.tex' in editor.lua_value('reference_preview')
    choose(editor, '../')
    assert editor.lua_value('browser_title') == str(project.parent)
    choose(editor, 'papers')
    assert editor.lua_value('browser_title') == str(folder)
    assert 'paper.txt' in editor.lua_value('reference_preview')
    type_keys(editor, 'paper.txt')
    assert editor.lua_value('browser_file') == str(folder / 'paper.txt')
    assert editor.lua_value('browser_visual') == 'true'
    editor.key('Return')
    assert editor.lua_value('browser_title') == str(folder)
    assert '//? Reference folder:' not in config.read_text()
    choose(editor, 'Subscribe to this folder')
    assert '//? Reference folder: ' + str(folder) in config.read_text()

    library_path = library / 'library.mepml'
    imported = wait_for_citation(editor, library_path, 'untagged-book')
    assert 'Verified XMP Title' in imported and 'First Author and Second Author' in imported, imported
    assert 'year = {2022}' in imported and 'doi = {10.1000/example}' in imported
    assert 'year = {1999}' not in imported and 'year = {2010}' not in imported
    assert 'title = {untagged book}' in imported and 'title_source = {filename}' in imported
    assert 'PDF Info Title' in imported and 'Info Author' in imported
    assert str(nested / 'sample.PDF') in imported

    corrected = imported.replace('Verified XMP Title', 'Reviewed Title')
    library_path.write_text(corrected)
    write_pdf(nested / 'later.pdf')
    editor.cmd('MepRefRefresh')
    refreshed = wait_for_citation(editor, library_path, 'later')
    assert refreshed.startswith(corrected) and refreshed.count(r'\citation(sample,') == 1, refreshed

    editor.cmd('MepRefSubscribe')
    choose(editor, 'Type a folder path')
    choose(editor, '../papers')
    assert editor.lua_value('browser_title') == str(folder)
    editor.key('Escape')
    assert editor.lua_value('tostring(mep.picker_is_open())') == 'false'
    editor.cmd('MepRefCollectionRemove')
    choose(editor, 'research')
    assert '//? Reference collection:' not in config.read_text()

    second = project.parent / 'second'
    second.mkdir()
    (second / 'notes.mepml').write_text('text\n')
    editor.cmd('project ' + str(second))
    editor.cmd('lua mep.pane_open(' + json.dumps(str(second / 'notes.mepml')) + ')')
    editor.cmd('MepRefInsert')
    choose(editor, 'folderonly')
    assert r'\cite(folderonly)' in editor.line(1)
    assert r'\citation(folderonly,' in (second / 'citations.mepml').read_text()
    assert saved == (project / 'citations.mepml').read_text()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build-dir', default='build/native')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='mepref-') as tmp:
        home = Path(tmp)
        project = home / 'project'
        folder = home / 'papers'
        library = home / 'share/mep/references'
        for directory in (project, folder, library):
            directory.mkdir(parents=True)
        (project / 'notes.mepml').write_text('text\n')
        (project / 'notes.tex').write_text('text\n')
        (project / 'citations.mepml').write_text(
            '// Keep this comment.\n'
            '\\citation(duplicate, title = {Project citation})\n'
            '\\citation(duplicate, title = {Later MEPML duplicate})\n')
        (project / '10-first.bib').write_text(
            '@string{venue = "Inherited Journal"}\n'
            '@article{projectbib, title = {Project (Bib}, crossref = {parent}}\n'
            '@article{duplicate, title = {Project Bib duplicate}}\n')
        (project / '20-second.bib').write_text(
            '@article{parent, journal = venue}\n'
            '@article{projectbib, title = {Later Bib duplicate}}\n')
        (library / 'library.mepml').write_text(
            '\\citation(projectbib, title = {Global duplicate})\n'
            '\\citation(globalonly, title = {Global preferred}, file = {../../../papers/paper.txt})\n')
        (folder / 'sources.bib').write_text(
            '@article{globalonly, title = {Folder duplicate}}\n'
            '@article{folderonly, title = {Folder Bib}}\n')
        (folder / 'paper.txt').write_text('Attached paper\n')
        (folder / 'loose notes.txt').write_text('Uncatalogued file\n')
        (library / 'config.mepml').write_text('//? Reference folder: ' + str(folder) + '\n')

        display = Xvfb()
        editor = None
        try:
            editor = Mep(args.build_dir, tmp, display.display, [str(project / 'notes.mepml')], cwd=project)
            check(editor, project, library, folder)
        except Exception:
            if editor:
                print(editor.log_tail())
            raise
        finally:
            if editor:
                editor.kill()
            display.close()
    print('Reference lookup order, preview, insertion, collections, promotion and right pane passed')


if __name__ == '__main__':
    main()
