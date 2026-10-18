// Shared by every page of the site. The German pages sit under de/ and say so
// in <html lang>, which is all the script needs to know about where it is.
var de = document.documentElement.lang === 'de';
function L(en, german) { return de ? german : en; }

// The light/dark switch. The system decides until the button is used, and from
// then on the button does -- kept in this browser and nowhere else.
(function(){
  var root = document.documentElement;
  var btn = document.getElementById('themeToggle');
  var icon = document.getElementById('themeIcon');
  if (!btn || !icon) return;
  var sun = '<circle cx="12" cy="12" r="4.2"/><path d="M12 2.6v2.2M12 19.2v2.2M2.6 12h2.2M19.2 12h2.2M5.3 5.3l1.6 1.6M17.1 17.1l1.6 1.6M18.7 5.3l-1.6 1.6M6.9 17.1l-1.6 1.6"/>';
  var moon = '<path d="M20.5 14.3A8.5 8.5 0 0 1 9.7 3.5a8.5 8.5 0 1 0 10.8 10.8z"/>';
  var light = window.matchMedia('(prefers-color-scheme: light)');

  function current(){
    var set = root.getAttribute('data-theme');
    if (set === 'light' || set === 'dark') return set;
    return light.matches ? 'light' : 'dark';
  }
  // The icon shows where the button goes, not where you are.
  function paint(){
    var dark = current() === 'dark';
    icon.innerHTML = dark ? sun : moon;
    var label = dark ? L('Switch to the light theme', 'Zum hellen Design wechseln')
                     : L('Switch to the dark theme', 'Zum dunklen Design wechseln');
    btn.setAttribute('aria-label', label);
    btn.title = label;
  }
  btn.hidden = false;
  paint();
  btn.addEventListener('click', function(){
    var next = current() === 'dark' ? 'light' : 'dark';
    root.setAttribute('data-theme', next);
    try { localStorage.setItem('qblank-theme', next); } catch (e) {}
    paint();
  });
  if (light.addEventListener) light.addEventListener('change', paint);
})();

// The language switch is a plain link and works without this. What this adds
// is memory: a language picked by hand is kept, and the head of each English
// page reads it before painting (see the inline script there). And the place:
// both languages are built block for block alike, so the block a third of the
// way down the screen, and how far into it, is found again on the other page.
(function(){
  var KEY = 'qblank-pos';
  function blocks() { return document.querySelectorAll('header, .page-head, section, h3, figure, table, .card, .note'); }
  // Unmoved by the transform the scroll reveal starts a section with.
  function top(el) { for (var y = 0; el; el = el.offsetParent) y += el.offsetTop; return y; }
  function line() { return window.innerHeight / 3; }
  function page(path) { return path.replace(/index\.html$/, ''); }

  var link = document.getElementById('langSwitch');
  if (link) link.addEventListener('click', function(){
    try { localStorage.setItem('qblank-lang', link.getAttribute('hreflang')); } catch (e) {}
    // The block that starts last above the line. If the line runs through it,
    // how far in, as a share of it; if it has ended, how far below its start:
    // the text after a heading is longer in one language than in the other.
    var y = window.scrollY + line(), list = blocks(), at = -1;
    for (var i = 0; i < list.length && top(list[i]) <= y; ++i) at = i;
    var max = document.documentElement.scrollHeight - window.innerHeight;
    var pos = {to: page(link.pathname), n: list.length, i: at, r: max > 0 ? window.scrollY / max : 0, t: Date.now()};
    if (at >= 0) {
      var d = y - top(list[at]), h = list[at].offsetHeight;
      if (d < h) pos.f = d / h; else pos.d = d;
    }
    try { sessionStorage.setItem(KEY, JSON.stringify(pos)); } catch (e) {}
  });

  var pos = null;
  try { pos = JSON.parse(sessionStorage.getItem(KEY)); sessionStorage.removeItem(KEY); } catch (e) {}
  if (!pos || pos.to !== page(location.pathname) || Date.now() - pos.t > 15000) return;
  var root = document.documentElement, moved = false;
  function restore(){
    if (moved) return;
    var list = blocks(), y;
    if (list.length === pos.n && pos.i >= 0) {
      var el = list[pos.i];
      y = top(el) + (pos.f !== undefined ? pos.f * el.offsetHeight : pos.d) - line();
    } else {
      y = pos.r * (root.scrollHeight - window.innerHeight);
    }
    root.style.scrollBehavior = 'auto';
    window.scrollTo(0, Math.max(0, y));
    root.style.scrollBehavior = '';
  }
  restore();
  // Pictures without a set size still grow as they load; once they have, the
  // place is taken again, unless you have started scrolling yourself.
  ['wheel', 'touchstart', 'keydown', 'mousedown'].forEach(function(e){
    window.addEventListener(e, function(){ moved = true; }, {once: true, passive: true});
  });
  if (document.readyState !== 'complete') window.addEventListener('load', restore);
})();

// The contents of the manual mark the section you are reading. On a narrow
// screen they are a bar along the top that scrolls sideways, and it keeps the
// marked one in view.
(function(){
  var toc = document.querySelector('.toc');
  if (!toc) return;
  var links = [].filter.call(toc.querySelectorAll('a[href^="#"]'), function(a){
    return document.getElementById(a.getAttribute('href').slice(1));
  });
  var on = null;
  // Ten positions, at most once a frame: browsers send scroll no more often.
  function mark(){
    var y = Math.min(window.innerHeight / 3, 160), pick = links[0];
    links.forEach(function(a){
      if (document.getElementById(a.getAttribute('href').slice(1)).getBoundingClientRect().top <= y) pick = a;
    });
    // The last sections can be too short to reach the line: at the bottom of
    // the page it is the last one you are reading.
    if (window.innerHeight + window.scrollY >= document.documentElement.scrollHeight - 2) pick = links[links.length - 1];
    if (pick === on) return;
    if (on) on.classList.remove('on');
    on = pick;
    on.classList.add('on');
    if (toc.scrollWidth > toc.clientWidth) {
      toc.scrollTo({left: on.offsetLeft - toc.clientWidth / 3, behavior: 'smooth'});
    }
  }
  window.addEventListener('scroll', mark, {passive: true});
  window.addEventListener('resize', mark);
  mark();
})();

