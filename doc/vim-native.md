# Full Vim editing in DB Browser for SQLite

The Windows x64 portable ZIP bundles Neovim 0.12.5 and its runtime under `nvim/`.
Keep that directory beside DB Browser's executable. In Edit > Preferences > SQL,
enable **Vim editing mode** and **Full Vim engine**. The editor status shows `NVIM`.
Disable Full Vim engine to return to the basic built-in emulator.

Native commands include J/gJ, . repeat, f/F/t/T, ;/comma, */#, >>/<<, =,
X/S/R, Ctrl+V block selection, gv, visual o, gu/gU, Ctrl+D/U/F/B,
zz/zt/zb, paragraph/sentence/screen motions, marks, jump history,
macros, named registers, :s / :%s / :g, and Vim regex search.
These use the Neovim engine, not reimplemented approximations.

vim-surround is bundled: ysiw", yss), cs"', ds), and visual S.
Opening bracket targets add padding; closing bracket targets do not.
Your default leader is comma. The comma leader mappings and reverse-find comma
share Neovim's normal mapping timeout rules (adjust `timeoutlen` if desired).

## Configuration

Hover over the NVIM status label to see the exact configuration directory.
It is Qt's application configuration location with `/nvim` appended.
Create `init.lua` there, or `init.vim` if you prefer Vimscript. `init.lua` takes
precedence. Reload by disabling/re-enabling Full Vim engine or restarting DB Browser.
Your existing standalone Neovim config is not automatically executed.

For a predictable custom location, set `DB4S_NVIM_CONFIG` to a directory before
starting DB Browser. Example PowerShell:

```powershell
$env:DB4S_NVIM_CONFIG = "$env:USERPROFILE\db4s-nvim"
New-Item -ItemType Directory -Force $env:DB4S_NVIM_CONFIG
notepad "$env:DB4S_NVIM_CONFIG\init.lua"
& '.\DB Browser for SQLite.exe'
```

Example init.lua:

```lua
vim.opt.shiftwidth = 2
vim.opt.expandtab = true
vim.opt.ignorecase = true
vim.opt.smartcase = true
vim.opt.timeoutlen = 400
vim.keymap.set('n', '<leader>xs', '<Cmd>write<CR>')
```

The defaults include `,,`, zh/zl/z;/z,, leader ss/xs/ci, and visual leader aa.
User settings load after defaults. If copying an existing init.lua, also provide
any Lua modules or plugins it requires. Plugin installation is not automatic.
To use another Neovim executable, set `DB4S_NVIM` to its full path.

## Integration boundaries

- Each SQL editor has a separate Neovim process and buffer. Registers, macros,
  marks and undo history are local to that editor; the system clipboard is shared.
- The SQL widget displays the current Neovim buffer, selection and command line.
  It is not a full terminal renderer: split layouts, terminal applications,
  floating plugin windows and completion popups are not rendered as Neovim UIs.
- Ctrl+Enter keeps executing SQL; Ctrl+S keeps the application's Save action.
  Other Ctrl commands go to Neovim while its editor has focus.
- `:write` in the original SQL buffer invokes the application's Save SQL action.
  File management through :edit/buffer switching is not integrated with DB Browser
  tab names. Use DB Browser tabs and Open SQL for file management.
- Buffer text, cursor, visual selection, and viewport are synchronized. Syntax
  colors and fonts still come from DB Browser. Neovim highlight/search decorations
  are not painted by the SQL widget.
- If Neovim exits, the last synchronized SQL text remains and the basic emulator
  becomes available. A restart resets that editor's Neovim undo/macro history.
- The x86 ZIP does not bundle Neovim (upstream supplies x64/ARM64 Windows builds).
  Its default fallback is the basic emulator, with the smaller supported command set.

## Third-party components

Neovim: https://github.com/neovim/neovim/releases/tag/v0.12.5
(Neovim Apache-2.0 and inherited Vim license files are retained in its distribution.)

vim-surround: Tim Pope, https://github.com/tpope/vim-surround
Vendored plugin blob: 8a4016e9101001fa37ae5e511fb45466b9d016f7.
Its header and license notices are retained in src/vim/surround.vim;
vim-surround is distributed under the same terms as Vim.
