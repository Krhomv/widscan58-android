// Runs before the widscan58 web UI inside the Android app's WebView, and gives
// it the browser APIs Android's WebView lacks, backed by the app (MainActivity,
// UsbBridge):
//   - navigator.usb: the subset of WebUSB the UI's CDC-ACM port and esptool-js
//     use (web/src/serial/webusb.ts), on top of Android's USB host API.
//   - speechSynthesis / SpeechSynthesisUtterance: lap callouts through
//     Android's text-to-speech.
//   - navigator.wakeLock: the app keeps the screen on anyway.
//   - <a download href="blob:..."> saves (recordings) into Downloads.
// The UI itself is unmodified. GPL-3.0-or-later.
(() => {
  "use strict";
  const N = window.WidscanNative;
  if (!N) return;

  // ---- helpers ----
  const b64enc = (u8) => {
    let s = "";
    for (let i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000));
    return btoa(s);
  };
  const b64dec = (s) => {
    const bin = atob(s);
    const u8 = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) u8[i] = bin.charCodeAt(i);
    return u8;
  };
  const bytesOf = (d) => {
    if (d === undefined || d === null) return new Uint8Array(0);
    if (d instanceof Uint8Array) return d;
    if (ArrayBuffer.isView(d)) return new Uint8Array(d.buffer, d.byteOffset, d.byteLength);
    return new Uint8Array(d);
  };
  const err = (name, msg) => new DOMException(msg, name);

  // Native calls answer through __widscan._cb(id, ok, result).
  const pending = new Map();
  let nextCb = 1;
  const call = (fn) =>
    new Promise((resolve, reject) => {
      const id = nextCb++;
      pending.set(id, { resolve, reject });
      fn(id);
    });

  // ---- WebUSB ----
  const REQUEST_TYPE = { standard: 0x00, class: 0x20, vendor: 0x40 };
  const RECIPIENT = { device: 0x00, interface: 0x01, endpoint: 0x02, other: 0x03 };
  const devices = new Map(); // native id -> USBDevice

  class Endpoint {
    constructor(e) {
      this.endpointNumber = e.endpointNumber;
      this.direction = e.direction;
      this.type = e.type;
      this.packetSize = e.packetSize;
    }
  }
  class Alternate {
    constructor(a) {
      this.alternateSetting = a.alternateSetting;
      this.interfaceClass = a.interfaceClass;
      this.interfaceSubclass = a.interfaceSubclass;
      this.interfaceProtocol = a.interfaceProtocol;
      this.interfaceName = a.interfaceName || null;
      this.endpoints = a.endpoints.map((e) => new Endpoint(e));
    }
  }
  class Interface {
    constructor(i) {
      this.interfaceNumber = i.interfaceNumber;
      this.alternates = i.alternates.map((a) => new Alternate(a));
      this.alternate = this.alternates[0];
      this.claimed = false;
    }
  }
  class Configuration {
    constructor(c) {
      this.configurationValue = c.configurationValue;
      this.configurationName = c.configurationName || null;
      this.interfaces = c.interfaces.map((i) => new Interface(i));
    }
  }

  class USBDevice {
    constructor(info) {
      this._id = info.id;
      this.vendorId = info.vendorId;
      this.productId = info.productId;
      this.deviceClass = info.deviceClass;
      this.deviceSubclass = info.deviceSubclass;
      this.deviceProtocol = info.deviceProtocol;
      this.productName = info.productName || "";
      this.manufacturerName = info.manufacturerName || "";
      this.serialNumber = info.serialNumber || "";
      this.usbVersionMajor = 2;
      this.usbVersionMinor = 0;
      this.usbVersionSubminor = 0;
      this.deviceVersionMajor = 0;
      this.deviceVersionMinor = 0;
      this.deviceVersionSubminor = 0;
      this.configurations = info.configurations.map((c) => new Configuration(c));
      // Android hands over devices already configured.
      this.configuration = this.configurations[0] || null;
      this.opened = false;
      this._gone = false;
      this._in = new Map(); // endpoint number -> { chunks, size, waiters, error }
    }

    _check() {
      if (this._gone) throw err("NotFoundError", "The device was disconnected.");
      if (!this.opened) throw err("InvalidStateError", "The device must be opened first.");
    }

    async open() {
      if (this._gone) throw err("NotFoundError", "The device was disconnected.");
      if (this.opened) return;
      if (!N.usbOpen(this._id)) throw err("SecurityError", "Access denied.");
      this.opened = true;
    }

    async close() {
      if (!this.opened) return;
      this.opened = false;
      N.usbClose(this._id);
      this._fail(err("AbortError", "The transfer was cancelled."));
      for (const i of this.configuration?.interfaces ?? []) i.claimed = false;
    }

    async selectConfiguration(value) {
      this._check();
      const c = this.configurations.find((c) => c.configurationValue === value);
      if (!c) throw err("NotFoundError", "The configuration value provided is not supported by the device.");
      this.configuration = c;
    }

    _iface(n) {
      const i = this.configuration?.interfaces.find((i) => i.interfaceNumber === n);
      if (!i) throw err("NotFoundError", "The interface number provided is not supported by the device in its current configuration.");
      return i;
    }

    async claimInterface(n) {
      this._check();
      const i = this._iface(n);
      if (i.claimed) return;
      if (!N.usbClaim(this._id, n)) throw err("NetworkError", "Unable to claim interface.");
      i.claimed = true;
    }

    async releaseInterface(n) {
      this._check();
      const i = this._iface(n);
      if (!i.claimed) return;
      N.usbRelease(this._id, n);
      i.claimed = false;
    }

    async selectAlternateInterface(n, alt) {
      this._check();
      const i = this._iface(n);
      const a = i.alternates.find((a) => a.alternateSetting === alt);
      if (!a) throw err("NotFoundError", "The alternate setting provided is not supported by the device.");
      if (alt !== 0) throw err("NotSupportedError", "Alternate settings are not supported here.");
      i.alternate = a;
    }

    // The app reads an IN endpoint continuously once it is first used and
    // pushes the bytes here (_in); each transferIn takes what has arrived.
    _queue(ep) {
      let q = this._in.get(ep);
      if (!q) this._in.set(ep, (q = { chunks: [], size: 0, waiters: [], error: null, started: false }));
      return q;
    }

    _take(q, length) {
      const n = Math.min(length, q.size);
      const out = new Uint8Array(n);
      let off = 0;
      while (off < n) {
        const c = q.chunks[0];
        const k = Math.min(c.length, n - off);
        out.set(c.subarray(0, k), off);
        off += k;
        if (k === c.length) q.chunks.shift();
        else q.chunks[0] = c.subarray(k);
      }
      q.size -= n;
      return { status: "ok", data: new DataView(out.buffer) };
    }

    _push(ep, bytes) {
      const q = this._queue(ep);
      q.chunks.push(bytes);
      q.size += bytes.length;
      while (q.size > 0 && q.waiters.length) {
        const w = q.waiters.shift();
        w.resolve(this._take(q, w.length));
      }
    }

    _fail(e, ep) {
      for (const [n, q] of this._in) {
        if (ep !== undefined && n !== ep) continue;
        q.error = e;
        for (const w of q.waiters.splice(0)) w.reject(e);
      }
      if (ep === undefined) this._in.clear();
    }

    async transferIn(ep, length) {
      this._check();
      const q = this._queue(ep);
      if (q.error) throw q.error;
      if (!q.started) {
        q.started = true;
        if (!N.usbStartIn(this._id, ep)) {
          q.started = false;
          throw err("NotFoundError", "The specified endpoint is not part of a claimed and selected alternate interface.");
        }
      }
      if (q.size > 0) return this._take(q, length);
      return new Promise((resolve, reject) => q.waiters.push({ length, resolve, reject }));
    }

    async transferOut(ep, data) {
      this._check();
      const n = await call((cb) => N.usbTransferOut(this._id, ep, b64enc(bytesOf(data)), cb));
      if (n < 0) throw err("NetworkError", "A transfer error has occurred.");
      return { status: "ok", bytesWritten: n };
    }

    async controlTransferOut(setup, data) {
      this._check();
      const type = (REQUEST_TYPE[setup.requestType] ?? 0) | (RECIPIENT[setup.recipient] ?? 0);
      const n = await call((cb) =>
        N.usbControlOut(this._id, type, setup.request, setup.value, setup.index, b64enc(bytesOf(data)), cb),
      );
      if (n < 0) throw err("NetworkError", "A transfer error has occurred.");
      return { status: "ok", bytesWritten: n };
    }

    async controlTransferIn() {
      throw err("NotSupportedError", "controlTransferIn is not supported here.");
    }

    async clearHalt() {}

    async reset() {
      throw err("NotSupportedError", "reset is not supported here.");
    }

    async forget() {}
  }

  const deviceFor = (info) => {
    let d = devices.get(info.id);
    if (!d) devices.set(info.id, (d = new USBDevice(info)));
    return d;
  };
  const list = () => JSON.parse(N.usbDevices());

  const matches = (info, f) => {
    if (f.vendorId !== undefined && f.vendorId !== info.vendorId) return false;
    if (f.productId !== undefined && f.productId !== info.productId) return false;
    if (f.serialNumber !== undefined && f.serialNumber !== info.serialNumber) return false;
    if (f.classCode === undefined) return true;
    const classes = [{ c: info.deviceClass, s: info.deviceSubclass, p: info.deviceProtocol }];
    for (const c of info.configurations)
      for (const i of c.interfaces)
        for (const a of i.alternates) classes.push({ c: a.interfaceClass, s: a.interfaceSubclass, p: a.interfaceProtocol });
    return classes.some(
      (x) =>
        x.c === f.classCode &&
        (f.subclassCode === undefined || x.s === f.subclassCode) &&
        (f.protocolCode === undefined || x.p === f.protocolCode),
    );
  };

  class USB extends EventTarget {
    constructor() {
      super();
      this.onconnect = null;
      this.ondisconnect = null;
    }

    async getDevices() {
      return list()
        .filter((i) => i.granted)
        .map(deviceFor);
    }

    // No chooser: the first attached device that matches, after Android's
    // own permission dialog when the app does not have access yet.
    async requestDevice(options) {
      const filters = options?.filters ?? [];
      const info = list().find((i) => !filters.length || filters.some((f) => matches(i, f)));
      if (!info) throw err("NotFoundError", "No device selected.");
      if (!(await call((cb) => N.usbRequestPermission(info.id, cb)))) throw err("NotFoundError", "No device selected.");
      return deviceFor({ ...info, granted: true });
    }

    _dispatch(type, device) {
      const e = new Event(type);
      Object.defineProperty(e, "device", { value: device });
      const handler = this["on" + type];
      if (typeof handler === "function") handler.call(this, e);
      this.dispatchEvent(e);
    }
  }
  const usb = new USB();
  Object.defineProperty(Navigator.prototype, "usb", { get: () => usb, configurable: true });

  // ---- Speech (lap callouts) ----
  if (!("speechSynthesis" in window)) {
    class Utterance extends EventTarget {
      constructor(text) {
        super();
        this.text = text ?? "";
        this.lang = "";
        this.voice = null;
        this.volume = 1;
        this.rate = 1;
        this.pitch = 1;
      }
    }
    class Synthesis extends EventTarget {
      get speaking() {
        return N.isSpeaking();
      }
      get pending() {
        return false;
      }
      get paused() {
        return false;
      }
      speak(u) {
        N.speak(String(u.text), +u.rate || 1, +u.pitch || 1, u.volume === undefined ? 1 : +u.volume, u.lang || "");
      }
      cancel() {
        N.stopSpeaking();
      }
      pause() {}
      resume() {}
      getVoices() {
        return [];
      }
    }
    window.SpeechSynthesisUtterance = Utterance;
    window.speechSynthesis = new Synthesis();
  }

  // ---- Screen wake lock: the app keeps the screen on while it is open ----
  if (!("wakeLock" in navigator)) {
    class Sentinel extends EventTarget {
      constructor() {
        super();
        this.type = "screen";
        this.released = false;
        this.onrelease = null;
      }
      async release() {
        if (this.released) return;
        this.released = true;
        const e = new Event("release");
        this.onrelease?.(e);
        this.dispatchEvent(e);
      }
    }
    const wakeLock = { request: async () => new Sentinel() };
    Object.defineProperty(Navigator.prototype, "wakeLock", { get: () => wakeLock, configurable: true });
  }

  // ---- Downloads (session recordings): WebView ignores <a download> ----
  async function save(a) {
    const name = a.download || "download";
    const blob = await (await fetch(a.href)).blob();
    N.saveFile(name, blob.type || "application/octet-stream", b64enc(new Uint8Array(await blob.arrayBuffer())));
  }
  const isDownload = (a) => a.hasAttribute("download") && /^(blob|data):/.test(a.href);
  const click = HTMLAnchorElement.prototype.click;
  HTMLAnchorElement.prototype.click = function () {
    if (isDownload(this)) {
      save(this).catch((e) => console.error("save failed:", e));
      return;
    }
    return click.call(this);
  };
  document.addEventListener(
    "click",
    (e) => {
      const a = e.target instanceof Element ? e.target.closest("a") : null;
      if (!a || !isDownload(a)) return;
      e.preventDefault();
      save(a).catch((x) => console.error("save failed:", x));
    },
    true,
  );

  // ---- Called by the app ----
  window.__widscan = {
    _cb(id, ok, result) {
      const p = pending.get(id);
      if (!p) return;
      pending.delete(id);
      if (ok) p.resolve(result);
      else p.reject(err("NetworkError", String(result)));
    },
    _in(id, ep, b64) {
      devices.get(id)?._push(ep, b64dec(b64));
    },
    _inEnd(id, ep) {
      devices.get(id)?._fail(err("NetworkError", "A transfer error has occurred."), ep);
    },
    _attached(info) {
      usb._dispatch("connect", deviceFor(info));
    },
    _detached(id) {
      const d = devices.get(id);
      if (!d) return;
      devices.delete(id);
      d._gone = true;
      d.opened = false;
      d._fail(err("NetworkError", "The device was disconnected."));
      usb._dispatch("disconnect", d);
    },
  };
})();
