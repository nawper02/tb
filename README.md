# tb — dev toolbox

Terminal app with three tabs: todo list, pastebin, and sync console.

## Build & install

```
sudo apt install libncurses-dev build-essential cmake
mkdir build && cd build && cmake .. && make && sudo make install
```

## Apps

- **td** — todo list
- **pb** — pastebin / snippet manager
- **cn** — sync console (encrypt & sync td/pb between machines)

Switch tabs with `Shift+Left / Shift+Right`.

## Sync setup (cn tab)

Syncs `~/.td.json` and `~/.pb.json` encrypted via AES-256-CBC. Requires `openssl` and `curl`.

1. Go to [kvdb.io](https://kvdb.io) and create a free bucket — you get a short ID
2. Press `K` in the cn tab and enter that bucket ID (saved to `~/.tb_console.json`)
3. Press `p` to set an encryption password (session-only, never saved to disk)
4. Press `u` to push, `d` to pull

On any other machine: enter the **same bucket ID** and **same password** — that's all. Nothing to copy between machines.

| Key | Action |
|-----|--------|
| `u` | Encrypt + push all |
| `d` | Pull + decrypt all |
| `U` / `D` | Push / pull selected entry |
| `K` | Set bucket ID |
| `p` | Set encryption password |
