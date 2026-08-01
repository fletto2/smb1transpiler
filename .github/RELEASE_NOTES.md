Standalone builds. No installer and no runtime to fetch.

Put the binary in a folder with your own Super Mario Bros `.nes` and Super
Mario All-Stars `.sfc`, and run it there. It writes `out/smb1_vera.dsk` and
verifies its own CRC32 (`8875B7F8`).

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

This release contains no Nintendo data. You supply the ROMs.
