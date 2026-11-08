// End-to-end test of the web build against a fake WebUSB programmer
// (fake_ch341a_webusb.js or fake_ch347_webusb.js): connect, read the whole
// 2 MB fake flash and compare it, then load an image with blank pages and
// run the page's erase + write + verify, comparing the fake flash after.
// Test data is generated, so no image files are needed.
//
// In the browser console of a served web build (see dev/README.md):
//   eval(await (await fetch('/_t/run_programmer_test.js')).text());
//   await runProgrammerTest('/_t/fake_ch341a_webusb.js', '__ch341');
//   await runProgrammerTest('/_t/fake_ch347_webusb.js', '__ch347');
// Reload the page between runs: each run installs its fake as navigator.usb.
async function runProgrammerTest(fakeUrl, fakeVar) {
  const logText = () => document.getElementById('log').innerText;
  for (let i = 0; i < 80 && !logText().includes('Ready'); i++) await new Promise(r => setTimeout(r, 100));
  eval(await (await fetch(fakeUrl + '?' + Date.now())).text());
  const s = window[fakeVar];

  // xorshift32 bytes: deterministic, and dense like compressed firmware
  const bytes = (n, seed) => {
    const a = new Uint8Array(n);
    let x = seed >>> 0 || 1;
    for (let i = 0; i < n; i++) { x ^= x << 13; x >>>= 0; x ^= x >>> 17; x ^= x << 5; x >>>= 0; a[i] = x & 0xFF; }
    return a;
  };
  s.mem.set(bytes(s.mem.length, 1));

  window.confirm = () => true;
  let blob = null;                      // capture the read's download
  URL.createObjectURL = (b) => { blob = b; return 'blob:fake'; };
  URL.revokeObjectURL = () => {};
  HTMLAnchorElement.prototype.click = function () {};

  await window.connectDevice();
  const chip = document.getElementById('info-chip').textContent;
  const programmer = document.getElementById('info-programmer').textContent;
  const reset = () => {
    s.outs = 0; s.ins = 0;
    s.ops = { pp: 0, se: 0, be: 0, wrsr: 0 };
  };

  reset();
  let t0 = performance.now();
  await window.doRead();
  const readMs = Math.round(performance.now() - t0);
  const got = blob ? new Uint8Array(await blob.arrayBuffer()) : new Uint8Array(0);
  let readOk = got.length === s.mem.length;
  for (let i = 0; readOk && i < got.length; i++) if (got[i] !== s.mem[i]) readOk = false;
  const read = { ms: readMs, matchesFlash: readOk, outTransfers: s.outs, inTransfers: s.ins };

  // image: dense data with every 4th 64 KB block blank
  const fw = bytes(s.mem.length, 2);
  for (let b = 0; b < fw.length; b += 0x40000) fw.fill(0xFF, b, b + 0x10000);
  let blankPages = 0;
  for (let o = 0; o < fw.length; o += 256) blankPages += fw.subarray(o, o + 256).every(v => v === 0xFF);
  const dt = new DataTransfer();
  dt.items.add(new File([fw], 'fake-fw.bin'));
  const input = document.getElementById('firmware-file');
  input.files = dt.files;
  window.firmwareSelected(input);
  for (let i = 0; i < 50 && !logText().includes('fake-fw.bin'); i++) await new Promise(r => setTimeout(r, 100));

  reset();
  t0 = performance.now();
  await window.doWrite();
  const writeMs = Math.round(performance.now() - t0);
  let writeOk = true;
  for (let i = 0; writeOk && i < fw.length; i++) if (s.mem[i] !== fw[i]) writeOk = false;
  const lines = logText().split('\n');
  return {
    chip, programmer, read,
    write: {
      ms: writeMs, flashMatchesImage: writeOk, pagesProgrammed: s.ops.pp, blankPages,
      chipErases: s.ops.be, outTransfers: s.outs, inTransfers: s.ins,
      log: lines.filter(l => /Verify|Firmware written|failed|error/i.test(l)).slice(-3),
    },
    fakeErrors: s.errors.slice(0, 5),
  };
}
