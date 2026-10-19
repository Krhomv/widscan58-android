// Channels tab, added by the Android app beside the widscan58 web UI: how busy
// each channel of each FPV platform is, read from the band panorama spectrum
// the UI already receives in monitor mode. The channel tables are Oscar
// Liang's 5.8 GHz FPV channel chart (https://oscarliang.com/fpv-channels/).
//
// The UI is unmodified; this reads its state through the window.widscan58
// hook ({ app, store }) it sets on connect, and adds a tab bar under its
// header. GPL-3.0-or-later.
(() => {
  "use strict";

  // ---- Channel tables (MHz, 5.8 GHz), Oscar Liang's chart (Sep 2023) ----
  // A channel is { name, mhz } plus, where the chart gives them: ce (its name
  // in CE mode; channels without one are FCC only), bw (MHz, when it differs
  // from its row's) and lane (0 top, 1 bottom: drawn half height, where the
  // chart staggers overlapping channels in one row).
  const band = (prefix, mhz) => mhz.map((f, j) => ({ name: `${prefix}${j + 1}`, mhz: f }));
  const chans = (...list) => list.map(([name, mhz, more]) => ({ name, mhz, ...more }));
  const PUBLIC = ["CH8", 5839, { ce: "CH8", bw: 20, pub: true }]; // the DJI / Avatar public channel
  const V1_25 = chans(
    ["CH1", 5660],
    ["CH2", 5695],
    ["CH3", 5735, { ce: "CE1" }],
    ["CH4", 5770, { ce: "CE2" }],
    ["CH5", 5805, { ce: "CE3" }],
    PUBLIC,
    ["CH6", 5878],
    ["CH7", 5914],
  );
  const V1_50 = chans(["CH1", 5695], ["CH2", 5770], PUBLIC, ["CH3", 5878]);
  const O3_20 = chans(
    ["CH1", 5669],
    ["CH2", 5705],
    ["CH3", 5768, { ce: "CE1" }],
    ["CH4", 5804, { ce: "CE2" }],
    ["CH5", 5839, { ce: "CE3" }],
    ["CH6", 5876],
    ["CH7", 5912],
  );
  const PLATFORMS = [
    { id: "A", group: "Analog", label: "Analog A", sub: "Boscam A", bw: 30, chans: band("A", [5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725]) },
    { id: "B", group: "Analog", label: "Analog B", sub: "Boscam B", bw: 30, chans: band("B", [5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866]) },
    { id: "E", group: "Analog", label: "Analog E", sub: "Boscam E", bw: 30, chans: band("E", [5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945]) },
    { id: "F", group: "Analog", label: "Analog F", sub: "Fatshark", bw: 30, chans: band("F", [5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880]) },
    { id: "R", group: "Analog", label: "Analog R", sub: "Raceband", bw: 30, chans: band("R", [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917]) },
    // Not on the chart; from the table on the same page.
    { id: "H", group: "Analog", label: "Analog H", sub: "Band H", bw: 30, chans: band("H", [5653, 5693, 5733, 5773, 5813, 5853, 5893, 5933]) },
    { id: "v1-25", group: "DJI V1 / Vista", label: "DJI V1 25M", sub: "25 Mbps", bw: 20, chans: V1_25 },
    { id: "v1-50", group: "DJI V1 / Vista", label: "DJI V1 50M", sub: "50 Mbps", bw: 40, chans: V1_50 },
    { id: "o3-10", group: "DJI O3 / O4", label: "DJI O3 10M", sub: "10 MHz (14 Mbps)", bw: 10, chans: O3_20 },
    { id: "o3-20", group: "DJI O3 / O4", label: "DJI O3 20M", sub: "20 MHz (25 Mbps)", bw: 20, chans: O3_20 },
    {
      id: "o3-40",
      group: "DJI O3 / O4",
      label: "DJI O3 40M",
      sub: "40 MHz (50 Mbps)",
      bw: 40,
      chans: chans(["CH1", 5677], ["CH2", 5794, { ce: "CE1" }], ["CH3", 5902]),
    },
    {
      id: "hdz",
      group: "HDZero",
      label: "HDZero",
      sub: "Raceband + F2, F4",
      bw: 27,
      chans: chans(
        ["R1", 5658],
        ["R2", 5695],
        ["R3", 5732, { ce: "CE1", lane: 0 }],
        ["F2", 5760, { lane: 1 }],
        ["R4", 5769, { ce: "CE2", lane: 0 }],
        ["F4", 5800, { lane: 1 }],
        ["R5", 5806, { ce: "CE3", lane: 0 }],
        ["R6", 5843, { ce: "CE4" }],
        ["R7", 5880],
        ["R8", 5917],
      ),
    },
    // Race mode's width is read off the chart's drawing. The chart prints R2
    // as 5659, a typo for Raceband R2.
    { id: "ws-race", group: "Walksnail Avatar", label: "WS Race", sub: "540p race mode", bw: 16, chans: band("R", [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917]) },
    { id: "ws-25", group: "Walksnail Avatar", label: "WS 25M", sub: "Standard 25 Mbps", bw: 20, chans: V1_25 },
    { id: "ws-50", group: "Walksnail Avatar", label: "WS 50M", sub: "Standard 50 Mbps", bw: 40, chans: V1_50 },
  ];
  /** A channel's name in CE mode, or null where CE has no such channel: the
   *  chart's CE names where its row gives them, otherwise the channels
   *  centred in the chart's CE range (5725-5875 MHz). */
  const CE_MHZ = [5725, 5875];
  const ceName = (pl, c) =>
    pl.chans.some((x) => x.ce && !x.pub) ? c.ce ?? null : c.mhz >= CE_MHZ[0] && c.mhz <= CE_MHZ[1] ? c.name : null;
  const DEFAULT_PLATFORMS = ["R", "F", "v1-25", "o3-20", "hdz", "ws-25"];
  // Level above the band's noise floor (dB) at which a channel counts as busy.
  const LEVELS = [
    { min: -Infinity, name: "Clear", color: "--good" },
    { min: 4, name: "Light", color: "--warning" },
    { min: 9, name: "Busy", color: "--serious" },
    { min: 16, name: "Occupied", color: "--critical" },
  ];
  const levelOf = (db) => LEVELS.reduce((l, x) => (db >= x.min ? x : l), LEVELS[0]);

  // ---- Settings (per viewer, this phone) ----
  const KEY = "widscan58-android.channels";
  const settings = { platforms: DEFAULT_PLATFORMS, region: "FCC", trace: "avg", tab: "scanner" };
  try {
    Object.assign(settings, JSON.parse(localStorage.getItem(KEY) || "{}"));
  } catch {
    /* storage unavailable or corrupt */
  }
  if (!Array.isArray(settings.platforms)) settings.platforms = DEFAULT_PLATFORMS;
  settings.platforms = settings.platforms.filter((id) => PLATFORMS.some((p) => p.id === id));
  if (settings.region !== "CE") settings.region = "FCC";
  const save = () => {
    try {
      localStorage.setItem(KEY, JSON.stringify(settings));
    } catch {
      /* storage unavailable */
    }
  };

  // ---- DOM helper ----
  const el = (tag, attrs = {}, ...kids) => {
    const e = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
    e.append(...kids);
    return e;
  };
  const css = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

  const style = el(
    "style",
    {},
    `
nav.aw-tabs { display: flex; gap: 4px; padding: 0 16px; border-bottom: 1px solid var(--border); }
nav.aw-tabs button { background: none; border: 0; border-bottom: 2px solid transparent; border-radius: 0; padding: 8px 12px; color: var(--ink-2); }
nav.aw-tabs button[aria-selected="true"] { color: var(--ink); border-bottom-color: var(--accent); font-weight: 600; }
body.aw-tab-channels #app > main:not(.aw-channels) { display: none !important; }
main.aw-channels { display: none; }
body.aw-tab-channels main.aw-channels { display: grid; }
.aw-note { color: var(--ink-2); margin: 4px 0 8px; }
.aw-note:empty { display: none; }
.aw-chart { position: relative; }
.aw-chart canvas { display: block; width: 100%; touch-action: manipulation; }
.aw-tip { position: absolute; pointer-events: none; background: var(--surface-2); border: 1px solid var(--border); border-radius: 6px; padding: 4px 8px; font-size: 12px; max-width: 100%; z-index: 2; }
.aw-legend i { height: 8px !important; width: 12px !important; border-radius: 2px; }
.aw-seg { display: inline-flex; }
.aw-seg button { border-radius: 0; margin-left: -1px; }
.aw-seg button:first-child { border-radius: 6px 0 0 6px; margin-left: 0; }
.aw-seg button:last-child { border-radius: 0 6px 6px 0; }
.aw-picker { display: grid; grid-template-columns: repeat(auto-fill, minmax(150px, 1fr)); gap: 4px 16px; margin: 4px 0 10px; }
.aw-picker h3 { grid-column: 1 / -1; font-size: 11px; font-weight: 600; color: var(--muted); margin: 6px 0 0; text-transform: uppercase; letter-spacing: 0.06em; }
.aw-picker[hidden] { display: none; }
.aw-picker label { display: flex; gap: 6px; align-items: center; }
.aw-picker label span { color: var(--muted); }
table.aw-best { border-collapse: collapse; width: 100%; font-variant-numeric: tabular-nums; }
table.aw-best th { text-align: left; font-size: 11px; font-weight: 500; color: var(--muted); padding: 2px 8px 4px 0; }
table.aw-best td { padding: 3px 8px 3px 0; border-top: 1px solid var(--border); }
table.aw-best td span { white-space: nowrap; }
table.aw-best i { display: inline-block; width: 8px; height: 8px; border-radius: 2px; margin-right: 5px; }
`,
  );

  // ---- Spectrum: one trace over the band, from the panorama segments ----
  const STEP = 0.25; // MHz per point

  /** The profile's device frequency (MHz) for a 5.8 GHz frequency, and back
   *  (the ESP32-S3 mock-up scales the band into 2.4 GHz). */
  const toDevice = (p, mhz) => (p.base_khz + (mhz * 1000 - p.equiv_base_khz) * p.scale) / 1000;
  const fromDevice = (p, mhz) => (mhz * 1000 - p.base_khz) / p.scale / 1000 + p.equiv_base_khz / 1000;

  /** dB per point from f0 (5.8 GHz MHz); NaN where no segment reaches. Each
   *  point comes from the segment whose LO is nearest, as in the panorama. */
  function trace(store, trace, f0, n) {
    const p = store.profile;
    const out = new Float32Array(n).fill(NaN);
    const best = new Float32Array(n).fill(Infinity);
    for (const s of store.segments.values()) {
      const sp = s.latest;
      const lo = sp.loKhz / 1000;
      const start = lo + sp.startOffHz / 1e6; // device MHz, centre of bin 0
      const bin = sp.binHz / 1e6;
      const a = Math.max(0, Math.floor((fromDevice(p, start - bin / 2) - f0) / STEP));
      const b = Math.min(n - 1, Math.ceil((fromDevice(p, start + (sp.db.length - 0.5) * bin) - f0) / STEP));
      for (let x = a; x <= b; x++) {
        const f = toDevice(p, f0 + x * STEP);
        const k = Math.round((f - start) / bin);
        if (k < 0 || k >= sp.db.length) continue;
        const d = Math.abs(f - lo);
        if (d >= best[x]) continue;
        best[x] = d;
        out[x] = trace === "max" ? s.maxHold[k] : trace === "live" ? sp.db[k] : 10 * Math.log10(s.avg[k] + 1e-30);
      }
    }
    return out;
  }

  /** The band's noise floor: the 10th percentile of the scanned points. */
  function floorOf(tr) {
    const v = Array.from(tr).filter((x) => !Number.isNaN(x)).sort((a, b) => a - b);
    return v.length ? v[Math.floor(v.length * 0.1)] : NaN;
  }

  /** Mean power over the channel (dB above floor) and the share of it scanned. */
  function measure(tr, f0, floor, c, bw) {
    const a = Math.max(0, Math.round((c.mhz - bw / 2 - f0) / STEP));
    const b = Math.min(tr.length - 1, Math.round((c.mhz + bw / 2 - f0) / STEP));
    let sum = 0;
    let got = 0;
    for (let x = a; x <= b; x++) {
      if (Number.isNaN(tr[x])) continue;
      sum += 10 ** (tr[x] / 10);
      got++;
    }
    const total = Math.max(1, Math.round(bw / STEP) + 1);
    return { db: got ? 10 * Math.log10(sum / got) - floor : NaN, coverage: got / total };
  }

  // ---- The view ----
  function build(header) {
    const tabs = {
      scanner: el("button", { role: "tab" }, "Scanner"),
      channels: el("button", { role: "tab" }, "Channels"),
    };
    const nav = el("nav", { class: "aw-tabs", role: "tablist" }, tabs.scanner, tabs.channels);

    /** Buttons that pick one value of a setting. */
    const segmented = (key, options, title) => {
      const seg = el("span", { class: "aw-seg", title });
      for (const [k, name] of Object.entries(options)) {
        const b = el("button", { "data-k": k }, name);
        b.onclick = () => {
          settings[key] = k;
          save();
          sync();
        };
        seg.append(b);
      }
      return seg;
    };
    const traceSeg = segmented("trace", { avg: "Average", live: "Live", max: "Max hold" }, "Spectrum trace the levels come from");
    const regionSeg = segmented("region", { FCC: "FCC", CE: "CE" }, "CE shows the chart's CE channels, or those inside its CE range");
    const resetBtn = el("button", { title: "Start the max hold and average over" }, "Reset holds");
    resetBtn.onclick = () => window.widscan58?.store.resetHolds();
    const pickBtn = el("button", { "aria-expanded": "false" }, "Platforms");

    const picker = el("div", { class: "aw-picker", hidden: "" });
    let group = "";
    for (const p of PLATFORMS) {
      if (p.group !== group) picker.append(el("h3", {}, (group = p.group)));
      const box = el("input", { type: "checkbox" });
      box.checked = settings.platforms.includes(p.id);
      box.onchange = () => {
        settings.platforms = PLATFORMS.filter((q) => (q === p ? box.checked : settings.platforms.includes(q.id))).map((q) => q.id);
        save();
        dirty = true;
      };
      picker.append(el("label", {}, box, p.group === "Analog" ? `${p.id} ${p.sub}` : p.sub, el("span", {}, `${p.bw} MHz`)));
    }
    pickBtn.onclick = () => {
      picker.hidden = !picker.hidden;
      pickBtn.classList.toggle("on", !picker.hidden);
      pickBtn.setAttribute("aria-expanded", String(!picker.hidden));
    };

    const note = el("div", { class: "aw-note" });
    const canvas = el("canvas");
    const tip = el("div", { class: "aw-tip", hidden: "" });
    const legend = el("div", { class: "legend aw-legend" });
    const best = el("table", { class: "aw-best" });
    const card = el(
      "section",
      { class: "card span-12" },
      el("h2", {}, "Channel occupancy", el("span", { class: "tools" }, regionSeg, traceSeg, resetBtn, pickBtn)),
      picker,
      note,
      el("div", { class: "aw-chart" }, canvas, tip),
      legend,
    );
    const bestCard = el("section", { class: "card span-12" }, el("h2", {}, "Clearest channel per platform"), best);
    const main = el("main", { class: "aw-channels" }, card, bestCard);
    header.after(nav);
    document.getElementById("app").append(main);

    function sync() {
      const on = settings.tab === "channels";
      document.body.classList.toggle("aw-tab-channels", on);
      tabs.scanner.setAttribute("aria-selected", String(!on));
      tabs.channels.setAttribute("aria-selected", String(on));
      for (const b of traceSeg.children) b.classList.toggle("on", b.dataset.k === settings.trace);
      for (const b of regionSeg.children) b.classList.toggle("on", b.dataset.k === settings.region);
      dirty = true;
      // The scanner's canvases size themselves on redraw.
      if (!on) window.dispatchEvent(new Event("resize"));
    }
    tabs.scanner.onclick = () => {
      settings.tab = "scanner";
      save();
      sync();
    };
    tabs.channels.onclick = () => {
      settings.tab = "channels";
      save();
      sync();
    };

    // Hit boxes of the channel blocks, for the tap/hover readout.
    let hits = [];
    const showTip = (ev) => {
      const r = canvas.getBoundingClientRect();
      const x = ev.clientX - r.left;
      const y = ev.clientY - r.top;
      const h = hits.find((b) => x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h);
      if (!h) return void (tip.hidden = true);
      const lv = Number.isNaN(h.m.db) || h.m.coverage < 0.6 ? "not scanned" : `${h.m.db.toFixed(1)} dB above floor, ${levelOf(h.m.db).name.toLowerCase()}`;
      const ce = settings.region === "CE" && ceName(h.p, h.c);
      const name = ce && ce !== h.c.name ? `${ce} (${h.c.name})` : h.c.name;
      tip.textContent = `${h.p.label} ${name}${h.c.pub ? " public" : ""} · ${h.c.mhz} MHz · ${h.c.bw ?? h.p.bw} MHz wide · ${lv}`;
      tip.hidden = false;
      const left = Math.max(0, Math.min(h.x + h.w / 2 - tip.offsetWidth / 2, r.width - tip.offsetWidth));
      tip.style.left = `${left}px`;
      tip.style.top = `${h.y - tip.offsetHeight - 4}px`;
    };
    canvas.addEventListener("pointermove", showTip);
    canvas.addEventListener("pointerdown", showTip);
    canvas.addEventListener("pointerleave", (e) => e.pointerType === "mouse" && (tip.hidden = true));

    legend.replaceChildren(
      ...LEVELS.map((l, j) =>
        el(
          "span",
          {},
          el("i", { style: `background:var(${l.color})` }),
          j === 0 ? `Clear (< ${LEVELS[1].min} dB)` : j === LEVELS.length - 1 ? `${l.name} (≥ ${l.min} dB)` : `${l.name} (${l.min}–${LEVELS[j + 1].min} dB)`,
        ),
      ),
      el("span", {}, el("i", { style: "background:var(--grid)" }), "Not scanned"),
      el("span", {}, "level = mean power over the channel above the band's noise floor"),
    );

    let dirty = true;
    let drawn = -1;
    let width = 0;
    function render() {
      if (settings.tab !== "channels") return;
      const w = window.widscan58;
      const store = w?.store;
      const live = !!(w && w.app.device);
      const mode = w?.app.settings.uiMode;
      if (!dirty && store && store.version === drawn && canvas.clientWidth === width) return;
      dirty = false;
      drawn = store ? store.version : -1;
      width = canvas.clientWidth;

      const rows = PLATFORMS.filter((p) => settings.platforms.includes(p.id));
      const ready = live && mode === "monitor" && store.profile && store.segments.size > 0;
      note.textContent = !live
        ? "Connect the board (or press Simulate in the Scanner tab's console) to see how busy each channel is."
        : mode !== "monitor"
          ? "The channels view reads the band spectrum of Monitor mode: switch to Monitor mode in the menu (☰)."
          : !ready
            ? store.status && store.status.spec_every === 0
              ? "The scanner's spectrum is off, so there is nothing to measure."
              : "Waiting for the spectrum…"
            : !rows.length
              ? "Pick platforms to show with Platforms."
              : "";

      const p = ready ? store.profile : null;
      const F0 = 5640;
      const F1 = 5955;
      const n = Math.round((F1 - F0) / STEP) + 1;
      const tr = p ? trace(store, settings.trace, F0, n) : new Float32Array(n).fill(NaN);
      const floor = floorOf(tr);

      // Layout: frequency axis and the spectrum strip on top, a row per platform.
      const narrow = width < 560;
      const labelW = narrow ? 74 : 112;
      const stripH = 56;
      const axisH = 18;
      const rowH = narrow ? 30 : 32;
      const top = 4;
      const h = top + stripH + axisH + rows.length * rowH + 4;
      canvas.style.height = `${h}px`;
      const dpr = window.devicePixelRatio || 1;
      canvas.width = Math.max(1, Math.round(width * dpr));
      canvas.height = Math.round(h * dpr);
      const ctx = canvas.getContext("2d");
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      ctx.clearRect(0, 0, width, h);
      const left = labelW;
      const right = width - 6;
      const sx = (f) => left + ((f - F0) / (F1 - F0)) * (right - left);
      const font = (px, weight = 400) => `${weight} ${px}px ${css("--font") || "sans-serif"}`;
      const C = {
        ink: css("--ink"),
        ink2: css("--ink-2"),
        muted: css("--muted"),
        grid: css("--grid"),
        axis: css("--axis"),
        accent: css("--accent"),
        page: css("--surface"),
      };

      // Grid lines every 50 MHz, labels every 50 (25 when wide).
      const stripTop = top;
      const stripBot = top + stripH;
      const rowsTop = stripBot + axisH;
      ctx.font = font(10);
      ctx.textAlign = "center";
      ctx.textBaseline = "middle";
      for (let f = Math.ceil(F0 / 25) * 25; f <= F1; f += 25) {
        const x = Math.round(sx(f)) + 0.5;
        const major = f % 50 === 0;
        ctx.strokeStyle = C.grid;
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(x, stripTop);
        ctx.lineTo(x, h - 4);
        ctx.stroke();
        if (major || !narrow) {
          ctx.fillStyle = C.muted;
          ctx.fillText(String(f), x, stripBot + axisH / 2);
        }
      }
      ctx.textAlign = "left";
      ctx.fillStyle = C.muted;
      ctx.fillText("MHz", 4, stripBot + axisH / 2);
      ctx.fillStyle = C.ink2;
      ctx.font = font(11, 600);
      ctx.fillText("Spectrum", 4, stripTop + 12);
      ctx.font = font(10);
      ctx.fillStyle = C.muted;
      ctx.fillText({ avg: "average", live: "live", max: "max hold" }[settings.trace], 4, stripTop + 26);

      // Spectrum strip, scaled from the floor up to the strongest point.
      if (!Number.isNaN(floor)) {
        let peak = floor + 20;
        for (const v of tr) if (v > peak) peak = v;
        const y0 = floor - 3;
        const sy = (v) => stripBot - 2 - ((Math.min(peak, Math.max(y0, v)) - y0) / (peak - y0)) * (stripH - 6);
        ctx.fillStyle = C.accent;
        ctx.globalAlpha = 0.25;
        ctx.beginPath();
        let pen = false;
        let startX = 0;
        const close = (x) => {
          ctx.lineTo(x, stripBot);
          ctx.lineTo(startX, stripBot);
          ctx.closePath();
        };
        let lastX = 0;
        for (let x = 0; x < n; x++) {
          const v = tr[x];
          const px = sx(F0 + x * STEP);
          if (Number.isNaN(v)) {
            if (pen) close(lastX);
            pen = false;
            continue;
          }
          if (!pen) {
            ctx.moveTo(px, stripBot);
            startX = px;
          }
          ctx.lineTo(px, sy(v));
          pen = true;
          lastX = px;
        }
        if (pen) close(lastX);
        ctx.fill();
        ctx.globalAlpha = 1;
        ctx.strokeStyle = C.accent;
        ctx.lineWidth = 1.25;
        ctx.beginPath();
        pen = false;
        for (let x = 0; x < n; x++) {
          const v = tr[x];
          if (Number.isNaN(v)) {
            pen = false;
            continue;
          }
          const px = sx(F0 + x * STEP);
          if (pen) ctx.lineTo(px, sy(v));
          else ctx.moveTo(px, sy(v));
          pen = true;
        }
        ctx.stroke();
      }
      ctx.strokeStyle = C.axis;
      ctx.beginPath();
      ctx.moveTo(left, stripBot + 0.5);
      ctx.lineTo(right, stripBot + 0.5);
      ctx.stroke();

      // A row per platform: each channel a block over its bandwidth, trimmed
      // so neighbours on the same row don't overlap.
      hits = [];
      const results = [];
      rows.forEach((pl, r) => {
        const y = rowsTop + r * rowH;
        ctx.textAlign = "left";
        ctx.fillStyle = C.ink;
        ctx.font = font(11, 600);
        ctx.fillText(pl.label, 4, y + rowH / 2 - (narrow ? 0 : 6));
        if (!narrow) {
          ctx.fillStyle = C.muted;
          ctx.font = font(10);
          ctx.fillText(`${pl.bw} MHz`, 4, y + rowH / 2 + 7);
        }
        const ce = settings.region === "CE";
        const shown = pl.chans.filter((c) => !ce || ceName(pl, c)).map((c) => ({ c, name: ce ? ceName(pl, c) : c.name, bw: c.bw ?? pl.bw }));
        if (!shown.length) {
          ctx.textAlign = "left";
          ctx.fillStyle = C.muted;
          ctx.font = font(11);
          ctx.fillText("No channels in CE", left + 4, y + rowH / 2);
        }
        const res = [];
        for (const s of shown) {
          const { c, bw } = s;
          // Trim against the neighbours sharing its lane (or the whole row).
          let half = bw / 2;
          for (const o of shown)
            if (o !== s && (c.lane === undefined || o.c.lane === undefined || o.c.lane === c.lane))
              half = Math.min(half, Math.abs(o.c.mhz - c.mhz) / 2);
          const x0 = sx(c.mhz - half) + 1;
          const x1 = sx(c.mhz + half) - 1;
          const by = y + 3 + (c.lane === 1 ? (rowH - 6) / 2 + 1 : 0);
          const bh = c.lane === undefined ? rowH - 6 : (rowH - 6) / 2 - 1;
          const m = p ? measure(tr, F0, floor, c, bw) : { db: NaN, coverage: 0 };
          const scanned = !Number.isNaN(m.db) && m.coverage >= 0.6;
          const lv = scanned ? levelOf(m.db) : null;
          ctx.fillStyle = lv ? css(lv.color) : C.grid;
          ctx.globalAlpha = lv ? 0.85 : 1;
          ctx.beginPath();
          ctx.roundRect(x0, by, Math.max(1, x1 - x0), bh, 4);
          ctx.fill();
          ctx.globalAlpha = 1;
          const label = narrow ? s.name.replace("CH", "") : s.name;
          ctx.font = font(c.lane === undefined ? 10 : 9, 600);
          ctx.textAlign = "center";
          if (ctx.measureText(label).width < x1 - x0 - 2) {
            ctx.fillStyle = lv ? "#0b0b0b" : C.muted;
            ctx.fillText(label, (x0 + x1) / 2, by + bh / 2 + 0.5);
          }
          hits.push({ x: x0, y: by, w: x1 - x0, h: bh, p: pl, c, m });
          res.push({ c, name: s.name, m, scanned });
        }
        results.push({ pl, res });
      });

      // Clearest (and busiest) scanned channel of each platform.
      const cell = (r) =>
        r
          ? el(
              "td",
              {},
              el("span", {}, el("i", { style: `background:var(${levelOf(r.m.db).color})` }), `${r.name} ${r.c.mhz}`),
              " ",
              el("span", { class: "muted" }, `${r.m.db.toFixed(1)} dB`),
            )
          : el("td", { class: "muted" }, "–");
      best.replaceChildren(
        el("tr", {}, el("th", {}, "Platform"), el("th", {}, "Clearest"), el("th", {}, "Busiest"), el("th", {}, "Clear")),
        ...results.map(({ pl, res }) => {
          const s = res.filter((r) => r.scanned).sort((a, b) => a.m.db - b.m.db);
          const clear = s.filter((r) => r.m.db < LEVELS[1].min).length;
          return el("tr", {}, el("td", {}, pl.label), cell(s[0]), cell(s.length > 1 ? s[s.length - 1] : null), el("td", {}, s.length ? `${clear}/${s.length}` : "–"));
        }),
      );
      const unscanned = results.some(({ res }) => res.some((r) => !r.scanned));
      if (ready && rows.length && unscanned)
        note.textContent =
          "Grey channels are outside what the scanner covers: it only sees the spectrum around the channels it scans. " +
          "Spread its channels across the band (Scanner tab, Channels, Edit) to fill the gaps.";
    }

    sync();
    setInterval(render, 200);
    window.addEventListener("resize", () => (dirty = true));
    new MutationObserver(() => (dirty = true)).observe(document.documentElement, { attributes: true, attributeFilter: ["data-theme"] });
  }

  // Loaded from <head>: the UI builds its page in module scripts, which have
  // run by DOMContentLoaded.
  function start() {
    const header = document.querySelector("#app > header.top");
    if (!header) return;
    document.head.append(style);
    build(header);
  }
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", start);
  else start();
})();
