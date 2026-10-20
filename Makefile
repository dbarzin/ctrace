# llvm-config, sinon la version suffixée la plus récente (llvm-config-19, ...)
LLVM_CONFIG ?= $(or $(shell command -v llvm-config 2>/dev/null),\
                    $(lastword $(sort $(wildcard /usr/bin/llvm-config-[0-9]*))))
ifeq ($(LLVM_CONFIG),)
$(error llvm-config introuvable : installez llvm-NN-dev et libclang-NN-dev, ou passez LLVM_CONFIG=...)
endif
CLANG       ?= $(shell $(LLVM_CONFIG) --bindir)/clang

CXXFLAGS := $(shell $(LLVM_CONFIG) --cxxflags) -fexceptions -O1 -g0 \
            -DCTRACE_RESOURCE_DIR='"$(shell $(CLANG) -print-resource-dir)"'
LIBDIR    := $(shell $(LLVM_CONFIG) --libdir)
# libclang-cpp.so si le paquet -dev fournit le lien, sinon la version exacte
CLANG_CPP := $(firstword $(wildcard $(LIBDIR)/libclang-cpp.so) $(wildcard $(LIBDIR)/libclang-cpp.so.*))
LDFLAGS   := $(shell $(LLVM_CONFIG) --ldflags) -Wl,-rpath,$(LIBDIR)
LDLIBS    := $(CLANG_CPP) $(shell $(LLVM_CONFIG) --libs) $(shell $(LLVM_CONFIG) --system-libs)

PREFIX ?= /usr/local

ctrace: ctrace.cpp ctrace_runtime.inc
	$(CXX) $(CXXFLAGS) ctrace.cpp -o $@ $(LDFLAGS) $(LDLIBS)

test: ctrace
	./ctrace examples/demo.c | gcc -Wall -x c - -o examples/demo
	./examples/demo

install: ctrace
	install -m 755 ctrace ctrace-cc $(DESTDIR)$(PREFIX)/bin/

clean:
	rm -f ctrace examples/demo

.PHONY: test install clean
