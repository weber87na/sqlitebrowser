local channel, config = ...
vim.g.mapleader = ','
vim.opt.swapfile = false
vim.opt.undofile = false
vim.opt.hidden = true
vim.opt.joinspaces = false
vim.opt.termguicolors = true
vim.opt.mouse = ''
vim.opt.number = false
vim.opt.relativenumber = false
vim.opt.signcolumn = 'no'
vim.opt.foldcolumn = '0'
vim.opt.wrap = false
vim.opt.shortmess:append('I')
vim.opt.clipboard = 'unnamedplus'
vim.g.db4s_clipboard = ''
vim.g.db4s_clipboard_kind = 'v'
vim.g.clipboard = {
  name = 'DB Browser clipboard', cache_enabled = 0,
  copy = {
    ['+'] = function(lines, kind) vim.g.db4s_clipboard=table.concat(lines, '\n'); vim.g.db4s_clipboard_kind=kind; vim.rpcnotify(channel, 'db4s_clipboard', lines, kind) end,
    ['*'] = function(lines, kind) vim.g.db4s_clipboard=table.concat(lines, '\n'); vim.g.db4s_clipboard_kind=kind; vim.rpcnotify(channel, 'db4s_clipboard', lines, kind) end,
  },
  paste = {
    ['+'] = function() return {vim.split(vim.g.db4s_clipboard, '\n', {plain=true}), vim.g.db4s_clipboard_kind} end,
    ['*'] = function() return {vim.split(vim.g.db4s_clipboard, '\n', {plain=true}), vim.g.db4s_clipboard_kind} end,
  },
}
local map = vim.keymap.set
map({'i', 'n', 'x'}, ',,', '<Esc>', {silent=true})
map('i', 'z;', '<Esc>$a;')
map('i', 'z,', '<Esc>$a,')
map('i', 'zh', '<Esc>^i')
map('i', 'zl', '<Esc>$a')
map('n', 'zh', '^')
map('n', 'zl', '$')
map('n', 'z;', '$a;<Esc>')
map('n', 'z,', '$a,<Esc>')
map({'n','x'}, ',ss', '/')
map('n', ',xs', '<Cmd>write<CR>')
map('n', ',xm', ':')
map('n', ',ci', 'gcc', {remap=true})
map('x', ',ci', 'gc', {remap=true})
map('x', ',aa', '"+y')
vim.cmd('filetype plugin indent on')
vim.bo.filetype = 'sql'
vim.bo.commentstring = '-- %s'
vim.bo.buftype = 'acwrite'
vim.api.nvim_buf_set_name(0, 'DBBrowser-query.sql')
vim.api.nvim_create_autocmd('BufWriteCmd', {
  buffer=0, callback=function() vim.rpcnotify(channel, 'db4s_save') end,
})
-- Config is specific to DB Browser. It is never silently copied from the
-- user's main Neovim configuration (which may depend on a terminal GUI).
local init = config .. '/init.lua'
local vimrc = config .. '/init.vim'
if vim.fn.filereadable(init) == 1 then
  local ok, err = pcall(dofile, init)
  if not ok then vim.rpcnotify(channel, 'db4s_error', tostring(err)) end
elseif vim.fn.filereadable(vimrc) == 1 then
  local ok, err = pcall(vim.cmd.source, vimrc)
  if not ok then vim.rpcnotify(channel, 'db4s_error', tostring(err)) end
end
local last_tick, last_buf = -1, -1
_G.db4s_snapshot = function()
  local buf = vim.api.nvim_get_current_buf()
  local tick = vim.api.nvim_buf_get_changedtick(buf)
  local lines = false
  if tick ~= last_tick or buf ~= last_buf then
    lines = vim.api.nvim_buf_get_lines(buf, 0, -1, true)
    last_tick, last_buf = tick, buf
  end
  local cursor = vim.api.nvim_win_get_cursor(0)
  local anchor = vim.fn.getpos('v')
  return {lines=lines, eol=vim.bo.endofline, mode=vim.fn.mode(1),
    cursor=cursor, anchor={anchor[2], math.max(0, anchor[3]-1)},
    top=vim.fn.line('w0'), recording=vim.fn.reg_recording(),
    modified=vim.bo.modified}
end
return true
