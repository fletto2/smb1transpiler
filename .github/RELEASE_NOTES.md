Standalone builds. No installer and no runtime to fetch.

Put the binary in a folder with your own Super Mario Bros `.nes` and Super
Mario All-Stars `.sfc`, and run it there. It writes three disk images and
verifies the CRC32 of each:

| file | for | CRC32 |
| --- | --- | --- |
| `out/smb1_vera.dsk` | Apple II, 140K 5.25" | `8875B7F8` |
| `out/smb1_vera.po` | Apple II, 800K ProDOS | `A15C62CB` |
| `out/smb1_vera.d64` | Commodore 64 + VERA, 1541 | `23A2D7FC` |

The `.po` and the `.d64` both carry the same game as the `.dsk`. The C64
payload and APU divide table come out byte-identical, and its VERA upload is
the same seven chunks in a different frame, so neither is a separate build that
could drift away from the other two.

This release fixes a bug in the `.d64` directory. Filenames shorter than the
16-byte field were padded from whatever followed the name in memory, so each
entry carried a few stray bytes of the next name or of the disk title. Files
read back correctly either way, which is why it went unnoticed, but a drive
listing the directory shows the stray bytes, and two compilers did not produce
the same disk. If you built a `.d64` with v1.1 or v1.2, build it again.

The `.d64` does not reproduce the disk images shipped from the C64 port's own
tree byte for byte, because those were written by many rounds of `c1541`
delete-and-rewrite and their sector allocation records that history rather than
any layout rule. Every file on it is byte-identical to the ones on those disks,
and it boots.

**Windows** — `smb1transpiler-windows-x64.exe` links the CRT statically, so it
needs nothing but the OS. It imports `KERNEL32` and the `api-ms-win-crt-*`
forwarders, which are part of Windows 10 and 11; on Windows 7 or 8.1 you would
need the Universal C Runtime update.

**macOS** — the binary is unsigned, so Gatekeeper quarantines anything
downloaded from a browser and the first run fails with "cannot be opened
because the developer cannot be verified". Clear the quarantine flag:

```sh
xattr -d com.apple.quarantine smb1transpiler-macos
chmod +x smb1transpiler-macos
```

The C64 image carries Krill's drive loader, which is 3-clause BSD; see LICENSE.

This release contains no Nintendo data. You supply the ROMs.
