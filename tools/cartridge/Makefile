# sqterm-pack (pack.cpp), built for this machine with sqcart's library
# (third_party/sqcart, vendored: tools/vendor_sqcart.sh). Offline.
#
#   make -C tools/cartridge CXX=/usr/lib/llvm-23/bin/clang++ BUILD=...
#
# Needs clang and libc++ with `import std;` (23, as sqcart's CI).

CXX     ?= clang++
BUILD   ?= ../../build/packages/cartridge-tool
B       := $(abspath $(BUILD))
SQCART  := ../../third_party/sqcart/v3
CART    := $(B)/sqcart
LIBS    := $(CART)/libsqcart-write.a $(CART)/libsqcart.a $(CART)/libsqcart-zstd.a
PCMS    := $(CART)/pcm/sq.cart.pcm $(CART)/pcm/sq.cart.write.pcm
LDFLAGS ?= -stdlib=libc++ -lpthread

$(B)/sqterm-pack: pack.cpp $(LIBS) $(PCMS)
	$(CXX) -std=c++23 -stdlib=libc++ -O2 -Wall -Wextra -Wno-missing-designated-field-initializers -fprebuilt-module-path=$(CART)/pcm -fmodule-file=std=$(CART)/pcm/std.pcm \
	  $< $(LIBS) $(LDFLAGS) -o $@

$(LIBS) $(PCMS) &: $(wildcard $(SQCART)/src/*) $(SQCART)/Makefile
	$(MAKE) -C $(SQCART) CXX="$(CXX)" BUILD=$(CART) $(LIBS) $(PCMS)
