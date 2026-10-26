"""Render the GitHub wiki into docs/wiki/ in the look of the website.

The wiki stays the one source. Edit a page there and push it, then run

    python tools/wiki_site.py [path to the wiki clone]

and commit what changed under docs/wiki/ and in docs/sitemap.xml. The clone
defaults to CapView-wiki next to this repository.

No dependencies: the converter covers the Markdown the wiki uses (headings,
paragraphs, lists, tables, fenced code, quotes, links, images, code, bold and
italics), and names every link or image it cannot resolve.
"""

import html
import json
import re
import struct
import sys
from pathlib import Path
from string import Template

REPO = Path(__file__).resolve().parent.parent
DOCS = REPO / 'docs'
OUT = DOCS / 'wiki'
SITE = 'https://qblank.com/'
GH_WIKI = 'https://github.com/NuclearMeltdown/qBlank/wiki'
RAW_DOCS = 'https://raw.githubusercontent.com/NuclearMeltdown/qBlank/main/docs/'

HOME_TITLE = 'Wiki'
HOME_TAG = 'How each part of qBlank works, which file it lives in, and why it was built the way it was.'

problems = []


def warn(msg):
    problems.append(msg)


def attr(s):
    return html.escape(s, quote=True)


def plain(fragment):
    """The text of an HTML fragment, as GitHub reads it for an anchor."""
    return html.unescape(re.sub(r'<[^>]+>', '', fragment))


def page_file(name):
    return 'index.html' if name == 'Home' else name.lower() + '.html'


def page_href(name):
    return './' if name == 'Home' else name.lower() + '.html'


def image_size(path):
    """Width and height of a PNG or JPEG, read from its header."""
    data = path.read_bytes()
    if data[:8] == b'\x89PNG\r\n\x1a\n':
        return struct.unpack('>II', data[16:24])
    if data[:2] == b'\xff\xd8':
        i = 2
        while i + 9 < len(data):
            if data[i] != 0xFF:
                i += 1
                continue
            marker = data[i + 1]
            length = struct.unpack('>H', data[i + 2:i + 4])[0]
            if marker in (0xC0, 0xC1, 0xC2):
                h, w = struct.unpack('>HH', data[i + 5:i + 9])
                return w, h
            i += 2 + length
    return None


# ------------------------------------------------------------------ inline

CODE = re.compile(r'(`+)(.+?)(?<!`)\1(?!`)', re.S)
ESCAPED = re.compile(r'\\([\\`*_{}\[\]()#+\-.!|<>])')
IMG = re.compile(r'!\[([^\]]*)\]\(([^)\s]+)\)')
LINK = re.compile(r'\[([^\]]+)\]\(([^)\s]+)\)')
BOLD = re.compile(r'\*\*(?=\S)(.+?)(?<=\S)\*\*', re.S)
EM = re.compile(r'(?<!\*)\*(?=[^\s*])(.+?)(?<=[^\s*])\*(?!\*)', re.S)
HOLE = re.compile('\x00(\\d+)\x00')


class Page:
    def __init__(self, name, names):
        self.name = name
        self.names = names            # lower-case name -> wiki name
        self.ids = set()
        self.counts = {}
        self.links = []               # (target page, fragment) to check at the end

    def slug(self, fragment):
        base = re.sub(r'[^\w\- ]', '', plain(fragment).strip().lower()).replace(' ', '-') or 'section'
        n = self.counts.get(base)
        self.counts[base] = 0 if n is None else n + 1
        s = base if n is None else '%s-%d' % (base, n + 1)
        self.ids.add(s)
        return s

    def link(self, url):
        if url.startswith('#'):
            self.links.append((self.name, url[1:]))
            return url
        if url.startswith(RAW_DOCS):
            return '../' + url[len(RAW_DOCS):]
        if url.startswith(SITE):
            return '../' + url[len(SITE):]
        if url == GH_WIKI or url.startswith(GH_WIKI + '/'):
            url = url[len(GH_WIKI) + 1:] or 'Home'
        elif re.match(r'^[a-z][a-z0-9+.-]*:', url, re.I) or url.startswith(('/', '.')):
            return url
        name, _, frag = url.partition('#')
        target = self.names.get(name.lower())
        if not target:
            warn('%s: link to a page that does not exist: %s' % (self.name, url))
            return url
        if frag:
            self.links.append((target, frag))
        return page_href(target) + ('#' + frag if frag else '')

    def image(self, url):
        src = self.link(url)
        size = None
        if src.startswith('../'):
            local = DOCS / src[3:]
            if local.is_file():
                size = image_size(local)
            else:
                warn('%s: image not in docs/: %s' % (self.name, src[3:]))
        return src, size


