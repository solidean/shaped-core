// Shared renderer for the nexus browser test runners. Reads window.NEXUS_WEB_MODULES — a list of
// { label, factory, js } entries written into the page by CMake — loads each module, and runs its tests
// one per animation frame, appending to a live, grouped results table with per-library subtotals and a
// grand total. The same driver powers both the per-library pages (one module) and the aggregate page
// (all of them), so they look and behave identically.

(function () {
  'use strict';

  var MODULES = window.NEXUS_WEB_MODULES || [];
  var el = function (id) { return document.getElementById(id); };
  var rows = el('rows');

  // Grand totals across every module.
  var total = 0, done = 0, passed = 0, failed = 0, checks = 0, ms = 0;

  function setSummary() {
    el('s-progress').textContent = done + ' / ' + total;
    el('s-passed').textContent = passed;
    el('s-failed').textContent = failed;
    el('s-checks').textContent = checks;
    el('s-time').textContent = ms.toFixed(1) + ' ms';
    el('bar').style.width = (total ? (done / total * 100) : 0) + '%';
  }

  function groupHeader(label, n) {
    var tr = document.createElement('tr');
    tr.className = 'group';
    var td = document.createElement('td');
    td.colSpan = 5;
    var strong = document.createElement('span');
    strong.textContent = label + ' — ' + n + ' tests';
    var sub = document.createElement('span');
    sub.className = 'sub';
    td.appendChild(strong);
    td.appendChild(sub);
    tr.appendChild(td);
    rows.appendChild(tr);
    return sub; // updated live with the per-library tally
  }

  function appendRow(num, name, ok, c, d, report) {
    var tr = document.createElement('tr');
    tr.className = ok ? 'pass' : 'fail';
    tr.innerHTML =
      '<td class="num">' + num + '</td>' +
      '<td class="name"></td>' +
      '<td class="status">' + (ok ? 'PASS' : 'FAIL') + '</td>' +
      '<td class="num">' + c + '</td>' +
      '<td class="num">' + d.toFixed(3) + ' ms</td>';
    tr.children[1].textContent = name; // textContent: test names are not HTML
    rows.appendChild(tr);
    if (report) {
      var r = document.createElement('tr');
      r.className = 'fail';
      var td = document.createElement('td');
      td.colSpan = 5;
      td.className = 'report';
      td.textContent = report;
      r.appendChild(td);
      rows.appendChild(r);
    }
    tr.scrollIntoView({ block: 'nearest' });
  }

  function errorRow(text) {
    var tr = document.createElement('tr');
    tr.className = 'fail';
    var td = document.createElement('td');
    td.colSpan = 5;
    td.className = 'report';
    td.textContent = text;
    tr.appendChild(td);
    rows.appendChild(tr);
  }

  // Inject the module's loader script, then resolve with its instantiated Module.
  function loadModule(m) {
    return new Promise(function (resolve, reject) {
      var s = document.createElement('script');
      s.src = m.js;
      s.onload = function () {
        var factory = window[m.factory];
        if (typeof factory !== 'function') { reject(new Error('factory ' + m.factory + ' not found')); return; }
        factory({ print: function (t) { console.log(t); }, printErr: function (t) { console.warn(t); } })
          .then(resolve, reject);
      };
      s.onerror = function () { reject(new Error('failed to load ' + m.js)); };
      document.head.appendChild(s);
    });
  }

  // Run all of one module's tests, one per frame, updating the table and totals as it goes.
  function runModule(M, label, sub) {
    return new Promise(function (resolve) {
      var name = M.cwrap('nx_web_test_name', 'string', ['number']);
      var run = M.cwrap('nx_web_run_test', 'number', ['number']);
      var lastChecks = M.cwrap('nx_web_last_checks', 'number', []);
      var lastMs = M.cwrap('nx_web_last_duration_ms', 'number', []);
      var lastReport = M.cwrap('nx_web_last_report', 'string', []);
      var n = M.ccall('nx_web_test_count', 'number', [], []);

      var i = 0, lp = 0, lf = 0, lc = 0, lms = 0;
      function step() {
        if (i >= n) {
          sub.textContent = '  (' + lp + ' passed, ' + lf + ' failed, ' + lc + ' checks, ' + lms.toFixed(1) + ' ms)';
          resolve();
          return;
        }
        var tname = name(i);
        var ok = run(i) === 1;
        var c = lastChecks();
        var d = lastMs();
        var report = ok ? '' : lastReport();

        lp += ok ? 1 : 0; lf += ok ? 0 : 1; lc += c; lms += d;
        passed += ok ? 1 : 0; failed += ok ? 0 : 1; checks += c; ms += d; done += 1;

        appendRow(done, label + ' :: ' + tname, ok, c, d, report);
        sub.textContent = '  (' + lp + ' passed, ' + lf + ' failed)';
        setSummary();

        i += 1;
        requestAnimationFrame(step);
      }
      requestAnimationFrame(step);
    });
  }

  function main() {
    if (!MODULES.length) { el('subtitle').textContent = 'no test modules configured'; return; }
    el('subtitle').textContent = 'loading ' + MODULES.length + ' module(s)…';

    // Phase 1: instantiate every module so the grand total is known before the run starts.
    var loaded = [];
    var chain = Promise.resolve();
    MODULES.forEach(function (m) {
      chain = chain.then(function () {
        return loadModule(m).then(function (M) {
          var count = M.ccall('nx_web_test_count', 'number', [], []);
          loaded.push({ m: m, M: M, count: count });
          total += count;
          setSummary();
        }, function (e) {
          console.error(e);
          errorRow(m.label + ': failed to load — ' + e.message);
        });
      });
    });

    // Phase 2: run each loaded module's tests, grouped under a header. Phase 3: final summary.
    chain.then(function () {
      el('subtitle').textContent = total + ' tests across ' + loaded.length + ' module(s) — one per animation frame';
      var seq = Promise.resolve();
      loaded.forEach(function (L) {
        seq = seq.then(function () {
          var sub = groupHeader(L.m.label, L.count);
          return runModule(L.M, L.m.label, sub);
        });
      });
      return seq;
    }).then(function () {
      var ok = failed === 0;
      el('subtitle').textContent =
        (ok ? '✓ all ' + passed + ' tests passed' : '✗ ' + failed + ' of ' + total + ' tests failed') +
        ' — ' + checks + ' checks in ' + ms.toFixed(1) + ' ms (test time)';
      document.title = (ok ? '✓' : '✗') + ' ' + (window.NEXUS_WEB_TITLE || 'nexus') + ' — ' + passed + '/' + total;
    });
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', main);
  else main();
})();
