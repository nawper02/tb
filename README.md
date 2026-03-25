# tb — dev toolbox

Terminal app with tabbed tools: todo list, pastebin, automation engine, notes, and sync console.

## Build & install

```
sudo apt install build-essential cmake pkg-config libncurses-dev xclip curl openssl
mkdir build && cd build && cmake .. && make && sudo make install
```

## Apps

- **td** — todo list with priorities, tags, notes, subtasks, folders
- **pb** — pastebin / snippet manager
- **ae** — automation engine (record & replay terminal sessions)
- **nt** — notes with text paragraphs and copyable blocks
- **cn** — sync console (encrypt & sync td/pb between machines)

Switch tabs with `Shift+Left / Shift+Right`.

## Sync setup (cn tab)

Syncs `~/.td.json`, `~/.pb.json`, `~/.ae.json`, and `~/.nt.json` encrypted via AES-256-CBC. Requires `openssl` and `curl`.

1. Go to [kvdb.io](https://kvdb.io), create a free account, and **verify your email** — then create a bucket and you'll get a short ID
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