def inline(text, page):
    stash = []

    def keep(s):
        stash.append(s)
        return '\x00%d\x00' % (len(stash) - 1)

    def code(m):
        body = m.group(2).replace('\n', ' ')
        if len(body) > 2 and body[0] == ' ' and body[-1] == ' ' and body.strip():
            body = body[1:-1]
        return keep('<code>%s</code>' % html.escape(body, quote=False))

    def img(m):
        src, size = page.image(html.unescape(m.group(2)))
        dims = ' width="%d" height="%d"' % size if size else ''
        return keep('<img src="%s" alt="%s"%s loading="lazy">' % (attr(src), attr(html.unescape(m.group(1))), dims))

    def link(m):
        return keep('<a href="%s">' % attr(page.link(html.unescape(m.group(2))))) + m.group(1) + keep('</a>')

    text = CODE.sub(code, text)
    text = ESCAPED.sub(lambda m: keep(html.escape(m.group(1), quote=False)), text)
    text = html.escape(text, quote=False)
    text = IMG.sub(img, text)
    text = LINK.sub(link, text)
    text = BOLD.sub(r'<strong>\1</strong>', text)
    text = EM.sub(r'<em>\1</em>', text)
    while '\x00' in text:
        text = HOLE.sub(lambda m: stash[int(m.group(1))], text)
    return text


# ------------------------------------------------------------------ blocks

FENCE = re.compile(r'^( {0,3})(`{3,}|~{3,})(.*)$')
HEAD = re.compile(r'^ {0,3}(#{1,6})\s+(.*?)(?:\s+#+)?\s*$')
ITEM = re.compile(r'^( {0,3})([-*+]|\d{1,9}[.)])( +|$)(.*)$')
TABLE_SEP = re.compile(r'^\s*\|?\s*:?-+:?\s*(\|\s*:?-+:?\s*)*\|?\s*$')
IMG_ONLY = re.compile(r'^!\[([^\]]*)\]\(([^)\s]+)\)$')
SRC_ONLY = re.compile(r'^(`[^`]+`\s*(,|and)?\s*)+$')


def indent(line):
    return len(line) - len(line.lstrip(' '))


def is_table(lines, i):
    return (lines[i].lstrip().startswith('|') and i + 1 < len(lines)
            and TABLE_SEP.match(lines[i + 1]) is not None)


def interrupts(lines, i):
    """Whether this line ends the paragraph above it."""
    line = lines[i]
    if HEAD.match(line) or FENCE.match(line) or line.lstrip().startswith('>') or is_table(lines, i):
        return True
    m = ITEM.match(line)
    return bool(m and m.group(4).strip() and (not m.group(2)[0].isdigit() or m.group(2)[:-1] == '1'))


def cells(row):
    row = row.strip()
    if row.startswith('|'):
        row = row[1:]
    if row.endswith('|') and not row.endswith('\\|'):
        row = row[:-1]
    return [c.strip().replace('\\|', '|') for c in re.split(r'(?<!\\)\|', row)]


def parse(lines):
    out = []
    i, n = 0, len(lines)
    while i < n:
        line = lines[i]
        if not line.strip():
            i += 1
            continue

        m = FENCE.match(line)
        if m:
            pad, fence = len(m.group(1)), m.group(2)
            close = re.compile(r'^ {0,3}%s{%d,}\s*$' % (re.escape(fence[0]), len(fence)))
            body = []
            i += 1
            while i < n and not close.match(lines[i]):
                body.append(lines[i][min(pad, indent(lines[i])):])
                i += 1
            i += 1
            out.append(('code', '\n'.join(body)))
            continue

        m = HEAD.match(line)
        if m:
            out.append(('h', len(m.group(1)), m.group(2)))
            i += 1
            continue

        if line.lstrip().startswith('>'):
            body = []
            while i < n and lines[i].lstrip().startswith('>'):
                body.append(re.sub(r'^\s*> ?', '', lines[i]))
                i += 1
            out.append(('quote', parse(body)))
            continue

        if is_table(lines, i):
            head = cells(lines[i])
            align = []
            for c in cells(lines[i + 1]):
                align.append('center' if c.startswith(':') and c.endswith(':') else
                             'right' if c.endswith(':') else '')
            rows = []
            i += 2
            while i < n and lines[i].lstrip().startswith('|'):
                row = cells(lines[i])
                rows.append((row + [''] * len(head))[:len(head)])
                i += 1
            out.append(('table', head, align, rows))
            continue

        if ITEM.match(line):
            i = parse_list(lines, i, out)
            continue

        body = [line.strip()]
        i += 1
        while i < n and lines[i].strip() and not interrupts(lines, i):
            body.append(lines[i].strip())
            i += 1
        out.append(('p', '\n'.join(body)))
    return out


