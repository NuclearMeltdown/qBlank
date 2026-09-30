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
// page reads it before painting (see the inline script there).
(function(){
  var link = document.getElementById('langSwitch');
  if (!link) return;
  link.addEventListener('click', function(){
    try { localStorage.setItem('qblank-lang', link.getAttribute('hreflang')); } catch (e) {}
  });
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
