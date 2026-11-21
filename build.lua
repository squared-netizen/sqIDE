-- build.lua: sqterm's build for sqmake (`sqmake`: build/sqterm.apk, signed
-- with ~/.android/apkpack.pem), the same app the Makefile builds:
-- `make apk BACKGROUND=1`. sq/build/template.lua (template.android.cpp's,
-- squared's to update) says every setting.
app = {
  name = "sqterm",
  background = true,
  -- pkg's, built by packages/pkg/recipe.sh with sqcart's reader; the app
  -- does not call it.
  exclude = {"sq_app/src/unpack_cartridge.cpp"},
  -- zstd's decoder, for the packages (12.5).
  flags = {"-Isq_app/third_party/zstd"},
}
include("sq/build/template.lua")