def parse_list(lines, i, out):
    n = len(lines)
    first = ITEM.match(lines[i])
    ordered = first.group(2)[0].isdigit()
    start = int(first.group(2)[:-1]) if ordered else 1
    items, loose = [], False
    while i < n:
        m = ITEM.match(lines[i])
        if not m or m.group(2)[0].isdigit() != ordered:
            break
        width = len(m.group(1)) + len(m.group(2)) + (len(m.group(3)) if m.group(4) else 1)
        body = [m.group(4)]
        i += 1
        while i < n:
            line = lines[i]
            if not line.strip():
                j = i
                while j < n and not lines[j].strip():
                    j += 1
                if j < n and indent(lines[j]) >= width:
                    body.extend([''] * (j - i))
                    i = j
                    continue
                break
            if indent(line) >= width:
                body.append(line[width:])
            elif body[-1].strip() and not interrupts(lines, i) and not ITEM.match(line):
                body.append(line.strip())       # a lazy continuation of the text
            else:
                break
            i += 1
        while body and not body[-1].strip():
            body.pop()
        if any(not b.strip() for b in body):
            loose = True
        items.append(parse(body))
        if i < n and not lines[i].strip():
            j = i
            while j < n and not lines[j].strip():
                j += 1
            m = ITEM.match(lines[j]) if j < n else None
            if m and m.group(2)[0].isdigit() == ordered and indent(lines[j]) < width:
                loose = True
                i = j
                continue
            break
    out.append(('list', ordered, start, loose, items))
    return i


def figure(text, page, caption=None):
    m = IMG_ONLY.match(text)
    src, size = page.image(m.group(2))
    name = src[3:] if src.startswith('../') else ''
    classes = ['ui'] if name.startswith(('settings-', 'shots/')) else []
    if size and size[0] < (528 if classes else 760):
        classes.append('own')
    dims = ' width="%d" height="%d"' % size if size else ''
    cls = ' class="%s"' % ' '.join(classes) if classes else ''
    cap = '\n<figcaption>%s</figcaption>' % inline(caption, page) if caption else ''
    return '<figure%s><img src="%s" alt="%s"%s loading="lazy">%s</figure>' % (
        cls, attr(src), attr(m.group(1)), dims, cap)


# A paragraph in italics right under a picture is its caption.
CAPTION = re.compile(r'^\*(?!\*)(.+)(?<!\*)\*$', re.S)


def render(tokens, page, tight=False):
    out = []
    skip = False
    for k, t in enumerate(tokens):
        kind = t[0]
        if skip:
            skip = False
            continue
        if kind == 'p':
            text = t[1]
            if IMG_ONLY.match(text):
                nxt = tokens[k + 1] if k + 1 < len(tokens) else None
                cap = CAPTION.match(nxt[1]) if nxt and nxt[0] == 'p' else None
                if cap and '*' not in cap.group(1).replace('**', ''):
                    skip = True
                out.append(figure(text, page, cap.group(1) if skip else None))
            elif tight:
                out.append(inline(text, page))
            else:
                cls = ' class="src"' if SRC_ONLY.match(text) else ''
                out.append('<p%s>%s</p>' % (cls, inline(text, page)))
        elif kind == 'h':
            level = min(max(t[1], 3), 6)
            body = inline(t[2], page)
            out.append('<h%d id="%s">%s</h%d>' % (level, page.slug(body), body, level))
        elif kind == 'code':
            out.append('<pre><code>%s</code></pre>' % html.escape(t[1], quote=False))
        elif kind == 'quote':
            out.append('<blockquote>\n%s\n</blockquote>' % '\n'.join(render(t[1], page)))
        elif kind == 'table':
            head, align, rows = t[1], t[2], t[3]

            def row(tag, values):
                tds = []
                for k, v in enumerate(values):
                    style = ' style="text-align:%s"' % align[k] if k < len(align) and align[k] else ''
                    tds.append('<%s%s>%s</%s>' % (tag, style, inline(v, page), tag))
                return '<tr>' + ''.join(tds) + '</tr>'

            body = ([row('th', head)] if any(head) else []) + [row('td', r) for r in rows]
            out.append('<div class="tbl"><table>\n%s\n</table></div>' % '\n'.join(body))
        elif kind == 'list':
            ordered, start, loose, items = t[1], t[2], t[3], t[4]
            tag = 'ol' if ordered else 'ul'
            first = ' start="%d"' % start if ordered and start != 1 else ''
            lis = ['<li>%s</li>' % '\n'.join(render(item, page, tight=not loose)) for item in items]
            out.append('<%s%s>\n%s\n</%s>' % (tag, first, '\n'.join(lis), tag))
    return out


