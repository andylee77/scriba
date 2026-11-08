// Fake WebUSB CH341A with an emulated W25Q16 (2 MB SPI NOR) behind it.
// Injected into the scriba page to drive the real WASM build end to end.
// Models the CH341A's USB behaviour: OUT data is consumed in 32-byte
// packets; every SPI stream packet (0xA8 + up to 31 bit-reversed bytes) is
// answered with one IN packet holding a reply byte per data byte; UIO stream
// packets (0xAB) set the pins (CS0 = bit 0 of the state, low = selected).
// Several IN transfers may be pending; replies fill them in order.
(() => {
  const PKT = 32;
  const st = window.__ch341 = {
    mem: new Uint8Array(2 * 1024 * 1024).fill(0xFF),
    replies: [],                 // queued IN packets
    pending: [],                 // pending transferIn: {length, resolve}
    cs: false, mosi: [], clk: 0, wel: false,
    errors: [], outs: 0, ins: 0, outPackets: 0, maxPending: 0, babble: 0,
    ops: { pp: 0, se: 0, be: 0 },
  };
  const err = (m) => { st.errors.push(m); console.error('[fake-ch341a] ' + m); };
  const swap = (x) => {
    x = ((x >> 1) & 0x55) | ((x << 1) & 0xaa);
    x = ((x >> 2) & 0x33) | ((x << 2) & 0xcc);
    return ((x >> 4) & 0x0f) | ((x << 4) & 0xf0);
  };

  // --- flash emulation (acts on the bytes clocked in while CS was low) ---
  const HDR = { 0x9F: 1, 0x05: 1, 0x35: 1, 0x15: 1, 0x03: 4, 0x0B: 5, 0xAB: 4, 0x90: 4, 0x5A: 5 };
  function misoAt(pos) {
    const h = HDR[st.mosi[0]] ?? 1;
    return pos < h ? 0xFF : misoByte(pos - h);
  }
  function misoByte(i) {
    const m = st.mosi, op = m[0];
    const a3 = () => (m[1] << 16) | (m[2] << 8) | m[3];
    switch (op) {
      case 0x9F: return [0xEF, 0x40, 0x15][i] ?? 0x00;
      case 0x05: return st.wel ? 0x02 : 0x00;
      case 0x35: case 0x15: return 0x00;
      case 0x03: case 0x0B: return st.mem[(a3() + i) & (st.mem.length - 1)];
      case 0xAB: return 0x14;
      default: return 0xFF;
    }
  }
  function endTransaction() {
    const m = st.mosi, op = m[0];
    const a3 = () => (m[1] << 16) | (m[2] << 8) | m[3];
    const needWel = (name) => { if (!st.wel) err(name + ' without WEL'); };
    switch (op) {
      case 0x06: st.wel = true; break;
      case 0x04: st.wel = false; break;
      case 0x01: st.wel = false; break;
      case 0x02: {
        needWel('PP');
        const a = a3(), base = a & ~0xFF;
        for (let i = 4; i < m.length; i++) st.mem[base + ((a + i - 4) & 0xFF)] &= m[i];
        st.ops.pp++; st.wel = false; break;
      }
      case 0x20: case 0x52: case 0xD8: {
        needWel('erase');
        const size = op === 0x20 ? 4096 : op === 0x52 ? 32768 : 65536;
        const a = a3() & ~(size - 1);
        st.mem.fill(0xFF, a, a + size);
        st.ops.se++; st.wel = false; break;
      }
      case 0x60: case 0xC7:
        needWel('chip erase'); st.mem.fill(0xFF); st.ops.be++; st.wel = false; break;
    }
  }

  function reply(bytes) {
    st.replies.push(bytes);
    flush();
  }
  function flush() {                       // fill pending IN transfers in order
    while (st.pending.length && st.replies.length) {
      const t = st.pending.shift(), p = st.replies.shift();
      if (p.length > t.length) { st.babble++; t.resolve({ status: 'babble', data: new DataView(new ArrayBuffer(0)) }); }
      else t.resolve({ status: 'ok', data: new DataView(p.buffer, p.byteOffset, p.length) });
    }
  }

  function setCs(low) {
    if (low && !st.cs) { st.mosi = []; st.clk = 0; }
    if (!low && st.cs) endTransaction();
    st.cs = low;
  }

  function onPacket(p) {
    st.outPackets++;
    switch (p[0]) {
      case 0x00: break;                              // the native path's leading zero packet
      case 0xAA: break;                              // I2C stream: speed config
      case 0xAB:                                     // UIO stream: pins
        for (let i = 1; i < p.length; i++) {
          const c = p[i];
          if (c === 0x20) break;                     // END
          if ((c & 0xC0) === 0x80) setCs(!(c & 0x01));
        }
        break;
      case 0xA8: {                                   // SPI stream
        if (!st.cs) err('SPI stream with CS high');
        const out = new Uint8Array(p.length - 1);
        for (let i = 1; i < p.length; i++) {
          st.mosi.push(swap(p[i]));
          out[i - 1] = swap(misoAt(st.clk++));
        }
        reply(out);
        break;
      }
      default: err('unknown packet 0x' + p[0].toString(16));
    }
  }

  const dev = {
    vendorId: 0x1A86, productId: 0x5512, opened: false,
    deviceVersionMajor: 3, deviceVersionMinor: 0, deviceVersionSubminor: 4,
    productName: 'Fake CH341A', configuration: { configurationValue: 1 }, configurations: [{}],
    async open() { this.opened = true; },
    async close() { this.opened = false; },
    async selectConfiguration() {},
    async claimInterface(n) { if (n !== 0) err('claimInterface ' + n); },
    async releaseInterface() {},
    async clearHalt() {},
    async reset() {},
    async controlTransferIn() { return { status: 'ok', data: new DataView(new ArrayBuffer(0)) }; },
    async controlTransferOut(s, d) { return { status: 'ok', bytesWritten: d ? d.byteLength : 0 }; },
    async transferOut(ep, data) {
      st.outs++;
      if (ep !== 2) err('OUT on ep ' + ep);
      const b = new Uint8Array(data.buffer || data, data.byteOffset || 0, data.byteLength);
      for (let o = 0; o < b.length; o += PKT) onPacket(b.slice(o, Math.min(o + PKT, b.length)));
      return { status: 'ok', bytesWritten: b.length };
    },
    transferIn(ep, length) {
      st.ins++;
      if (ep !== 2) err('IN on ep ' + ep);
      return new Promise((resolve) => {
        st.pending.push({ length, resolve });
        st.maxPending = Math.max(st.maxPending, st.pending.length);
        flush();
      });
    },
  };

  Object.defineProperty(navigator, 'usb', { configurable: true, value: {
    requestDevice: async () => dev,
    getDevices: async () => [dev],
    addEventListener() {}, removeEventListener() {},
  }});
  st.dev = dev;
  return 'fake CH341A installed';
})();
