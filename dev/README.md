# Development tools

Test tools used while adding CH347 support, partition identification and the
CH341A transfer pipeline. None of them needs a programmer or a flash chip,
and the test data is generated, so nothing here comes from a real device.
They are not part of the scriba build.

## Host tests (Linux)

```bash
dev/run-host-tests.sh [image.bin ...]
```

Builds with AddressSanitizer and UndefinedBehaviorSanitizer (gcc and the
libusb-1.0 headers) and runs:

- `mock/mock_ch347.c`: `src/ch347_spi.c` against a fake CH347 at the USB
  packet level. It covers 510-, 1024- and 4096-byte read frames, firmware
  versions on both sides of 0x0341 (chip-select flags), the
  `--no-cs-flags` option, and injected failures. The injected failures print
  driver errors such as `command 0xC4 failed`; those are expected. The last
  line says whether all checks passed.
- `layout/layout_cli.c` and `layout/layout_json.c`: `src/flash_layout.c` on
  the images from `layout/make_test_images.py` (empty, tiny, blank, random,
  odd and overflowing `mtdparts` strings, and a 4 MB camera-style flash with an Ingenic boot
  header, a CRC-checked U-Boot env, a uImage, squashfs and empty JFFS2). The
  JSON driver also tries every output buffer size. Pass your own dumps as
  arguments to include them.

## Web build against fake programmers

`web/fake_ch341a_webusb.js` and `web/fake_ch347_webusb.js` replace
`navigator.usb` with a fake programmer and a 2 MB W25Q16 behind it, so the
real WASM build runs end to end in the browser.
`web/run_programmer_test.js` connects, reads and compares the flash, then
erases, writes and verifies an image with blank pages, and reports the
transfer counts.

```bash
make -C web
dev/web/stage.sh            # copies the fakes and runner into web/dist/_t
cd web/dist && python3 -m http.server 8347 --bind 127.0.0.1
```

Open http://127.0.0.1:8347/ in Chrome and run in the console:

```js
eval(await (await fetch('/_t/run_programmer_test.js')).text());
await runProgrammerTest('/_t/fake_ch341a_webusb.js', '__ch341');
```

Reload and repeat with `'/_t/fake_ch347_webusb.js', '__ch347'`. Remove
`web/dist/_t` before deploying the build.

## Scripts

- `scripts/verify_branch.sh [BRANCH] [FROM]` builds and tests every commit
  in `FROM..BRANCH` on its own in a fresh clone: the CLI, `test.sh`, and the
  web build when an emsdk is found (`EMSDK_DIR`, default `~/emsdk`).
  `FROM` defaults to `origin/master`.
- `scripts/longmsgs.py`, run from the repo root, lists log and printf
  messages long enough to exceed the web log's 80-column line budget.
