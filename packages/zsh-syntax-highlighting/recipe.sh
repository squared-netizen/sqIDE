# zsh-syntax-highlighting: the command line coloured as it is typed, as fish
# does: a command that does not exist in red, one that does in green, paths
# underlined, strings and options in their own colours. A zsh script
# (nothing is compiled), BSD-3-Clause, sourced last by sqterm's zshrc
# (packages/zsh/zshrc) when it is installed; SQTERM_PLAIN=1 in ~/.zshenv
# leaves it out.
#
#   share/zsh/plugins/zsh-syntax-highlighting/zsh-syntax-highlighting.zsh
#   .../.version, .revision-hash          which it reads when it starts
#   .../highlighters/<name>/<name>-highlighter.zsh
#
# Not its tests or their data. A git source: the tag's commit is in
# packages/sources.lock.

version=0.8.0
license="BSD-3-Clause"
git=https://github.com/zsh-users/zsh-syntax-highlighting
tag=$version
file=
top=
depends=
keep="share/zsh/plugins/zsh-syntax-highlighting share/licenses/zsh-syntax-highlighting"

recipe_build() {
  local to=$dest$SQ_PREFIX/share/zsh/plugins/zsh-syntax-highlighting h
  mkdir -p "$to/highlighters" "$dest$SQ_PREFIX/share/licenses/zsh-syntax-highlighting"
  cp zsh-syntax-highlighting.zsh .version .revision-hash "$to/"
  for h in highlighters/*/; do
    h=$(basename "$h")
    mkdir -p "$to/highlighters/$h"
    cp "highlighters/$h/$h-highlighter.zsh" "$to/highlighters/$h/"
  done
  cp COPYING.md "$dest$SQ_PREFIX/share/licenses/zsh-syntax-highlighting/COPYING.md"
}
