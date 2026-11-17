# zsh-autosuggestions: as a line is typed, the rest of it offered in grey,
# from the history and then from completion, as fish does; the right arrow
# (or End) takes it. A zsh script (nothing is compiled), MIT, sourced by
# sqterm's zshrc (packages/zsh/zshrc) when it is installed; SQTERM_PLAIN=1
# in ~/.zshenv leaves it out.
#
#   share/zsh/plugins/zsh-autosuggestions/zsh-autosuggestions.zsh
#
# A git source (GitHub's tag archives are not a published file with a
# sha256): the tag's commit is in packages/sources.lock.

version=0.7.1
license="MIT"
git=https://github.com/zsh-users/zsh-autosuggestions
tag=v$version
file=
top=
depends=
keep="share/zsh/plugins/zsh-autosuggestions share/licenses/zsh-autosuggestions"

recipe_build() {
  local to=$dest$SQ_PREFIX/share/zsh/plugins/zsh-autosuggestions
  mkdir -p "$to" "$dest$SQ_PREFIX/share/licenses/zsh-autosuggestions"
  cp zsh-autosuggestions.zsh "$to/"
  cp LICENSE "$dest$SQ_PREFIX/share/licenses/zsh-autosuggestions/LICENSE"
}