// The copy button beside the winget command. The clipboard is only there on a
// secure page; where it is not, the button stays hidden and the command can
// still be selected by hand.
(function(){
  var btn = document.getElementById('copyCmd');
  var cmd = document.getElementById('cmd');
  if (!btn || !cmd || !navigator.clipboard || !window.isSecureContext) return;
  var svg = btn.querySelector('svg');
  var copy = svg.innerHTML;
  var done = '<path d="M5 12.5l4.5 4.5L19 7.5"/>';
  var idle = L('Copy the command', 'Befehl kopieren');
  var timer = 0;
  btn.hidden = false;
  btn.addEventListener('click', function(){
    navigator.clipboard.writeText(cmd.textContent).then(function(){
      svg.innerHTML = done;
      btn.title = L('Copied', 'Kopiert');
      clearTimeout(timer);
      timer = setTimeout(function(){ svg.innerHTML = copy; btn.title = idle; }, 1600);
    }, function(){});
  });
})();

// The note beside the winget command. Until the package is merged into the
// community repository it says "coming soon"; from then on its folder there
// lists the versions, and the note names the newest instead. Read from
// winget-pkgs itself, so nothing here has to change on the day of the merge.
(function(){
  var note = document.querySelector('.cmd .soon');
  if (!note) return;
  function parts(v) { return v.split('.').map(Number); }
  function newer(a, b) {
    var p = parts(a), q = parts(b);
    for (var i = 0; i < Math.max(p.length, q.length); ++i) {
      if ((p[i] || 0) !== (q[i] || 0)) return (p[i] || 0) > (q[i] || 0);
    }
    return false;
  }
  fetch('https://api.github.com/repos/microsoft/winget-pkgs/contents/manifests/n/NuclearMeltdown/qBlank')
    .then(function(r){ return r.ok ? r.json() : null; })
    .then(function(list){
      if (!Array.isArray(list)) return;
      var best = '';
      list.forEach(function(x){
        if (x.type === 'dir' && /^\d+(\.\d+)*$/.test(x.name) && (!best || newer(x.name, best))) best = x.name;
      });
      if (!best) return;
      note.textContent = 'v' + best;
      note.title = L('The version on winget', 'Die Version auf winget');
      note.classList.add('ver');
    })
    .catch(function(){});
})();

// Scroll reveal, and nothing that breaks without JavaScript: the sections are
// visible by default if this never runs.
(function(){
  var els = document.querySelectorAll('.reveal');
  if (!('IntersectionObserver' in window)) {
    els.forEach(function(e){ e.classList.add('on'); });
    return;
  }
  var io = new IntersectionObserver(function(entries){
    entries.forEach(function(en){
      if (en.isIntersecting) { en.target.classList.add('on'); io.unobserve(en.target); }
    });
  }, {rootMargin:'0px 0px -12% 0px', threshold:0.08});
  els.forEach(function(e){ io.observe(e); });
})();

// A word inside a release asset's label. The label reads "qBlank.exe (app-x64)"
// -- a file name for whoever is looking at the release page, a word for whoever
// is picking the file -- so the word is looked for inside it. The hyphen counts
// as part of a word, or "app" would match halfway into "app-x64".
function hasWord(text, word) {
  return new RegExp('(^|[^A-Za-z0-9_-])' + word + '($|[^A-Za-z0-9_-])').test(text || '');
}

// Which file in a release is the program. The same question the updater answers
// in src/update/release_source.cpp, and it has to be answered the same way: by
// the label, never by position. A release also carries the migrator, that one is
// uploaded first, and it would win any race for "the first .exe in the list".
function pickProgram(assets) {
  var exes = (assets || []).filter(function(x){ return /\.exe$/i.test(x.name); });
  function labelled(word) {
    return exes.filter(function(x){ return hasWord(x.label, word); })[0];
  }
  // x64 for every visitor: Windows on ARM runs it under emulation, and a browser
  // cannot be asked reliably which of the two it is running on.
  var exe = labelled('app-x64') || labelled('app');
  if (exe) return exe;

  // A release whose labels say nothing this page knows: the largest executable
  // that is not the migrator. The migrator is a few hundred kilobytes and the
  // program is megabytes.
  return exes.filter(function(x){ return !hasWord(x.label, 'migrator'); })
             .sort(function(p, q){ return q.size - p.size; })[0];
}

// Name the newest release on the download button, so it says what you get.
// Falls back silently to the generic /releases/latest link. Only on a page
// that has the button.
(function(){
  var a = document.getElementById('dl');
  if (!a) return;
  fetch('https://api.github.com/repos/NuclearMeltdown/qBlank/releases/latest')
    .then(function(r){ return r.ok ? r.json() : null; })
    .then(function(j){
      if (!j) return;
      var exe = pickProgram(j.assets);
      if (exe) a.href = exe.browser_download_url;
      a.textContent = j.tag_name ? L('Download ', '') + j.tag_name + L('', ' herunterladen')
                                 : L('Download the latest release', 'Neueste Version herunterladen');
    })
    .catch(function(){});
})();
