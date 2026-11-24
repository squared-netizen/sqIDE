# sqgen: the name an app's build once ran squared's layout compiler by. It is
# sqgui's sqgui-gen now (2026-11-24: pkg install sqgui), whose output is
# sqgen's, byte for byte; this package keeps the old name working, for a
# workspace whose sq/build/template.lua still runs `sqgen`:
#
#   bin/sqgen      a link to sqgui-gen (the sqgui package's; packages/needs)

version=0.2.0
license=MIT
file=
top=
depends=
inputs=
bundled=yes
keep="bin/sqgen"

recipe_build() {
  mkdir -p "$dest$SQ_PREFIX/bin"
  ln -sf sqgui-gen "$dest$SQ_PREFIX/bin/sqgen"
}
