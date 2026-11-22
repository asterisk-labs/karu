/* How Karu reads. Tabs switch the panel of each figure, and the range explorer
   redraws from its controls. */
(function () {
  'use strict';

  var NS = 'http://www.w3.org/2000/svg';
  var KiB = 1024;
  var MiB = 1024 * 1024;

  function el(name, attrs, text) {
    var node = document.createElementNS(NS, name);
    Object.keys(attrs).forEach(function (key) { node.setAttribute(key, attrs[key]); });
    if (text != null) node.textContent = text;
    return node;
  }

  function group(n) {
    return String(n).replace(/\B(?=(\d{3})+(?!\d))/g, ',');
  }

  function size(bytes) {
    if (bytes >= MiB) return (Math.round(bytes / MiB * 100) / 100) + ' MiB';
    if (bytes >= KiB) return (Math.round(bytes / KiB * 10) / 10) + ' KiB';
    return bytes + ' B';
  }

  // Tabs: one selected button shows its panel; arrow keys move along the list.
  document.querySelectorAll('.rk [role="tablist"]').forEach(function (list) {
    var tabs = Array.prototype.slice.call(list.querySelectorAll('[role="tab"]'));
    function select(tab, focus) {
      tabs.forEach(function (other) {
        var on = other === tab;
        other.setAttribute('aria-selected', String(on));
        other.tabIndex = on ? 0 : -1;
        document.getElementById(other.getAttribute('aria-controls')).hidden = !on;
      });
      if (focus) tab.focus();
    }
    tabs.forEach(function (tab, index) {
      tab.addEventListener('click', function () { select(tab, false); });
      tab.addEventListener('keydown', function (event) {
        var step = { ArrowRight: 1, ArrowDown: 1, ArrowLeft: -1, ArrowUp: -1 }[event.key];
        if (!step) return;
        event.preventDefault();
        select(tabs[(index + step + tabs.length) % tabs.length], true);
      });
    });
    select(tabs.filter(function (t) { return t.getAttribute('aria-selected') === 'true'; })[0] || tabs[0], false);
  });

  // Range explorer: a 64 KiB object, optionally seen through a 32 KiB window.
  var explorer = document.querySelector('[data-rk="range"]');
  if (explorer) {
    var OBJECT = 64 * KiB;
    var WIN_START = 16 * KiB;
    var WIN_LENGTH = 32 * KiB;
    var X0 = 10;
    var W = 700;
    var offsetInput = explorer.querySelector('[name="offset"]');
    var lengthInput = explorer.querySelector('[name="length"]');
    var windowInput = explorer.querySelector('[name="window"]');
    var svg = explorer.querySelector('svg');
    var outputs = {};
    explorer.querySelectorAll('[data-out]').forEach(function (node) { outputs[node.dataset.out] = node; });

    function x(byte) { return X0 + byte / OBJECT * W; }

    function drawRange() {
      var windowed = windowInput.checked;
      var limit = windowed ? WIN_LENGTH : OBJECT;
      offsetInput.max = limit - 1;
      var offset = Math.min(Number(offsetInput.value), limit - 1);
      var length = Number(lengthInput.value);
      var base = windowed ? WIN_START : 0;
      var first = base + offset;
      var end = first + length;
      var outside = windowed ? offset + length > WIN_LENGTH : false;
      var pastEnd = !windowed && end > OBJECT;

      explorer.querySelector('[data-show="offset"]').textContent = group(offset);
      explorer.querySelector('[data-show="length"]').textContent = group(length);

      while (svg.firstChild) svg.removeChild(svg.firstChild);
      svg.appendChild(el('rect', { class: 'track', x: X0, y: 26, width: W, height: 30 }));
      if (windowed) {
        svg.appendChild(el('rect', { class: 'window', x: x(WIN_START), y: 20, width: WIN_LENGTH / OBJECT * W, height: 42 }));
        svg.appendChild(el('text', { x: x(WIN_START), y: 14 }, 'window, bytes 16,384 to 49,151'));
      }
      var shown = Math.min(end, OBJECT) - first;
      svg.appendChild(el('rect', {
        class: 'pick' + (outside || pastEnd ? ' is-bad' : ''),
        x: x(first), y: 26, width: Math.max(2, shown / OBJECT * W), height: 30
      }));
      svg.appendChild(el('line', { class: 'tick', x1: X0, y1: 62, x2: X0, y2: 68 }));
      svg.appendChild(el('line', { class: 'tick', x1: X0 + W, y1: 62, x2: X0 + W, y2: 68 }));
      svg.appendChild(el('text', { x: X0, y: 82 }, '0'));
      svg.appendChild(el('text', { x: X0 + W, y: 82, 'text-anchor': 'end' }, '65,535, the last byte'));

      var last = end - 1;
      outputs.interval.textContent = '[' + group(offset) + ', ' + group(offset + length) + ')';
      outputs.absolute.textContent = windowed
        ? '[' + group(first) + ', ' + group(end) + ')'
        : 'the same, no window';
      var verdict = outputs.verdict.parentNode;
      verdict.classList.toggle('is-bad', outside || pastEnd);
      if (outside) {
        outputs.header.textContent = 'none';
        outputs.verdict.textContent = 'KARU_ERR_RANGE before any request, because the read leaves the window';
      } else if (pastEnd) {
        outputs.header.textContent = 'Range: bytes=' + first + '-' + last;
        outputs.verdict.textContent = 'got ' + group(OBJECT - first) + ' bytes, then KARU_ERR_RANGE for the rest';
      } else {
        outputs.header.textContent = 'Range: bytes=' + first + '-' + last;
        outputs.verdict.textContent = '206 with Content-Range: bytes ' + first + '-' + last + '/' + OBJECT;
      }
    }

    [offsetInput, lengthInput, windowInput].forEach(function (input) {
      input.addEventListener('input', drawRange);
    });
    drawRange();
  }
})();
