/* Prom の動作 (画面の更新・グラフ・操作)。見た目は style.css、骨組みは index.html にあります。 */
(() => {
    'use strict';

    /* ---------------------------------------------------------------
     * C側(WebView2ホスト)との通信
     *   JS -> C : "init" / "refresh" / "kill <pid>"                 (文字列)
     *   C -> JS : {type:"host"|"update"|"icons"|"kill", ...}         (JSON)
     * ブラウザで直接開いた場合は、ダミーデータのデモモードで動きます。
     * ------------------------------------------------------------- */
    const INTERVALS = [250, 500, 1000, 2000, 5000];   /* 更新間隔(ms)の候補。ヘッダーのボタンで切り替え */

    const host = window.chrome && window.chrome.webview;
    const $ = s => document.querySelector(s);
    const rows = $('#rows'), q = $('#q'), scrim = $('#scrim'), menu = $('#menu');

    const state = {
        procs: [], sort: { key: 'cpu', dir: -1 }, sel: null, filter: '', paused: false, interval: 2000,
        hist: { cpu: [], mem: [], disk: [], gpu: [], rx: [], tx: [] },
        sys: { cpu: 0, memUsed: 0, memTotal: 0, memPct: 0, gpu: 0 }, disk: { active: 0, read: 0, write: 0 },
        net: { rx: 0, tx: 0 }, drives: [],
        open: 'ov', segs: null, hl: -1, dmode: 'cpu', center: { t: '', l: '' }, pending: null, killName: '', winDir: '', gpuOk: false, expanded: new Set(), killBatch: null
    };
    const icons = new Map();       /* exeのパス -> 画像URL ("*" は汎用アイコン) */
    const rowEls = new Map();      /* pid -> 行の要素 */
    const pathCache = new Map();   /* pid -> exeのパス (Cは新しいプロセスのときだけ送ってくる) */

    function post(msg) { if (host) host.postMessage(msg); else mockPost(msg); }
    if (host) host.addEventListener('message', e => onMessage(e.data));

    function onMessage(d) {
        switch (d.type) {
            case 'host':
                document.documentElement.classList.toggle('glass', !!d.glass);
                $('#glass').setAttribute('aria-pressed', d.glass ? 'true' : 'false');
                state.winDir = (d.win || '').toLowerCase();
                break;
            case 'update':
                for (const p of d.procs) {
                    if (p.path !== undefined) pathCache.set(p.pid, p.path);
                    else p.path = pathCache.get(p.pid) || '';
                    p.gpu = p.gpu || 0;
                }
                if (pathCache.size > d.procs.length * 2 + 64) {
                    const live = new Set(d.procs.map(p => p.pid));
                    for (const k of [...pathCache.keys()]) if (!live.has(k)) pathCache.delete(k);
                }
                state.procs = d.procs;
                updateSummary(d);
                render();
                break;
            case 'open':
                if (!d.ok) toast('ファイルの場所を開けませんでした (パスを取得できないプロセスです)', true);
                break;
            case 'icons':
                d.items.forEach(it => addIcon(it.k, it.d));
                render();
                renderBody();
                break;
            case 'kill': {                       /* グループ終了のときは、全部の結果が揃ってから1回だけ知らせる */
                const kb = state.killBatch || { left: 1, ok: 0, ng: 0, err: 0 };
                if (d.count !== undefined) { kb.ok += d.count - d.failed; kb.ng += d.failed; if (d.failed) kb.err = d.error; }   /* ツリー終了は1通で全体の結果が届く */
                else if (d.ok) kb.ok++; else { kb.ng++; kb.err = d.error; }
                if (--kb.left <= 0) {
                    state.killBatch = null;
                    const nm = state.killName || 'プロセス';
                    if (!kb.ng) toast(kb.ok ? nm + ' を終了しました' : '終了するプロセスがありませんでした');
                    else if (!kb.ok) toast('終了できませんでした (エラー ' + kb.err + ')。管理者として実行すると終了できる場合があります。', true);
                    else toast(kb.ok + ' 個を終了、' + kb.ng + ' 個は終了できませんでした。', true);
                    setTimeout(() => post('refresh'), 400);
                }
                break;
            }
        }
    }

    /* ---------------- ユーティリティ (数値は小数第2位まで) ---------------- */
    const fmtPct = v => v.toFixed(2) + '%';
    function fmtBytes(b) {
        if (b >= 1073741824) return (b / 1073741824).toFixed(2) + ' GB';
        if (b >= 1048576) return (b / 1048576).toFixed(2) + ' MB';
        return (b / 1024).toFixed(2) + ' KB';
    }
    function fmtRate(b) {
        if (b >= 1048576) return (b / 1048576).toFixed(2) + ' MB/s';
        return (b / 1024).toFixed(2) + ' KB/s';
    }
    const esc = s => String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
    function hue(s) { let h = 0; for (let i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) % 360; return h; }
    function toast(msg, isError) {
        const t = $('#toast');
        t.textContent = msg;
        t.classList.toggle('error', !!isError);
        t.classList.add('show');
        clearTimeout(toast.t);
        toast.t = setTimeout(() => t.classList.remove('show'), 3600);
    }
    function addIcon(key, b64) {
        if (b64) icons.set(key, 'data:image/png;base64,' + b64);
    }
    const iconUrl = path => (path && icons.get(path)) || icons.get('*') || null;

    /* ---------------- 左のカード: 開くのは1つだけ ---------------- */
    const tiles = ['ov', 'cpu', 'mem', 'disk', 'gpu', 'net'].map(id => {
        const el = document.querySelector('.tile[data-id="' + id + '"]');
        return { id, el, head: el.querySelector('.t-head'), more: el.querySelector('.more'), svg: el.querySelector('.graph') };
    });
    tiles.forEach(t => t.head.addEventListener('click', () => {
        state.open = state.open === t.id ? '' : t.id;   /* 開いているものをもう一度押すと閉じる */
        applyOpen();
    }));

    function pushHist(k, v) { const a = state.hist[k]; a.push(v); if (a.length > 60) a.shift(); }

    /* 線グラフ (開いているカードは円グラフなので描かない = 軽くなる) */
    function graph(svg, list, max, fill) {
        let html = '';
        list.forEach((s, k) => {
            const a = s.a, n = a.length;
            if (n < 2) return;
            const step = 120 / 59, off = 120 - (n - 1) * step;
            const pts = a.map((v, i) => (off + i * step).toFixed(1) + ',' + (39 - Math.min(1, v / max) * 37).toFixed(1));
            const line = 'M' + pts.join('L');
            if (k === 0 && fill) html += '<path class="fill" d="' + line + 'L120,40L' + off.toFixed(1) + ',40Z"/>';
            html += '<path class="line' + (k && !s.c ? ' l2' : '') + '"' + (s.c ? ' style="stroke:' + s.c + '"' : '') + ' d="' + line + '"/>';
        });
        svg.innerHTML = html;
    }
    function drawGraphs() {
        const H = state.hist;
        tiles.forEach(t => {
            if (t.id === state.open) return;
            if (t.id === 'ov') graph(t.svg, [{ a: H.cpu, c: 'var(--c-cpu)' }, { a: H.mem, c: 'var(--c-mem)' }, { a: H.disk, c: 'var(--c-disk)' }], 100, false);
            else if (t.id === 'net') graph(t.svg, [{ a: H.rx }, { a: H.tx }], Math.max(262144, ...H.rx, ...H.tx) * 1.15, true);
            else graph(t.svg, [{ a: H[t.id] }], 100, true);
        });
    }

    function updateSummary(d) {
        const memPct = d.memTotal ? d.memUsed / d.memTotal * 100 : 0;
        state.gpuOk = !!d.gpuOk;
        state.sys = { cpu: d.cpu, memUsed: d.memUsed, memTotal: d.memTotal, memPct, gpu: d.gpuOk ? d.gpu : 0 };
        state.disk = d.disk; state.net = d.net; state.drives = d.drives || [];
        pushHist('cpu', d.cpu); pushHist('mem', memPct); pushHist('disk', d.disk.active);
        pushHist('gpu', state.sys.gpu); pushHist('rx', d.net.rx); pushHist('tx', d.net.tx);

        $('#cpuV').textContent = fmtPct(d.cpu);
        $('#cpuSub').textContent = d.cores + ' 論理プロセッサ';
        $('#memV').textContent = fmtPct(memPct);
        $('#memSub').textContent = fmtBytes(d.memUsed) + ' / ' + fmtBytes(d.memTotal);
        $('#diskV').textContent = fmtPct(d.disk.active);
        $('#diskSub').textContent = '読取 ' + fmtRate(d.disk.read) + '\n書込 ' + fmtRate(d.disk.write);
        $('#netV').textContent = fmtRate(d.net.rx + d.net.tx);
        $('#netSub').textContent = '↓ ' + fmtRate(d.net.rx) + '\n↑ ' + fmtRate(d.net.tx);
        $('#gpuV').textContent = d.gpuOk ? fmtPct(d.gpu) : '–';
        $('#gpuSub').textContent = d.gpuOk ? '' : 'GPU のカウンターがありません';
        $('#ovCpu').textContent = fmtPct(d.cpu);
        $('#ovMem').textContent = fmtPct(memPct);
        $('#ovDisk').textContent = fmtPct(d.disk.active);

        const th = d.procs.reduce((s, p) => s + (p.th || 0), 0);
        $('#foot').textContent = d.procs.length + ' プロセス · ' + th.toLocaleString() + ' スレッド';
        drawGraphs();
        renderBody();
    }

    /* ---------------- 円グラフ (開いたカードの中に表示) ---------------- */
    const PAL = ['#6ea8ff', '#b794f6', '#5fd6a0', '#ffb86b', '#ff8fb8'];
    const OTHER = '#8b93a1', REST = 'rgba(255,255,255,.14)';
    const SVGNS = 'http://www.w3.org/2000/svg';
    const mk = (tag, attrs) => { const e = document.createElementNS(SVGNS, tag); for (const k in attrs) e.setAttribute(k, attrs[k]); return e; };
    const li = html => { const e = document.createElement('li'); e.innerHTML = html; return e; };

    /* (a) 項目別の円グラフ: CPU / メモリ / ディスク / ネットワーク で使い回す */
    const donut = (() => {
        const box = document.createElement('div');
        box.className = 'box';
        box.innerHTML = '<div class="donut"><svg viewBox="0 0 120 120"><circle class="trk" cx="60" cy="60" r="46" stroke-width="13"/>' +
            '<g transform="rotate(-90 60 60)"></g></svg><div class="dc"><div class="big"></div><div class="lbl"></div></div></div><ul class="legend"></ul>';
        const g = box.querySelector('g'), ul = box.querySelector('ul');
        const circles = [], items = [];
        for (let i = 0; i < 7; i++) {
            const c = mk('circle', { cx: 60, cy: 60, r: 46, pathLength: 100, class: 'seg', 'stroke-dasharray': '0 100' });
            g.appendChild(c); circles.push(c);
            const l = li('<i class="dot"></i><span class="lico"></span><span class="ln"></span><span class="lv"></span>');
            ul.appendChild(l);
            items.push({ li: l, dot: l.children[0], ico: l.children[1], ln: l.children[2], lv: l.children[3] });
            const enter = () => hl(i), leave = () => hl(-1);
            const click = () => { const s = state.segs && state.segs[i]; if (s && s.group) toggleFilter(s.name); };
            [c, l].forEach(el => { el.addEventListener('mouseenter', enter); el.addEventListener('mouseleave', leave); el.addEventListener('click', click); });
        }
        return { box, circles, items, big: box.querySelector('.big'), lbl: box.querySelector('.lbl') };
    })();

    /* (b) 全体像: CPU / メモリ / ディスクの使用率を3重のリングで表示 */
    const RINGS = [
        { name: 'CPU', c: 'var(--c-cpu)', r: 52 },
        { name: 'メモリ', c: 'var(--c-mem)', r: 43 },
        { name: 'ディスク', c: 'var(--c-disk)', r: 34 }
    ];
    const rings = (() => {
        const box = document.createElement('div');
        box.className = 'box rings';
        box.innerHTML = '<div class="donut"><svg viewBox="0 0 120 120"><g transform="rotate(-90 60 60)"></g></svg>' +
            '<div class="dc"><div class="big"></div><div class="lbl"></div></div></div><ul class="legend"></ul>';
        const g = box.querySelector('g'), ul = box.querySelector('ul');
        const arcs = [], items = [];
        RINGS.forEach((rg, i) => {
            g.appendChild(mk('circle', { cx: 60, cy: 60, r: rg.r, class: 'trk', 'stroke-width': 7 }));
            const a = mk('circle', { cx: 60, cy: 60, r: rg.r, pathLength: 100, class: 'arc', 'stroke-dasharray': '0 100' });
            a.style.stroke = rg.c;
            g.appendChild(a); arcs.push(a);
            const l = li('<i class="dot"></i><span class="ln"></span><span class="lv"></span>');
            l.classList.add('static');
            l.firstChild.style.background = rg.c;
            l.children[1].textContent = rg.name;
            ul.appendChild(l);
            items.push({ li: l, lv: l.children[2] });
            const enter = () => ringHl(i), leave = () => ringHl(-1);
            [a, l].forEach(el => { el.addEventListener('mouseenter', enter); el.addEventListener('mouseleave', leave); });
        });
        return { box, arcs, items, big: box.querySelector('.big'), lbl: box.querySelector('.lbl'), hl: -1 };
    })();

    function renderRings() {
        const v = [state.sys.cpu, state.sys.memPct, state.disk.active];
        v.forEach((p, i) => {
            const c = Math.max(0, Math.min(100, p));
            rings.arcs[i].setAttribute('stroke-dasharray', c.toFixed(2) + ' ' + (100 - c).toFixed(2));
            rings.items[i].lv.textContent = fmtPct(p);
        });
        ringCenter();
    }
    function ringCenter() {
        const v = [state.sys.cpu, state.sys.memPct, state.disk.active];
        let i = rings.hl, label;
        if (i >= 0) label = RINGS[i].name;
        else { i = v.indexOf(Math.max(...v)); label = RINGS[i].name + ' が最大'; }
        rings.big.textContent = fmtPct(v[i]);
        rings.lbl.textContent = label;
    }
    function ringHl(i) {
        rings.hl = i;
        rings.arcs.forEach((a, k) => a.classList.toggle('hl', k === i));
        rings.items.forEach((l, k) => l.li.classList.toggle('hl', k === i));
        ringCenter();
    }

    function groups(key) {
        const m = new Map();
        for (const p of state.procs) {
            const v = key === 'cpu' ? Math.max(0, p.cpu) : key === 'gpu' ? p.gpu : p.mem;
            if (v <= 0) continue;
            let g = m.get(p.name);
            if (!g) { g = { name: p.name, v: 0, n: 0, path: '' }; m.set(p.name, g); }
            g.v += v; g.n++;
            if (!g.path && p.path) g.path = p.path;
        }
        return [...m.values()].sort((a, b) => b.v - a.v);
    }
    const pad = (segs, n) => { while (segs.length < n) segs.push({ name: '', v: 0, empty: true, color: PAL[segs.length] || OTHER }); };

    function computeSegs(mode) {
        const segs = [];
        let total = 0, ct = '', cl = '';
        if (mode === 'cpu' || mode === 'mem' || mode === 'gpu') {
            const isPct = mode !== 'mem';             /* CPU と GPU は割合(%)、メモリは容量 */
            total = isPct ? 100 : state.sys.memTotal;
            const top = groups(mode).slice(0, 5);
            const topSum = top.reduce((s, g) => s + g.v, 0);
            const used = Math.max(isPct ? state.sys[mode] : state.sys.memUsed, topSum);
            top.forEach((g, i) => segs.push({ name: g.name, n: g.n, v: g.v, path: g.path, group: true, color: PAL[i] }));
            pad(segs, 5);
            segs.push({ name: 'その他', v: used - topSum, color: OTHER });
            segs.push({ name: isPct ? 'アイドル' : '空き', v: Math.max(0, total - used), color: REST });
            segs.forEach(s => { s.text = isPct ? fmtPct(Math.max(0, s.v)) : fmtBytes(Math.max(0, s.v)); });
            ct = fmtPct(isPct ? state.sys[mode] : state.sys.memPct);
            cl = mode === 'cpu' ? 'CPU 使用率' : mode === 'gpu' ? 'GPU 使用率' : 'メモリ使用率';
        } else if (mode === 'disk') {            /* ドライブごとの使用容量 */
            const dr = state.drives;
            total = dr.reduce((s, d) => s + d.t, 0);
            const free = dr.reduce((s, d) => s + d.f, 0);
            dr.slice(0, 5).forEach((d, i) => segs.push({ name: d.n, v: d.t - d.f, color: PAL[i], text: fmtBytes(d.t - d.f) + ' / ' + fmtBytes(d.t) }));
            pad(segs, 5);
            const rest = dr.slice(5).reduce((s, d) => s + d.t - d.f, 0);
            segs.push({ name: 'その他', v: rest, color: OTHER, empty: dr.length <= 5, text: fmtBytes(rest) });
            segs.push({ name: '空き', v: free, color: REST, text: fmtBytes(free) });
            ct = total ? fmtPct((total - free) / total * 100) : '–';
            cl = '容量の使用率';
        } else {                                 /* ネットワーク: 受信と送信の割合 */
            const { rx, tx } = state.net;
            total = rx + tx;
            segs.push({ name: '受信', v: rx, color: 'var(--c-net)', text: fmtRate(rx) });
            segs.push({ name: '送信', v: tx, color: 'var(--c-net2)', text: fmtRate(tx) });
            pad(segs, 7);
            ct = fmtRate(total);
            cl = '受信 + 送信';
        }
        segs.forEach(s => { s.f = total > 0 ? Math.max(0, s.v) / total : 0; });
        return { segs, ct, cl };
    }

    function renderDonut(mode) {
        const { segs, ct, cl } = computeSegs(mode);
        state.segs = segs; state.dmode = mode; state.center = { t: ct, l: cl };
        const filterable = mode === 'cpu' || mode === 'mem' || mode === 'gpu';
        let cum = 0;
        segs.forEach((s, i) => {
            const gap = s.f > 0.01 ? 0.7 : 0;
            const len = Math.max(0, s.f * 100 - gap);
            const c = donut.circles[i];
            c.setAttribute('stroke-dasharray', len.toFixed(2) + ' ' + (100 - len).toFixed(2));
            c.setAttribute('stroke-dashoffset', (-cum * 100).toFixed(2));
            c.style.stroke = s.color;
            cum += s.f;

            const L = donut.items[i];
            L.li.style.display = s.empty ? 'none' : '';
            L.li.classList.toggle('static', !s.group);
            L.dot.style.background = s.color;
            const url = s.group ? iconUrl(s.path) : null;
            L.ico.innerHTML = url ? '<img alt="" src="' + url + '">' : '';
            L.ico.style.display = filterable ? '' : 'none';
            L.ln.textContent = s.name + (s.n > 1 ? ' ×' + s.n : '');
            L.lv.textContent = s.text;
        });
        donutCenter();
    }
    function donutCenter() {
        const s = state.segs && state.segs[state.hl];
        if (s && !s.empty) { donut.big.textContent = s.text; donut.lbl.textContent = s.name; }
        else { donut.big.textContent = state.center.t; donut.lbl.textContent = state.center.l; }
    }
    function hl(i) {
        state.hl = i;
        donut.circles.forEach((c, k) => c.classList.toggle('hl', k === i));
        donut.items.forEach((l, k) => l.li.classList.toggle('hl', k === i));
        donutCenter();
    }
    function toggleFilter(name) {
        state.filter = state.filter === name ? '' : name;
        q.value = state.filter;
        render();
    }

    function renderBody() {
        const o = state.open;
        if (o === 'ov') renderRings();
        else if (o) renderDonut(o);
    }
    function applyOpen() {
        tiles.forEach(t => {
            const open = t.id === state.open;
            t.el.classList.toggle('open', open);
            t.head.setAttribute('aria-expanded', open);
            if (open) {
                const box = t.id === 'ov' ? rings.box : donut.box;
                if (box.parentNode !== t.more) t.more.appendChild(box);   /* 1つの円グラフ部品を、開いたカードへ移す */
            }
        });
        state.hl = -1;
        drawGraphs();
        renderBody();
    }

    /* ---------------- プロセス一覧 ----------------
     * アプリ / バックグラウンド プロセス / Windows プロセス の3つに分け、
     * 同じ名前のものは「chrome.exe (12)」のように1行にまとめます (クリックで展開)。
     * 行は差分更新: 同じ行の要素を使い回して、変わった所だけ書き換えます。 */
    const CATS = [['app', 'アプリ'], ['bg', 'バックグラウンド プロセス'], ['win', 'Windows プロセス']];
    let lastItems = [];

    function isWindowsProc(p) {
        if (p.pid === 0 || p.pid === 4) return true;
        if (!p.path) return true;                 /* パスが取れない = 保護されたシステムのプロセス */
        return !!state.winDir && p.path.toLowerCase().startsWith(state.winDir + '\\');
    }
    function groupCat(procs) {
        if (procs.some(p => p.app)) return 'app';           /* ウィンドウを持つ人が1つでもいれば「アプリ」 */
        return procs.every(isWindowsProc) ? 'win' : 'bg';
    }

    function makeSec() {
        const tr = document.createElement('tr');
        tr.className = 'sec';
        tr.innerHTML = '<td colspan="6"><span class="st"></span><span class="sc"></span></td>';
        return { sec: true, tr, st: tr.querySelector('.st'), sc: tr.querySelector('.sc'), last: {} };
    }
    function makeRow() {
        const tr = document.createElement('tr');
        tr.innerHTML = '<td><div class="name"><span class="chev"></span><span class="ico"></span><span class="nm"></span></div></td>' +
            '<td class="num"></td><td><div class="cpuw"><span class="v"></span><span class="bar"><i></i></span></div></td>' +
            '<td class="num"></td><td class="num"></td><td class="path"></td>';
        const t = tr.children;
        return {
            tr, ico: t[0].querySelector('.ico'), nm: t[0].querySelector('.nm'), pid: t[1], cpuCell: t[2],
            v: t[2].querySelector('.v'), bar: t[2].querySelector('.bar i'), gpu: t[3], mem: t[4], path: t[5], last: {}
        };
    }
    function setText(el, r, key, val) { if (r.last[key] !== val) { r.last[key] = val; el.textContent = val; } }

    function updateRow(r, it) {
        const p = it.p;
        r.tr.dataset.key = it.key;
        const cls = (it.child ? 'child ' : '') + (it.group ? 'grp ' : '') + (it.open ? 'open ' : '') + (it.key === state.sel ? 'sel' : '');
        if (r.last.cls !== cls) { r.last.cls = cls; r.tr.className = cls; }
        setText(r.nm, r, 'nm', p.name + (it.n > 1 ? ' (' + it.n + ')' : ''));

        const url = iconUrl(p.path), ini = (p.name.trim().charAt(0) || '?').toUpperCase();
        const ik = url || ('L' + ini);
        if (r.last.ik !== ik) {
            r.last.ik = ik;
            r.ico.classList.toggle('letter', !url);
            r.ico.style.setProperty('--h', hue(p.name));
            r.ico.innerHTML = url ? '<img alt="" draggable="false" src="' + url + '">' : '<b>' + esc(ini) + '</b>';
        }

        setText(r.pid, r, 'pid', it.group ? '—' : String(p.pid));
        const cpu = p.cpu >= 0 ? p.cpu : null;
        setText(r.v, r, 'v', cpu == null ? '—' : fmtPct(cpu));
        const w = cpu == null ? 0 : Math.min(100, Math.round(cpu));
        if (r.last.w !== w) { r.last.w = w; r.bar.style.width = w + '%'; }
        const lvl = cpu == null ? '' : cpu >= 50 ? 'hot' : cpu >= 15 ? 'warm' : '';
        if (r.last.lvl !== lvl) { r.last.lvl = lvl; r.cpuCell.className = lvl; }
        setText(r.gpu, r, 'gpu', state.gpuOk ? fmtPct(p.gpu || 0) : '—');
        setText(r.mem, r, 'mem', p.mem > 0 ? fmtBytes(p.mem) : '—');
        if (r.last.path !== p.path) { r.last.path = p.path; r.path.textContent = p.path || ''; r.path.title = p.path || ''; }
    }

    function stateText() { return state.paused ? '一時停止中' : (state.interval / 1000) + '秒ごとに自動更新'; }

    function render() {
        const f = state.filter.toLowerCase();
        const list = state.procs.filter(p =>
            !f || p.name.toLowerCase().includes(f) || String(p.pid).includes(f) || (p.path || '').toLowerCase().includes(f));

        const { key, dir } = state.sort;
        const by = (a, b) => {
            let x = a[key], y = b[key];
            if (key === 'name' || key === 'path') { x = (x || '').toLowerCase(); y = (y || '').toLowerCase(); return x < y ? -dir : x > y ? dir : 0; }
            return (x - y) * dir;
        };

        /* 同じ名前をまとめる (CPU・メモリは合計、PIDは最小のもの) */
        const gm = new Map();
        for (const p of list) {
            const k = p.name.toLowerCase();
            let g = gm.get(k);
            if (!g) { g = { key: 'g:' + k, name: p.name, procs: [], cpu: -1, gpu: 0, mem: 0, pid: Infinity, path: '' }; gm.set(k, g); }
            g.procs.push(p);
            if (p.cpu >= 0) g.cpu = Math.max(g.cpu, 0) + p.cpu;
            g.mem += p.mem;
            g.gpu += p.gpu || 0;
            if (p.pid < g.pid) g.pid = p.pid;
            if (!g.path && p.path) g.path = p.path;
        }
        const buckets = { app: [], bg: [], win: [] };
        for (const g of gm.values()) buckets[groupCat(g.procs)].push(g);

        const items = [];
        for (const [cat, label] of CATS) {
            const gs = buckets[cat];
            if (!gs.length) continue;
            gs.sort(by);
            items.push({ sec: true, key: 'h:' + cat, label, count: gs.length });
            for (const g of gs) {
                if (g.procs.length === 1) { const p = g.procs[0]; items.push({ key: 'p:' + p.pid, p, n: 1 }); continue; }
                const open = state.expanded.has(g.key);
                items.push({ key: g.key, p: g, n: g.procs.length, group: true, open, procs: g.procs });
                if (open) g.procs.slice().sort(by).forEach(p => items.push({ key: 'p:' + p.pid, p, n: 1, child: true }));
            }
        }
        lastItems = items;

        const live = new Set(), order = [];
        for (const it of items) {
            let r = rowEls.get(it.key);
            if (!r) { r = it.sec ? makeSec() : makeRow(); rowEls.set(it.key, r); }
            if (it.sec) { setText(r.st, r, 'st', it.label); setText(r.sc, r, 'sc', String(it.count)); }
            else updateRow(r, it);
            order.push(r);
            live.add(it.key);
        }
        for (const k of [...rowEls.keys()]) if (!live.has(k)) rowEls.delete(k);

        /* 並び順を合わせ、不要になった行を取り除く */
        let node = rows.firstElementChild;
        for (const r of order) {
            if (node === r.tr) node = node.nextElementSibling;
            else rows.insertBefore(r.tr, node);
        }
        while (node) { const nx = node.nextElementSibling; rows.removeChild(node); node = nx; }

        document.querySelectorAll('thead th').forEach(th => {
            const on = th.dataset.k === key;
            th.classList.toggle('sorted', on);
            th.querySelector('.arrow').textContent = on ? (dir > 0 ? '↑' : '↓') : '';
        });
        $('#count').textContent = list.length === state.procs.length ? state.procs.length + ' 件' : list.length + ' / ' + state.procs.length + ' 件';
        $('#state').textContent = stateText();
        syncButtons();
    }

    /* ---------------- 選択 / 操作 ---------------- */
    /* 選択中の行 -> 終了の対象 (グループ行ならそのグループ全部) */
    function selTarget() {
        const it = lastItems.find(i => i.key === state.sel);
        if (!it || it.sec) return null;
        if (!it.group) return { name: it.p.name, pids: [it.p.pid], openPid: it.p.pid, group: false };
        const withPath = it.procs.find(p => p.path);
        return { name: it.p.name, pids: it.procs.map(p => p.pid), openPid: (withPath || it.procs[0]).pid, group: true };
    }
    function syncButtons() { $('#kill').disabled = !selTarget(); }
    function select(key) { state.sel = key; render(); }
    function toggleGroup(key) {
        if (state.expanded.has(key)) state.expanded.delete(key); else state.expanded.add(key);
        render();
    }

    rows.addEventListener('click', e => {
        const tr = e.target.closest('tr');
        if (!tr || tr.classList.contains('sec')) return;
        if (tr.classList.contains('grp') && e.target.closest('.chev')) { toggleGroup(tr.dataset.key); return; }
        select(tr.dataset.key);
    });
    rows.addEventListener('dblclick', e => {
        const tr = e.target.closest('tr');
        if (tr && tr.classList.contains('grp')) toggleGroup(tr.dataset.key);
    });
    rows.addEventListener('contextmenu', e => {
        const tr = e.target.closest('tr');
        if (!tr || tr.classList.contains('sec')) return;
        select(tr.dataset.key);
        openMenu(e.clientX, e.clientY);
    });
    document.addEventListener('contextmenu', e => e.preventDefault());
    document.addEventListener('click', e => { if (!menu.contains(e.target)) closeMenu(); });

    function openMenu(x, y) {
        menu.classList.add('open');
        const r = menu.getBoundingClientRect();
        menu.style.left = Math.max(8, Math.min(x, innerWidth - r.width - 8)) + 'px';
        menu.style.top = Math.max(8, Math.min(y, innerHeight - r.height - 8)) + 'px';
    }
    function closeMenu() { menu.classList.remove('open'); }

    /* 子孫プロセスの数 (確認ダイアログに表示するため) */
    function descendantCount(roots) {
        const kids = new Map();
        for (const p of state.procs) {
            if (p.pid === 0) continue;
            let a = kids.get(p.pp);
            if (!a) kids.set(p.pp, a = []);
            a.push(p.pid);
        }
        const seen = new Set(roots), q = [...roots];
        let n = 0;
        while (q.length) for (const c of kids.get(q.pop()) || []) if (!seen.has(c)) { seen.add(c); q.push(c); n++; }
        return n;
    }

    function askKill(tree) {
        const t = selTarget();
        if (!t) return;
        closeMenu();
        t.tree = !!tree;
        state.pending = t;
        const n = t.pids.length;
        if (t.tree) {
            const kids = descendantCount(t.pids);
            $('#dlgTitle').textContent = '「' + t.name + '」とその子プロセスをすべて終了しますか?';
            $('#dlgBody').textContent = '本体 ' + n + ' 個と、その子プロセス ' + kids + ' 個 (合計 ' + (n + kids) + ' 個) を強制終了します。保存されていないデータは失われます。';
        } else {
            $('#dlgTitle').textContent = t.group ? '「' + t.name + '」の ' + n + ' 個のプロセスを終了しますか?' : '「' + t.name + '」を終了しますか?';
            $('#dlgBody').textContent = (t.group
                ? n + ' 個のプロセス (PID ' + t.pids.slice(0, 6).join(', ') + (n > 6 ? ' ほか' : '') + ')'
                : 'PID ' + t.pids[0] + ' のプロセス') + 'を強制終了します。保存されていないデータは失われます。';
        }
        scrim.classList.add('open');
        $('#dlgCancel').focus();
    }
    function closeDialog() { scrim.classList.remove('open'); state.pending = null; }

    $('#kill').addEventListener('click', () => askKill(false));
    $('#mKill').addEventListener('click', () => askKill(false));
    $('#mTree').addEventListener('click', () => askKill(true));
    $('#mOpen').addEventListener('click', () => {
        const t = selTarget();
        closeMenu();
        if (t) post('open ' + t.openPid);
    });
    $('#mRefresh').addEventListener('click', () => { closeMenu(); post('refresh'); });
    $('#dlgCancel').addEventListener('click', closeDialog);
    scrim.addEventListener('click', e => { if (e.target === scrim) closeDialog(); });
    $('#dlgOk').addEventListener('click', () => {
        const p = state.pending;
        closeDialog();
        if (p && p.tree) {
            state.killName = p.name + ' とその子プロセス';
            state.killBatch = { left: 1, ok: 0, ng: 0, err: 0 };
            post('killtree ' + p.pids.join(' '));
        } else if (p) {
            state.killName = p.group ? p.name + ' (' + p.pids.length + ')' : p.name;
            state.killBatch = { left: p.pids.length, ok: 0, ng: 0, err: 0 };
            p.pids.forEach(pid => post('kill ' + pid));
        }
    });

    document.querySelectorAll('thead th').forEach(th => th.addEventListener('click', () => {
        const k = th.dataset.k;
        state.sort = state.sort.key === k
            ? { key: k, dir: -state.sort.dir }
            : { key: k, dir: (k === 'name' || k === 'path') ? 1 : -1 };
        render();
    }));

    q.addEventListener('input', () => { state.filter = q.value; render(); });

    addEventListener('keydown', e => {
        const typing = document.activeElement === q;
        if (e.key === 'Escape') { closeDialog(); closeMenu(); if (typing) { q.value = ''; state.filter = ''; render(); q.blur(); } return; }
        if (e.ctrlKey && (e.key === 'f' || e.key === 'F')) { e.preventDefault(); q.focus(); q.select(); return; }
        if (e.key === 'F5') { e.preventDefault(); post('refresh'); return; }
        if (scrim.classList.contains('open') || typing) return;
        if (e.key === 'Delete') { askKill(e.shiftKey); return; }   /* Shift+Delete = ツリーごと */
        if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
            e.preventDefault();
            const trs = [...rows.children].filter(tr => !tr.classList.contains('sec'));
            if (!trs.length) return;
            let i = trs.findIndex(tr => tr.dataset.key === state.sel);
            i = e.key === 'ArrowDown' ? Math.min(trs.length - 1, i + 1) : Math.max(0, i < 0 ? 0 : i - 1);
            select(trs[i].dataset.key);
            const el = [...rows.children].find(tr => tr.dataset.key === state.sel);
            if (el) el.scrollIntoView({ block: 'nearest' });
        } else if (e.key === 'ArrowRight' || e.key === 'ArrowLeft') {
            const it = lastItems.find(i => i.key === state.sel);
            if (it && it.group && it.open === (e.key === 'ArrowLeft')) { e.preventDefault(); toggleGroup(it.key); }
        }
    });

    /* ---------------- 更新間隔 / 一時停止 ---------------- */
    const ICON_PAUSE = '<svg viewBox="0 0 24 24" fill="currentColor"><rect x="6" y="5" width="4" height="14" rx="1.2"/><rect x="14" y="5" width="4" height="14" rx="1.2"/></svg>';
    const ICON_PLAY = '<svg viewBox="0 0 24 24" fill="currentColor"><path d="M8 5.5v13a1 1 0 0 0 1.5.9l10-6.5a1 1 0 0 0 0-1.8l-10-6.5A1 1 0 0 0 8 5.5z"/></svg>';
    function syncControls() {
        $('#pause').innerHTML = state.paused ? ICON_PLAY : ICON_PAUSE;
        $('#rate').textContent = (state.interval / 1000) + '秒';
        $('#state').textContent = stateText();
    }
    let timer = 0;
    function schedule() { clearTimeout(timer); timer = setTimeout(tick, state.interval); }
    function tick() { if (!state.paused && !document.hidden) post('refresh'); schedule(); }
    $('#pause').addEventListener('click', () => { state.paused = !state.paused; syncControls(); });
    $('#glass').addEventListener('click', () => {
        const on = document.documentElement.classList.contains('glass');
        post('glass ' + (on ? '0' : '1'));
    });
    $('#rate').addEventListener('click', () => {
        state.interval = INTERVALS[(INTERVALS.indexOf(state.interval) + 1) % INTERVALS.length];
        syncControls(); schedule();
    });
    document.addEventListener('visibilitychange', () => { if (!document.hidden && !state.paused) post('refresh'); });

    /* ---------------- デモモード (ブラウザで直接開いたとき) ---------------- */
    let mock = null;
    function mockPost(msg) {
        if (!mock) initMock();
        if (msg === 'init') {
            mock.sent.clear();
            onMessage({ type: 'host', glass: false, cores: 16, win: 'C:\\Windows' });
            mockIcons();
        } else if (msg.startsWith('glass ')) onMessage({ type: 'host', glass: msg === 'glass 1', cores: 16, win: 'C:\\Windows' });
        else if (msg === 'refresh') mock.tick();
        else if (msg.startsWith('kill ')) {
            const pid = +msg.slice(5);
            mock.procs = mock.procs.filter(p => p.pid !== pid);
            setTimeout(() => onMessage({ type: 'kill', pid, ok: true, error: 0 }), 100);
        } else if (msg.startsWith('killtree ')) {
            const dead = new Set(msg.slice(9).split(' ').map(Number));
            for (let ch = true; ch;) { ch = false; for (const p of mock.procs) if (!dead.has(p.pid) && dead.has(p.pp)) { dead.add(p.pid); ch = true; } }
            const n = mock.procs.filter(p => dead.has(p.pid)).length;
            mock.procs = mock.procs.filter(p => !dead.has(p.pid));
            setTimeout(() => onMessage({ type: 'kill', pid: 0, ok: true, error: 0, count: n, failed: 0 }), 100);
        } else if (msg.startsWith('open ')) {
            setTimeout(() => onMessage({ type: 'open', ok: true }), 50);
        }
    }
    function initMock() {
        document.title += ' (デモ)';
        const names = ['System Idle Process', 'System', 'explorer.exe', 'chrome.exe', 'chrome.exe', 'chrome.exe', 'chrome.exe', 'chrome.exe', 'chrome.exe', 'chrome.exe', 'chrome.exe', 'chrome.exe', 'chrome.exe', 'chrome.exe', 'chrome.exe', 'Code.exe', 'Code.exe',
            'Discord.exe', 'Spotify.exe', 'node.exe', 'python.exe', 'Teams.exe', 'dwm.exe', 'SearchHost.exe', 'svchost.exe', 'svchost.exe',
            'firefox.exe', 'notepad.exe', '日本語アプリ.exe'];
        const SYS = new Set(['dwm.exe', 'SearchHost.exe', 'svchost.exe', 'explorer.exe']);
        const APPS = new Set(['explorer.exe', 'chrome.exe', 'Code.exe', 'Discord.exe', 'Spotify.exe', 'Teams.exe', 'firefox.exe', 'notepad.exe', '日本語アプリ.exe']);
        mock = {
            sent: new Set(),
            procs: names.map((n, i) => ({
                pid: i === 0 ? 0 : 400 + i * 137, th: 2 + (i * 7) % 40, name: n, cpu: 0, gpu: 0, pp: names.indexOf(n) === i ? (i < 2 ? 0 : 400 + 2 * 137) : 400 + names.indexOf(n) * 137, app: APPS.has(n) && names.indexOf(n) === i,
                mem: i === 0 ? 0 : (30 + (i * 53) % 900) * 1048576,
                path: i < 2 ? '' : (SYS.has(n) ? 'C:\\Windows\\System32\\' : 'C:\\Program Files\\Demo\\') + n
            })),
            tick() {
                this.procs.forEach(p => {
                    if (p.pid === 0) { p.cpu = -1; return; }
                    p.cpu = Math.random() < 0.25 ? Math.random() * Math.random() * 60 : Math.random() * 1.5;
                    p.mem = Math.max(1048576, p.mem + (Math.random() - 0.5) * 4e6);
                    p.gpu = Math.random() < 0.15 ? Math.random() * 40 : 0;
                });
                const cpu = Math.min(100, this.procs.reduce((s, p) => s + Math.max(0, p.cpu), 0) / 3);
                onMessage({
                    type: 'update', cpu, cores: 16, gpuOk: true, gpu: Math.min(100, this.procs.reduce((s, p) => s + p.gpu, 0)), memTotal: 16 * 2 ** 30,
                    memUsed: this.procs.reduce((s, p) => s + p.mem, 0) + 5 * 2 ** 30,
                    disk: { active: Math.random() * 30, read: Math.random() * 8e6, write: Math.random() * 3e6 },
                    net: { rx: Math.random() * 2e6, tx: Math.random() * 4e5 },
                    drives: [{ n: 'C:', t: 512e9, f: 180e9 }, { n: 'D:', t: 1e12, f: 600e9 }],
                    procs: this.procs.map(p => {          /* pathは新しいプロセスのときだけ送る (本物のCと同じ) */
                        const o = { pid: p.pid, th: p.th, name: p.name, cpu: p.cpu, mem: p.mem };
                        if (p.app) o.app = 1;
                        o.pp = p.pp;
                        if (p.gpu > 0.05) o.gpu = p.gpu;
                        if (!this.sent.has(p.pid)) { this.sent.add(p.pid); o.path = p.path; }
                        return o;
                    })
                });
            }
        };
    }
    function mockIcons() {
        const items = [];
        mock.procs.forEach(p => {
            if (!p.path) return;
            const c = document.createElement('canvas');
            c.width = c.height = 48;
            const x = c.getContext && c.getContext('2d');
            if (!x) return;
            x.fillStyle = 'hsl(' + hue(p.name) + ',60%,52%)';
            x.beginPath(); x.roundRect(4, 4, 40, 40, 10); x.fill();
            x.fillStyle = '#fff'; x.font = 'bold 24px sans-serif'; x.textAlign = 'center'; x.textBaseline = 'middle';
            x.fillText(p.name.charAt(0).toUpperCase(), 24, 26);
            items.push({ k: p.path, d: c.toDataURL('image/png').split(',')[1] });
        });
        if (items.length) onMessage({ type: 'icons', items });
    }

    /* ---------------- 起動 ---------------- */
    syncControls();
    applyOpen();
    post('init');
    post('refresh');
    schedule();
})();