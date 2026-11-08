// Fake WebUSB CH347T (mode 1) with an emulated W25Q16 (2 MB SPI NOR) behind it.
// Injected into the scriba page to drive the real WASM build end to end.
(() => {
  const PKT = 512;
  const st = window.__ch347 = {
    framePayload: 507,          // data bytes per read frame (507 = 2022 fw, 4093 = 4 KB frames)
    firmver: 0x0544,            // reported by the 0xCA chip-info read (CS flags from 0x0341)
    mem: new Uint8Array(2 * 1024 * 1024).fill(0xFF),
    q: [],                       // queued IN packets (Uint8Array <= 512)
    cs: false, mosi: [], readPos: 0, wel: false,
    errors: [], stalls: 0, overflows: 0, outs: 0, ins: 0,
    claimed: [], claimRejected: [],
    ops: { pp: 0, se: 0, be: 0, wrsr: 0 },
  };
  const err = (m) => { st.errors.push(m); console.error('[fake-ch347] ' + m); };

  function queueFrame(bytes) {           // split a device frame into packets, no ZLP
    for (let o = 0; o < bytes.length; o += PKT) st.q.push(bytes.slice(o, o + PKT));
  }
  const status = (cmd) => queueFrame(Uint8Array.of(cmd, 1, 0, 0));

  // --- flash emulation (acts on the bytes clocked in while CS was low) ---
  // Bytes clocked before the data phase (opcode + address/dummy) read as 0xFF.
  const HDR = { 0x9F: 1, 0x05: 1, 0x35: 1, 0x15: 1, 0x03: 4, 0x0B: 5, 0xAB: 4, 0x90: 4, 0x5A: 5 };
  function misoAt(pos) {                 // pos = byte index since CS went low
    const h = HDR[st.mosi[0]] ?? 1;
    return pos < h ? 0xFF : misoByte(pos - h);
  }
  function misoByte(i) {                 // i = byte index within the data phase
    const m = st.mosi, op = m[0];
    const a3 = () => (m[1] << 16) | (m[2] << 8) | m[3];
    switch (op) {
      case 0x9F: return [0xEF, 0x40, 0x15][i] ?? 0x00;
      case 0x05: return st.wel ? 0x02 : 0x00;
      case 0x35: case 0x15: return 0x00;
      case 0x03: return st.mem[(a3() + i) & (st.mem.length - 1)];
      case 0x0B: return st.mem[(a3() + i) & (st.mem.length - 1)];
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
      case 0x01: st.ops.wrsr++; st.wel = false; break;
      case 0x02: {                        // page program: AND into the page, wrap at 256
        needWel('PP');
        const a = a3(), base = a & ~0xFF;
        for (let i = 4; i < m.length; i++) {
          const o = base + ((a + i - 4) & 0xFF);
          st.mem[o] &= m[i];
        }
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

  // --- CH347 command handling ---
  // One OUT transfer may carry several commands back to back (batching);
  // st.firstOnly models firmware that runs only the first of them.
  function onOut(t) {
    st.outs++;
    if (t.length > 510) err('OUT transfer ' + t.length + ' > 510');
    for (let off = 0; off < t.length; ) {
      if (t.length - off < 3) return err('runt command');
      // firmware 0341+ reads the top nibble of a C2 length as CS flags
      const hi = (t[off] === 0xC2 && st.firmver >= 0x0341) ? (t[off + 2] & 0x0F) : t[off + 2];
      const len = t[off + 1] | (hi << 8);
      if (off + 3 + len > t.length) return err('command overruns transfer');
      onCmd(t.subarray(off, off + 3 + len));
      off += 3 + len;
      if (st.firstOnly) break;
    }
  }

  function onCmd(b) {
    const len = b[1] | (b[2] << 8);
    st.cmds = st.cmds || {};
    st.cmds[b[0]] = (st.cmds[b[0]] || 0) + 1;
    switch (b[0]) {
      case 0xC0: st.divisors = (st.divisors || []).concat((b[15] >> 3) & 7); status(0xC0); break;
      case 0xC1:
        if (b[3] & 0x80) {
          const low = !(b[3] & 0x40);
          if (low && !st.cs) { st.mosi = []; st.clk = 0; }
          if (!low && st.cs) endTransaction();
          st.cs = low;
        }
        break;
      case 0xC4:
        if (!st.cs) err('write with CS high');
        for (let i = 3; i < b.length; i++) { st.mosi.push(b[i]); st.clk++; }
        status(0xC4); break;
      case 0xCA:                          // chip info: firmware version in bytes 3..4
        queueFrame(Uint8Array.of(0xCA, 4, 0, st.firmver & 0xFF, st.firmver >> 8, 0, 0));
        break;
      case 0xC2: {                        // full duplex: MOSI in, MISO out, same length
        const flags = st.firmver >= 0x0341 ? (b[2] & 0xC0) : 0;
        st.flagged = (st.flagged || 0) + (flags ? 1 : 0);
        if (flags & 0x80) {               // assert CS0 before the frame
          if (!st.cs) { st.mosi = []; st.clk = 0; }
          st.cs = true;
        }
        const total = b.length - 3;
        const miso = new Uint8Array(total).fill(0xFF);
        if (!st.cs) err('write+read with CS high');
        else for (let i = 0; i < total; i++) { st.mosi.push(b[3 + i]); miso[i] = misoAt(st.clk++); }
        for (let o = 0; o < total; o += st.framePayload) {
          const n = Math.min(st.framePayload, total - o);
          const f = new Uint8Array(n + 3);
          f[0] = 0xC2; f[1] = n & 0xFF; f[2] = (n >> 8) | flags;   // echo the flags, as some firmware may
          f.set(miso.subarray(o, o + n), 3);
          queueFrame(f);
        }
        if ((flags & 0x40) && st.cs) {     // release CS0 after the frame
          endTransaction();
          st.cs = false;
        }
        break;
      }
      case 0xC3: {
        if (!st.cs) err('read with CS high');
        let cnt = b[3] | (b[4] << 8) | (b[5] << 16) | (b[6] << 24 >>> 0);
        while (cnt > 0) {
          const n = Math.min(cnt, st.framePayload);
          const f = new Uint8Array(n + 3);
          f[0] = 0xC3; f[1] = n & 0xFF; f[2] = n >> 8;
          for (let i = 0; i < n; i++) f[3 + i] = misoAt(st.clk++);
          queueFrame(f);
          cnt -= n;
        }
        break;
      }
      default: err('unknown cmd 0x' + b[0].toString(16));
    }
  }

  const dev = {
    vendorId: 0x1A86, productId: 0x55DB, opened: false,
    productName: 'Fake CH347T', configuration: { configurationValue: 1 }, configurations: [{}],
    async open() { this.opened = true; },
    async close() { this.opened = false; },
    async selectConfiguration() {},
    async claimInterface(n) {
      if (n !== 2) { st.claimRejected.push(n); throw new DOMException('Unable to claim interface ' + n + ' (owned by the serial driver)', 'NetworkError'); }
      st.claimed.push(n);
    },
    async releaseInterface() {},
    async clearHalt() {},
    async reset() {},
    async transferOut(ep, data) {
      if (ep !== 6) err('OUT on ep ' + ep);
      onOut(new Uint8Array(data));
      return { status: 'ok', bytesWritten: data.byteLength };
    },
    transferIn(ep, length) {
      st.ins++;
      if (ep !== 6) err('IN on ep ' + ep);
      const parts = []; let got = 0;
      for (;;) {
        if (st.q.length === 0) {
          if (got > 0 && got % PKT !== 0) break;
          st.stalls++;                      // host waits for data that never comes
          return new Promise(() => {});
        }
        const p = st.q[0];
        if (p.length > length - got) { st.overflows++; st.q.shift(); return Promise.resolve({ status: 'babble', data: new DataView(new ArrayBuffer(0)) }); }
        st.q.shift(); parts.push(p); got += p.length;
        if (p.length < PKT || got === length) break;
      }
      const out = new Uint8Array(got); let o = 0;
      for (const p of parts) { out.set(p, o); o += p.length; }
      return Promise.resolve({ status: 'ok', data: new DataView(out.buffer) });
    },
  };

  Object.defineProperty(navigator, 'usb', { configurable: true, value: {
    requestDevice: async () => dev,
    getDevices: async () => [dev],
    addEventListener() {}, removeEventListener() {},
  }});
  st.dev = dev;
  return 'fake CH347 installed';
})();