# ------------------------------------------------------------------ pages

TEMPLATE = Template('''<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>$title</title>
<meta name="description" content="$desc">
<link rel="icon" href="../icon.png">
<link rel="canonical" href="$url">
<script type="application/ld+json">$crumbs</script>
<link rel="stylesheet" href="../site.css">
<script>
// The theme before anything is painted. The wiki is in English only, so unlike
// the other pages this one sends no German reader to a German copy.
(function(){
  try {
    var t = localStorage.getItem('qblank-theme');
    if (t === 'light' || t === 'dark') document.documentElement.setAttribute('data-theme', t);
  } catch (e) {}
})();
</script>
</head>
<body>
<!-- Made from the GitHub wiki by tools/wiki_site.py. Edit the wiki, then run it again. -->

<nav class="topbar" aria-label="Site">
  <div class="wrap bar">
    <a class="brand" href="../"><img src="../icon.png" alt="">qBlank</a>
    <div class="links">
      <a href="../install.html">Download</a>
      <a href="../manual.html">Manual</a>
      <a href="../faq.html">Troubleshooting</a>
      <a href="../compare.html">Compare</a>
      <a href="../changelog.html">Changelog</a>
      <a href="./" aria-current="$current">Wiki</a>
      <a href="https://github.com/NuclearMeltdown/qBlank">GitHub</a>
    </div>
    <div class="tools">
      <button class="theme-toggle" id="themeToggle" type="button" hidden>
        <svg id="themeIcon" viewBox="0 0 24 24" aria-hidden="true"></svg>
      </button>
    </div>
  </div>
</nav>

<div class="page-head">
  <div class="wrap">
    <h1>$h1</h1>
    <p class="tag">$tag</p>
  </div>
</div>

<div class="wrap doc wiki">
  <nav class="toc">
    <div class="secs">
$toc
    </div>
  </nav>

  <main>
$body
$pager
  </main>
</div>

<footer>
  <div class="wrap row">
    <div>GPLv3 licensed · © 2026 NuclearMeltdown</div>
    <div><a href="../">Home</a> · <a href="../manual.html">Manual</a> · <a href="../install.html">Download</a> · <a href="../faq.html">Troubleshooting</a> · <a href="../compare.html">Compare</a> · <a href="../changelog.html">Changelog</a> · <a href="https://github.com/NuclearMeltdown/qBlank">GitHub</a> · <a href="./">Wiki</a></div>
  </div>
</footer>

<script src="../site.js"></script>
</body>
</html>
''')


def contents(home_md):
    """The pages in the order of Home's tables, each with what it covers."""
    order, covers = [], {}
    for m in re.finditer(r'^\|\s*\[([^\]]+)\]\(([^)#]+)\)\s*\|\s*(.+?)\s*\|\s*$', home_md, re.M):
        order.append(m.group(2))
        covers[m.group(2)] = (m.group(1), m.group(3))
    return order, covers


