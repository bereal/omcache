LIBDIR ?= /usr/lib
BASE=$(DESTDIR)$(LIBDIR)/node_modules/
#/opt/f-secure/fsio-nodejs/lib/node_modules/
OMCACHE_BIN:=./build/Release/omcache.node

NODEJS_PACKAGE ?= nodejs
export DEB_DH_GENCONTROL_ARGS_ALL = -- -Vmisc:Depends=$(NODEJS_PACKAGE)
export DEB_DH_INSTALL_ARGS := $(BASE)/omcache.node

omcache: $(OMCACHE_BIN)

$(OMCACHE_BIN): build/binding.Makefile
	node-gyp build --verbose

build/binding.Makefile:
	node-gyp configure

install: $(OMCACHE_BIN)
	mkdir -p $(BASE)
	cp package.json $(BASE)
	cp $< $(BASE)

deb:
	dpkg-buildpackage -b -uc -us
	mv ../node-omcache* .

clean:
	rm -rf build node-omcache*.changes node-omcache*.deb

deb-clean:

.PHONY: all omcache
