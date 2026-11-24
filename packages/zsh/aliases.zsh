# sqterm's aliases ($PREFIX/etc/zsh/aliases.zsh): read by sqterm's zshrc for
# every interactive zsh, before your ~/.zshrc, which can change or remove any
# of them (`unalias ll`, or `alias ll='...'`). Part of the zsh package: an
# update writes it again, so keep your own in ~/.zshrc.
#
# `alias` lists them all; `which ll` says what one is.

# --- listing: sqls (sqterm-tools), else ls ------------------------------------
if (( $+commands[sqls] )); then
  alias ls='sqls'                # icons, colours, folders first
  alias l='sqls -1'              # one a line
  alias ll='sqls -l'             # long: kind, size, when, link targets
  alias la='sqls -A'             # dotfiles too
  alias lla='sqls -lA'           # long, dotfiles too
  alias lt='sqls --tree'         # the folders under here, as a tree
  alias lt2='sqls --tree -L 2'   # two levels of it
  alias lt3='sqls --tree -L 3'
  alias lnew='sqls -lt'          # newest first
  alias lbig='sqls -lS'          # largest first
else
  alias ls='ls --color=auto'
  alias l='ls -1'
  alias ll='ls -lh'
  alias la='ls -A'
  alias lla='ls -lhA'
  alias lnew='ls -lht'
  alias lbig='ls -lhS'
fi

# --- the screen and the shell ---------------------------------------------------
alias cls='clear'
alias c='clear'
alias reload='exec zsh'          # start the shell again, rereading its files
alias h='history -20'            # the last 20 commands
alias hg='history 1 | grep -i'   # search all of it: hg make
alias path='print -rl -- $path'  # the PATH, a folder a line
alias j='jobs -l'

# --- moving around --------------------------------------------------------------
alias ..='cd ..'
alias ...='cd ../..'
alias ....='cd ../../..'
alias -- -='cd -'                # back where you were
alias d='dirs -v'                # the folders cd has been to (cd -2 goes back two)

# --- files ------------------------------------------------------------------------
alias md='mkdir -p'
alias rd='rmdir'
alias cp='cp -i'                 # ask before replacing a file
alias mv='mv -i'
alias df='df -h'
alias du='du -h'
alias du1='du -h -d 1'           # each folder here, and its size
alias grep='grep --color=auto'

# --- the SDK ------------------------------------------------------------------------
alias sm='sqmake'
alias smr='sqmake run'
alias smt='sqmake test'
alias smd='sqmake --debug'
alias sq='square'
alias sqi='square info'
alias sqv='square verify'

# --- git, when installed ----------------------------------------------------------
if (( $+commands[git] )); then
  alias g='git'
  alias gs='git status -sb'
  alias ga='git add'
  alias gaa='git add -A'
  alias gc='git commit'
  alias gcm='git commit -m'
  alias gd='git diff'
  alias gds='git diff --staged'
  alias gl='git log --oneline --graph --decorate -20'
  alias gp='git push'
  alias gpl='git pull'
  alias gco='git checkout'
  alias gb='git branch'
fi

# --- an editor, when installed ------------------------------------------------------
if (( $+commands[nvim] )); then
  alias v='nvim'
  alias vi='nvim'
  alias vim='nvim'
fi