def build(name, md, names, order, covers):
    page = Page(name, names)
    lines = md.replace('\r\n', '\n').split('\n')
    while lines and not lines[0].strip():
        lines.pop(0)
    m = HEAD.match(lines[0]) if lines else None
    if m and len(m.group(1)) == 1:
        title = inline(m.group(2), page)
        lines = lines[1:]
    else:
        title = html.escape(name.replace('-', ' '))
        warn('%s: no "# title" on the first line' % name)

    if name == 'Home':
        h1, tag, desc = HOME_TITLE, HOME_TAG, HOME_TAG
        head_title = 'qBlank wiki — how it works and why'
    else:
        h1 = title
        cover = covers.get(name, (None, None))[1]
        if not cover:
            warn('%s: not in the tables on Home' % name)
        tag = inline(cover + '.', page) if cover else ''
        desc = plain(tag) or plain(title)
        head_title = '%s — qBlank wiki' % plain(title)

    # Each section is rendered before the next heading takes its anchor, so the
    # anchors are numbered in the order of the page, as on GitHub.
    intro, sections = [], []
    current, pending = intro, []
    for tok in parse(lines) + [('h', 2, None)]:
        if tok[0] == 'h' and tok[1] == 2:
            current.extend(render(pending, page))
            pending = []
            if tok[2] is None:
                break
            head = inline(tok[2], page)
            current = []
            sections.append((page.slug(head), head, current))
        else:
            pending.append(tok)

    def block(lines_):
        return '\n'.join('      ' + b for b in lines_)

    parts = []
    if intro:
        parts.append('    <section>\n%s\n    </section>' % block(intro))
    for sid, head, body in sections:
        parts.append('    <section id="%s">\n      <h2>%s</h2>\n%s\n    </section>' % (sid, head, block(body)))
    toc = '\n'.join('      <a href="#%s">%s</a>' % (sid, html.escape(plain(head), quote=False))
                    for sid, head, _ in sections)

    seq = ['Home'] + order
    pager = ''
    if name in seq:
        k = seq.index(name)
        label = lambda p: 'Contents' if p == 'Home' else covers.get(p, (p.replace('-', ' '),))[0]
        links = []
        if k > 0:
            links.append('      <a class="prev" href="%s"><span>&larr; Previous</span>%s</a>'
                         % (page_href(seq[k - 1]), html.escape(label(seq[k - 1]))))
        if k + 1 < len(seq):
            links.append('      <a class="next" href="%s"><span>Next &rarr;</span>%s</a>'
                         % (page_href(seq[k + 1]), html.escape(label(seq[k + 1]))))
        pager = '    <nav class="pager" aria-label="Wiki pages">\n%s\n    </nav>' % '\n'.join(links)

    url = SITE + 'wiki/' + ('' if name == 'Home' else page_file(name))
    crumbs = [{'@type': 'ListItem', 'position': 1, 'name': 'qBlank', 'item': SITE},
              {'@type': 'ListItem', 'position': 2, 'name': 'Wiki', 'item': SITE + 'wiki/'}]
    if name != 'Home':
        crumbs.append({'@type': 'ListItem', 'position': 3, 'name': plain(title), 'item': url})
    text = TEMPLATE.substitute(
        title=html.escape(head_title, quote=False),
        desc=attr(desc),
        url=url,
        crumbs=json.dumps({'@context': 'https://schema.org', '@type': 'BreadcrumbList',
                           'itemListElement': crumbs}, ensure_ascii=False, separators=(',', ':')),
        current='page' if name == 'Home' else 'true',
        h1=h1, tag=tag, toc=toc, body='\n\n'.join(parts), pager=pager)
    return text, page, url


def sitemap(urls):
    path = DOCS / 'sitemap.xml'
    text = path.read_bytes().decode('utf-8')
    eol = '\r\n' if '\r\n' in text else '\n'
    block = [ '  <!-- wiki: written by tools/wiki_site.py, English only -->']
    for u in urls:
        block += ['  <url>', '    <loc>%s</loc>' % u, '  </url>']
    block.append('  <!-- /wiki -->')
    block = eol.join(block) + eol
    old = re.compile(r'  <!-- wiki:.*?<!-- /wiki -->\r?\n', re.S)
    text = old.sub(lambda m: block, text) if old.search(text) else text.replace('</urlset>', block + '</urlset>')
    path.write_bytes(text.encode('utf-8'))


def main():
    sys.stdout.reconfigure(errors='replace')
    wiki = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO.parent / 'CapView-wiki'
    sources = {p.stem: p.read_text(encoding='utf-8-sig')
               for p in sorted(wiki.glob('*.md')) if not p.stem.startswith('_')}
    if 'Home' not in sources:
        sys.exit('no Home.md in %s' % wiki)
    names = {k.lower(): k for k in sources}
    order, covers = contents(sources['Home'])
    order = [p for p in order if p in sources] + sorted(p for p in sources if p not in order and p != 'Home')

    eol = '\r\n' if b'\r\n' in (DOCS / 'index.html').read_bytes()[:4096] else '\n'
    OUT.mkdir(exist_ok=True)
    built, urls = {}, []
    for name in ['Home'] + order:
        text, page, url = build(name, sources[name], names, order, covers)
        built[name] = page
        urls.append(url)
        target = OUT / page_file(name)
        data = text.replace('\n', eol).encode('utf-8')
        if not target.is_file() or target.read_bytes() != data:
            target.write_bytes(data)
    for f in OUT.glob('*.html'):
        if f.name not in {page_file(n) for n in built}:
            f.unlink()
            print('removed', f.name)

    for name, page in built.items():
        for target, frag in page.links:
            if frag not in built[target].ids:
                warn('%s: no #%s on %s' % (name, frag, target))
    sitemap(urls)

    print('%d pages in %s' % (len(built), OUT))
    for p in problems:
        print('warning:', p)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
