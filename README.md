# td

## Build/install
```
```
sudo apt install libncurses-dev build-essential cmake
mkdur build && cd build && cmake .. && make && sudo make install

## First time setup
1. Create a free account at https://jsonbin.io and copy your Master Key from the dashboard
2. In the cn tab, press K to enter the key (saved to ~/.tb_console.json)
3. Press p to set an encryption password (held in memory only, never saved)

Syncing:
- u — encrypt + push both td and pb to jsonbin
- d — pull both down + decrypt (backs up existing files to .bak)
- U / D — push/pull just the selected entry
- j/k — move between entries

Between machines: copy ~/.tb_console.json to the other machine — it contains the bin IDs so both machines point to the same bins. The API key is also in there so you only need to re-enter the session password.
