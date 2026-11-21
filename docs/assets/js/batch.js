// One client.submit() of four reads, A to D, seen across your code, the Karu
// engine and storage. The planner joins the reads into requests with the rule
// of src/runtime/planner.cpp and its default limits: a remote object reads
// through gaps of up to 1 MiB, a local file joins only ranges that touch. The
// transport runs every request at once, the bytes come back up, scatter puts
// each read's bytes in its buffer and drops the gap, and each read completes
// when its request ends, in the order batch.next() returns them.

(function (karu) {
  'use strict';

  var NS = 'http://www.w3.org/2000/svg';
  var MB = 1e6;

  var SIZE = 8 * MB;
  var READS = [
    { tag: 'A', offset: 1.0 * MB, length: 0.8 * MB, tone: '#492ae8', dark: true },
    { tag: 'B', offset: 1.8 * MB, length: 0.6 * MB, tone: '#f05b7f' },
    { tag: 'C', offset: 2.9 * MB, length: 0.6 * MB, tone: '#f7cc09' },
    { tag: 'D', offset: 5.5 * MB, length: 1.0 * MB, tone: '#c4ffc2' }
  ];
  var GAP_TONE = '#b8b8bf';

  // planner defaults, see CONFIGURATION.md
  var GAP = 1048576;
  var LIMIT = 67108864;
  var PARTS = 1024;
  var AMPLIFICATION = 16;

  var W = 560;
  var H = 426;
  var REGIONS = [
    { name: 'YOUR CODE', y: 0, h: 160 },
    { name: 'KARU', y: 160, h: 122, tint: true },
    { name: 'STORAGE', y: 282, h: 144 }
  ];

  // your code: the reads on the left, the buffers on the right
  var ROW_Y = 74;
  var ROW_STEP = 19;
  var BUF_X = 289;
  var BUF_Y = 50;
  var BUF_W = 56;
  var BUF_H = 50;
  var BUF_STEP = 65;
  var BUS_UP = 120;

  // the engine: four stages in a row
  var BOX_Y = 186;
  var BOX_H = 66;
  var BOX_W = 118;
  var BOXES = ['planner', 'transport', 'scatter', 'completions'];
  var BOX_X = [20, 154, 288, 422];

  // storage: the object and the requests that reach it
  var BAR_X0 = 20;
  var BAR_X1 = 540;
  var BAR_Y = 352;
  var BAR_H = 36;
  var BUS_DOWN = 318;
  var PX = (BAR_X1 - BAR_X0) / SIZE;

  // ms
  var SHOW = 150;
  var SHOW_STEP = 170;
  var PLAN = 1050;
  var SEND = 2050;
  var REACH = 420;     // a request on its way down
  var TRAVEL = 1000;   // a packet on its way up, from the object to a buffer
  var SPAWN = 120;
  var HOLD = 3000;
  var FADE = 450;
  var MODES = {
    remote: { label: 'S3 object', verb: 'GET', how: 'HTTP, all at once', name: 's3://bucket/data.bin', first: 450, rate: 1.1 * MB / 1000 },
    local: { label: 'Local file', verb: 'pread', how: 'pread, all at once', name: '/data/data.bin', first: 60, rate: 1.6 * MB / 1000 }
  };

  function el(tag, attrs, parent) {
    var node = document.createElementNS(NS, tag);
    for (var key in attrs) node.setAttribute(key, attrs[key]);
    if (parent) parent.appendChild(node);
    return node;
  }

  function text(parent, x, y, value, cls, anchor) {
    var node = el('text', { x: x, y: y, 'class': cls }, parent);
    if (anchor) node.setAttribute('text-anchor', anchor);
    node.textContent = value;
    return node;
  }

  function clamp(v) {
    return v < 0 ? 0 : v > 1 ? 1 : v;
  }

  function mb(bytes) {
    return (bytes / MB).toFixed(1) + ' MB';
  }

  function xOf(offset) {
    return BAR_X0 + offset * PX;
  }

  function center(name) {
    var i = BOXES.indexOf(name);
    return BOX_X[i] + BOX_W / 2;
  }

  function plan(reads, remote) {
    var gap = remote ? GAP : 0;  // a local pread gains nothing from reading through gaps
    var sorted = reads.slice().sort(function (a, b) { return a.offset - b.offset; });
    var requests = [];
    for (var i = 0; i < sorted.length;) {
      var head = sorted[i];
      var r = { offset: head.offset, last: head.offset + head.length - 1, useful: head.length, parts: [head] };
      var j = i + 1;
      while (j < sorted.length) {
        var next = sorted[j];
        if (next.offset > r.last && next.offset - r.last - 1 > gap) break;
        var last = Math.max(r.last, next.offset + next.length - 1);
        var length = last - r.offset + 1;
        if (r.parts.length === PARTS || length > LIMIT || length > (r.useful + next.length) * AMPLIFICATION) break;
        r.parts.push(next);
        r.last = last;
        r.useful += next.length;
        j++;
      }
      r.length = r.last - r.offset + 1;
      requests.push(r);
      i = j;
    }
    return requests;
  }

  // a point a given distance along a polyline
  function along(points, d) {
    for (var i = 1; i < points.length; i++) {
      var a = points[i - 1];
      var b = points[i];
      var seg = Math.abs(b[0] - a[0]) + Math.abs(b[1] - a[1]);
      if (d <= seg) {
        var f = seg ? d / seg : 0;
        return [a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f];
      }
      d -= seg;
    }
    return points[points.length - 1];
  }

  function length(points) {
    var total = 0;
    for (var i = 1; i < points.length; i++) {
      total += Math.abs(points[i][0] - points[i - 1][0]) + Math.abs(points[i][1] - points[i - 1][1]);
    }
    return total;
  }

  // packets pass through a stage out of sight, and show again on its wires
  function inside(at) {
    if (at[1] < BOX_Y || at[1] > BOX_Y + BOX_H) return false;
    for (var i = 0; i < BOX_X.length; i++) {
      if (at[0] > BOX_X[i] && at[0] < BOX_X[i] + BOX_W) return true;
    }
    return false;
  }

  function line(points) {
    return 'M' + points.map(function (p) { return p[0] + ' ' + p[1]; }).join('L');
  }

  karu.batch = function (figure) {
    var reduce = window.matchMedia('(prefers-reduced-motion: reduce)').matches;

    var call = document.createElement('div');
    call.className = 'batch-call';
    call.innerHTML = 'auto batch = client.<span class="bt-tok-fn">submit</span>(reads);';
    figure.appendChild(call);

    var svg = el('svg', { viewBox: '0 0 ' + W + ' ' + H, role: 'img', 'aria-label': figure.getAttribute('data-label') || '' }, figure);
    var defs = el('defs', {}, svg);
    var hatch = el('pattern', { id: 'bt-hatch', width: 6, height: 6, patternUnits: 'userSpaceOnUse', patternTransform: 'rotate(45)' }, defs);
    el('rect', { width: 6, height: 6, fill: '#fff' }, hatch);
    el('line', { x1: 0, y1: 0, x2: 0, y2: 6, stroke: 'rgba(0,0,0,.35)', 'stroke-width': 2 }, hatch);
    var head = el('marker', { id: 'bt-head', viewBox: '0 0 10 10', refX: 8, refY: 5, markerWidth: 7, markerHeight: 7, orient: 'auto-start-reverse' }, defs);
    el('path', { d: 'M2 1L8 5L2 9', 'class': 'bt-head' }, head);

    REGIONS.forEach(function (region, i) {
      var g = el('g', { 'class': 'bt-region' + (region.tint ? ' is-tint' : '') }, svg);
      el('rect', { 'class': 'bt-band', x: 0, y: region.y, width: W, height: region.h }, g);
      if (i > 0) el('line', { 'class': 'bt-rule', x1: 0, y1: region.y, x2: W, y2: region.y }, g);
      text(g, W - 16, region.y + 17, region.name, 'bt-region-name', 'end');
    });

    var wires = el('g', {}, svg);
    var flow = el('g', {}, svg);

    // your code: the reads it submits
    text(svg, 20, 38, 'reads', 'bt-head-label');
    text(svg, 48, 55, 'offset', 'bt-col');
    text(svg, 108, 55, 'length', 'bt-col');
    var reads = READS.map(function (read, i) {
      var y = ROW_Y + i * ROW_STEP;
      var g = el('g', { 'class': 'bt-row' + (read.dark ? ' is-dark' : ''), style: '--tone:' + read.tone }, svg);
      el('rect', { 'class': 'bt-chip', x: 20, y: y - 8, width: 16, height: 16 }, g);
      text(g, 28, y + 4, read.tag, 'bt-chip-tag', 'middle');
      text(g, 48, y + 4, mb(read.offset), 'bt-cell');
      text(g, 108, y + 4, mb(read.length), 'bt-cell');
      return Object.assign({ row: g, at: SHOW + i * SHOW_STEP, index: i }, read);
    });

    // and the buffers it owns, one per read
    text(svg, BUF_X, 38, 'buffers', 'bt-head-label');
    reads.forEach(function (read, i) {
      var x = BUF_X + i * BUF_STEP;
      var g = el('g', { 'class': 'bt-buffer', style: '--tone:' + read.tone }, svg);
      el('rect', { 'class': 'bt-buffer-box', x: x, y: BUF_Y, width: BUF_W, height: BUF_H }, g);
      read.fill = el('rect', { 'class': 'bt-buffer-fill', x: x, y: BUF_Y + BUF_H - 10, width: 0, height: 10 }, g);
      el('line', { 'class': 'bt-buffer-rule', x1: x, y1: BUF_Y + BUF_H - 10, x2: x + BUF_W, y2: BUF_Y + BUF_H - 10 }, g);
      el('rect', { 'class': 'bt-buffer-edge', x: x, y: BUF_Y, width: BUF_W, height: BUF_H }, g);
      text(g, x + 8, BUF_Y + 18, read.tag, 'bt-buffer-tag');
      text(g, x + 8, BUF_Y + 31, mb(read.length), 'bt-buffer-size');
      read.buffer = g;
      read.bufferX = x + BUF_W / 2;
    });
    text(svg, center('completions'), BUS_UP + 24, 'batch.next()', 'bt-next', 'middle');

    // the engine
    var boxes = {};
    BOXES.forEach(function (name, i) {
      var x = BOX_X[i];
      var g = el('g', { 'class': 'bt-stage' }, svg);
      el('rect', { 'class': 'bt-stage-box', x: x, y: BOX_Y, width: BOX_W, height: BOX_H }, g);
      text(g, x + 10, BOX_Y + 18, name, 'bt-stage-title');
      var main = text(g, x + 10, BOX_Y + 40, '', 'bt-stage-main');
      var detail = text(g, x + 10, BOX_Y + 56, '', 'bt-stage-detail');
      boxes[name] = { node: g, main: main, detail: detail };
      if (i > 0) {
        el('path', { 'class': 'bt-link', d: 'M' + (BOX_X[i - 1] + BOX_W + 1) + ' ' + (BOX_Y + BOX_H / 2) + 'H' + (x - 2), 'marker-end': 'url(#bt-head)' }, wires);
      }
    });
    var chips = reads.map(function (_, i) {
      var g = el('g', { 'class': 'bt-done', transform: 'translate(' + (BOX_X[3] + 10 + i * 19) + ' ' + (BOX_Y + 28) + ')' }, boxes.completions.node);
      var box = el('rect', { width: 15, height: 15 }, g);
      var label = text(g, 7.5, 11.5, '', 'bt-done-tag', 'middle');
      return { node: g, box: box, label: label };
    });
    boxes.completions.detail.textContent = 'in arrival order';

    // the fixed wires: submit into the planner, scatter up to the buffers, completions up to next()
    var submitPath = [[center('planner'), ROW_Y + 3 * ROW_STEP + 16], [center('planner'), BOX_Y - 2]];
    el('path', { 'class': 'bt-link', d: line(submitPath), 'marker-end': 'url(#bt-head)' }, wires);
    var sx = center('scatter');
    var upPath = 'M' + sx + ' ' + BOX_Y + 'V' + BUS_UP + 'M' + reads[0].bufferX + ' ' + BUS_UP + 'H' + reads[3].bufferX;
    el('path', { 'class': 'bt-link', d: upPath }, wires);
    reads.forEach(function (read) {
      el('path', { 'class': 'bt-link', d: 'M' + read.bufferX + ' ' + BUS_UP + 'V' + (BUF_Y + BUF_H + 2), 'marker-end': 'url(#bt-head)' }, wires);
    });
    el('path', { 'class': 'bt-link', d: 'M' + center('completions') + ' ' + BOX_Y + 'V' + (BUS_UP + 32), 'marker-end': 'url(#bt-head)' }, wires);

    // storage: the object, its reads, and what each request reads and drops
    var objectName = text(svg, 20, 302, '', 'bt-uri');
    el('rect', { 'class': 'bt-object', x: BAR_X0, y: BAR_Y, width: BAR_X1 - BAR_X0, height: BAR_H }, svg);
    text(svg, BAR_X0, BAR_Y + BAR_H + 16, '0', 'bt-ruler');
    text(svg, BAR_X1, BAR_Y + BAR_H + 16, '8 MB', 'bt-ruler', 'end');
    reads.forEach(function (read) {
      var g = el('g', { 'class': 'bt-range' + (read.dark ? ' is-dark' : ''), style: '--tone:' + read.tone }, svg);
      el('rect', { x: xOf(read.offset), y: BAR_Y, width: read.length * PX, height: BAR_H }, g);
      text(g, xOf(read.offset + read.length / 2), BAR_Y + BAR_H / 2 + 5, read.tag, 'bt-range-tag', 'middle');
      read.range = g;
    });
    var planLayer = el('g', {}, svg);

    var dots = el('g', {}, svg);
    var control = el('circle', { 'class': 'bt-control', r: 4.5 }, dots);
    var pool = [];
    for (var p = 0; p < 40; p++) pool.push(el('circle', { 'class': 'bt-packet', r: 3.2 }, dots));

    var state;

    function build(name) {
      var mode = MODES[name];
      var requests = plan(reads, name === 'remote');
      planLayer.textContent = '';
      flow.textContent = '';
      objectName.textContent = mode.name;
      boxes.planner.main.textContent = requests.length + ' ' + mode.verb + 's';
      boxes.planner.detail.textContent = 'from ' + reads.length + ' reads';
      boxes.transport.detail.textContent = mode.how;
      boxes.scatter.detail.textContent = 'into your buffers';

      var tx = center('transport');
      var xs = requests.map(function (r) { return xOf(r.offset + r.length / 2); });
      var order = [];
      var bars = requests.map(function (r, k) {
        var x = xOf(r.offset);
        var width = r.length * PX;
        var vx = xs[k];
        var start = SEND + REACH + mode.first;
        var end = start + r.length / mode.rate;

        // what the request covers, and the bytes it reads and drops
        var g = el('g', { 'class': 'bt-span' }, planLayer);
        var cursor = r.offset;
        r.parts.forEach(function (part) {
          if (part.offset > cursor) {
            el('rect', { 'class': 'bt-gap', x: xOf(cursor), y: BAR_Y, width: (part.offset - cursor) * PX, height: BAR_H }, g);
            text(g, xOf((cursor + part.offset) / 2), BAR_Y + BAR_H + 16, 'read and dropped', 'bt-gap-label', 'middle');
          }
          cursor = Math.max(cursor, part.offset + part.length);
        });
        el('rect', { 'class': 'bt-span-edge', x: x - 2, y: BAR_Y - 2, width: width + 4, height: BAR_H + 4 }, g);

        // the request: down from the transport to the bytes it covers
        var down = [[tx, BOX_Y + BOX_H], [tx, BUS_DOWN], [vx, BUS_DOWN], [vx, BAR_Y - 4]];
        var wire = el('path', { 'class': 'bt-request', d: line(down) }, flow);
        var label = text(flow, vx + 6, BAR_Y - 12, mode.verb + ' ' + mb(r.length), 'bt-request-label');
        var reach = el('circle', { 'class': 'bt-control', r: 4 }, dots);

        // a packet's way back up, through scatter into one buffer
        var up = [[vx, BAR_Y], [vx, BUS_DOWN], [tx, BUS_DOWN], [tx, BOX_Y + BOX_H / 2], [sx, BOX_Y + BOX_H / 2], [sx, BUS_UP]];
        var routes = r.parts.map(function (part) {
          return { part: part, points: up.concat([[part.bufferX, BUS_UP], [part.bufferX, BUF_Y + BUF_H]]) };
        });
        var dropped = { part: null, points: up.slice(0, 5) };

        return { r: r, wire: wire, label: label, reach: reach, down: down, routes: routes, dropped: dropped, start: start, end: end };
      });

      bars.slice().sort(function (a, b) { return a.end - b.end; }).forEach(function (bar) {
        bar.r.parts.forEach(function (part) { order.push({ read: part, at: bar.end + TRAVEL }); });
      });
      chips.forEach(function (chip, i) {
        chip.label.textContent = order[i].read.tag;
        chip.box.style.fill = order[i].read.tone;
        chip.node.classList.toggle('is-dark', !!order[i].read.dark);
      });

      var finish = Math.max.apply(null, order.map(function (o) { return o.at; })) + HOLD;
      state = { mode: mode, bars: bars, order: order, finish: finish };
    }

    // which part of a request a byte belongs to, or null for a dropped gap
    function partAt(r, got) {
      var at = r.offset + got;
      for (var i = 0; i < r.parts.length; i++) {
        var part = r.parts[i];
        if (at >= part.offset && at < part.offset + part.length) return part;
      }
      return null;
    }

    function render(t) {
      var fade = 1 - clamp((t - state.finish) / FADE);
      reads.forEach(function (read) {
        var shown = clamp((t - read.at) / 220) * fade;
        read.row.style.opacity = shown;
        read.range.style.opacity = shown;
        read.buffer.style.opacity = (0.3 + 0.7 * clamp((t - read.at) / 220)) * fade;
      });

      // submit: one dot down into the planner
      var submitted = (t - PLAN) / 300;
      var planned = clamp((t - PLAN - 300) / 260) * fade;
      var sent = clamp((t - SEND) / 200) * fade;
      if (submitted > 0 && submitted < 1) {
        var at = along(submitPath, submitted * length(submitPath));
        control.setAttribute('cx', at[0]);
        control.setAttribute('cy', at[1]);
        control.style.opacity = fade;
      } else {
        control.style.opacity = 0;
      }
      planLayer.style.opacity = planned;
      boxes.planner.main.style.opacity = planned;
      boxes.planner.detail.style.opacity = planned;
      boxes.planner.node.classList.toggle('is-lit', t > PLAN + 300 && t < SEND + 200);

      var inFlight = false;
      var scattering = false;
      var moved = 0;
      var landed = 0;
      state.bars.forEach(function (bar) {
        bar.wire.style.opacity = sent;
        bar.label.style.opacity = sent;
        var going = (t - SEND) / REACH;
        if (going > 0 && going < 1) {
          var at = along(bar.down, going * length(bar.down));
          bar.reach.setAttribute('cx', at[0]);
          bar.reach.setAttribute('cy', at[1]);
          bar.reach.style.opacity = fade;
        } else {
          bar.reach.style.opacity = 0;
        }
        var got = Math.min(Math.max(t - bar.start, 0) * state.mode.rate, bar.r.length);
        var arrived = Math.min(Math.max(t - TRAVEL - bar.start, 0) * state.mode.rate, bar.r.length);
        moved += got;
        bar.r.parts.forEach(function (part) {
          var p = clamp((arrived - (part.offset - bar.r.offset)) / part.length);
          part.fill.setAttribute('width', BUF_W * p);
          landed += p * part.length;
        });
        if (t >= SEND && t < bar.end) inFlight = true;
        if (t >= bar.start && t < bar.end + TRAVEL) scattering = true;
        bar.wire.classList.toggle('is-live', t >= SEND && t < bar.end);
      });
      boxes.transport.main.textContent = mb(moved);
      boxes.transport.main.style.opacity = sent;
      boxes.transport.detail.style.opacity = sent;
      boxes.transport.node.classList.toggle('is-lit', inFlight);
      boxes.scatter.main.textContent = mb(landed);
      boxes.scatter.main.style.opacity = sent;
      boxes.scatter.detail.style.opacity = sent;
      boxes.scatter.node.classList.toggle('is-lit', scattering);

      // the packets: one leaves each request every SPAWN ms while it streams
      var used = 0;
      state.bars.forEach(function (bar) {
        var first = Math.ceil(Math.max(t - TRAVEL - bar.start, 0) / SPAWN);
        for (var s = first; ; s++) {
          var born = bar.start + s * SPAWN;
          if (born > t || born > bar.end) break;
          var part = partAt(bar.r, (born - bar.start) * state.mode.rate);
          var route = part ? bar.routes.filter(function (r) { return r.part === part; })[0] : bar.dropped;
          var total = length(route.points);
          var dist = (t - born) / TRAVEL * length(bar.routes[0].points);
          if (dist > total || used >= pool.length) continue;
          var dot = pool[used++];
          var at = along(route.points, dist);
          dot.setAttribute('cx', at[0]);
          dot.setAttribute('cy', at[1]);
          dot.style.fill = part ? part.tone : GAP_TONE;
          dot.style.opacity = inside(at) ? 0 : (part ? 1 : 1 - dist / total) * fade;
        }
      });
      for (var i = used; i < pool.length; i++) pool[i].style.opacity = 0;

      var lit = false;
      state.order.forEach(function (o, i) {
        var done = t >= o.at;
        chips[i].node.style.opacity = (done ? 1 : 0) * fade;
        o.read.buffer.classList.toggle('is-done', done);
        if (done && t < o.at + 260) lit = true;
      });
      boxes.completions.node.classList.toggle('is-lit', lit);
      boxes.completions.detail.style.opacity = sent;
    }

    var name = 'remote';
    var begin = 0;
    var running = false;

    function frame(now) {
      if (!begin) begin = now;
      var t = now - begin;
      if (t > state.finish + FADE + 250) {
        begin = now;
        t = 0;
      }
      render(t);
      requestAnimationFrame(frame);
    }

    function pick(next) {
      name = next;
      build(name);
      buttons.forEach(function (b) {
        b.classList.toggle('is-active', b.dataset.mode === name);
        b.setAttribute('aria-pressed', String(b.dataset.mode === name));
      });
      if (reduce) {
        render(state.finish - HOLD + 1);
        return;
      }
      begin = 0;
      if (!running) {
        running = true;
        requestAnimationFrame(frame);
      }
    }

    var carousel = document.createElement('div');
    carousel.className = 'batch-carousel';
    var buttons = Object.keys(MODES).map(function (key) {
      var button = document.createElement('button');
      button.type = 'button';
      button.className = 'batch-carousel__dot';
      button.dataset.mode = key;
      button.textContent = MODES[key].label;
      button.addEventListener('click', function () { if (name !== key) pick(key); });
      carousel.appendChild(button);
      return button;
    });
    figure.appendChild(carousel);

    pick(name);
  };

  var figure = document.getElementById('batch');
  if (figure) karu.batch(figure);
})((window.karu = window.karu || {}));
