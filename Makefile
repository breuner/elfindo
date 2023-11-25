# 
# Use "make help" to find out about configuration options.
#

EXE_NAME           ?= $(shell cat build_helpers/findexename)
EXE_VER_MAJOR      ?= 0
EXE_VER_MINOR      ?= 9
EXE_VER_PATCHLEVEL ?= 5
EXE_VERSION        ?= $(EXE_VER_MAJOR).$(EXE_VER_MINOR)-$(EXE_VER_PATCHLEVEL)
EXE                ?= $(BIN_PATH)/$(EXE_NAME)
EXE_UNSTRIPPED     ?= $(EXE)-unstripped
PACKAGE_NAME       ?= $(shell cat build_helpers/packagename)

SOURCE_PATH        ?= ./source
BIN_PATH           ?= ./bin
EXTERNAL_PATH      ?= ./external
PACKAGING_PATH     ?= ./packaging
BUILD_HELPERS_PATH ?= ./build_helpers

INST_PATH          ?= /usr/local/bin
PKG_INST_PATH      ?= /usr/bin

CXX                ?= g++
STRIP              ?= strip
CXX_FLAVOR         ?= c++17

CXXFLAGS_COMMON   = -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 $(CXXFLAGS_BOOST) \
	-DNCURSES_NOMACROS -DEXE_NAME=\"$(EXE_NAME)\" -DEXE_VERSION=\"$(EXE_VERSION)\" \
	-I $(SOURCE_PATH) \
	-Wall -Wunused-variable -Woverloaded-virtual -Wextra -Wno-unused-parameter -fmessage-length=0 \
	-fno-strict-aliasing -pthread -ggdb -std=$(CXX_FLAVOR)
CXXFLAGS_RELEASE  = -O3 -Wuninitialized
CXXFLAGS_DEBUG    = -O0 -D_FORTIFY_SOURCE=2 -DBUILD_DEBUG

LDFLAGS_COMMON    = -rdynamic -pthread -lrt -lstdc++fs
LDFLAGS_RELASE    = -O3
LDFLAGS_DEBUG     = -O0

SOURCES          := $(shell find $(SOURCE_PATH) -name '*.cpp')
OBJECTS          := $(SOURCES:.cpp=.o)
OBJECTS_CLEANUP  := $(shell find $(SOURCE_PATH) -name '*.o') # separate to clean after C file rename
DEPENDENCY_FILES := $(shell find $(SOURCE_PATH) -name '*.d')

# Release & debug flags for compiler and linker
ifeq ($(BUILD_DEBUG), 1)
CXXFLAGS = $(CXXFLAGS_COMMON) $(CXXFLAGS_DEBUG) $(CXXFLAGS_EXTRA)
LDFLAGS  = $(LDFLAGS_COMMON) $(LDFLAGS_DEBUG) $(LDFLAGS_EXTRA)
else
CXXFLAGS = $(CXXFLAGS_COMMON) $(CXXFLAGS_RELEASE) $(CXXFLAGS_EXTRA)
LDFLAGS  = $(LDFLAGS_COMMON) $(LDFLAGS_RELASE) $(LDFLAGS_EXTRA)
endif

# Dynamic or static linking
ifeq ($(BUILD_STATIC), 1)
LDFLAGS            += -static 
else # dynamic linking
endif

# Use Microsoft mimalloc for memory allocations.
# Note: This needs to come as very last in link order, thus we have a separate variable to ensure
# it's the trailing arg for the linker. (Can be confirmed e.g. via MIMALLOC_SHOW_STATS=1)
ifeq ($(USE_MIMALLOC), 1)
CXXFLAGS += -DUSE_MIMALLOC
LDFLAGS_MIMALLOC_TAIL := -L external/mimalloc/build -l:libmimalloc.a
endif

# Support build in Cygwin environment
ifeq ($(CYGWIN_SUPPORT), 1)
# EXE_UNSTRIPPED includes EXE in definition, so must be updated first 
EXE_UNSTRIPPED     := $(EXE_UNSTRIPPED).exe
EXE                := $(EXE).exe

CXX_FLAVOR         := gnu++17
CXXFLAGS           += -DCYGWIN_SUPPORT
endif

all: $(SOURCES) $(EXE)

$(EXE): $(EXE_UNSTRIPPED)
ifdef BUILD_VERBOSE
	$(STRIP) --strip-debug $(EXE_UNSTRIPPED) -o $(EXE)
else
	@echo [STRIP] $@ 
	@$(STRIP) --strip-debug $(EXE_UNSTRIPPED) -o $(EXE)
endif

$(EXE_UNSTRIPPED): $(OBJECTS)
ifdef BUILD_VERBOSE
	$(CXX) -o $(EXE_UNSTRIPPED) $(OBJECTS) $(LDFLAGS) $(LDFLAGS_MIMALLOC_TAIL)
else
	@echo [LINK] $@
	@$(CXX) -o $(EXE_UNSTRIPPED) $(OBJECTS) $(LDFLAGS) $(LDFLAGS_MIMALLOC_TAIL)
endif

.c.o:
ifdef BUILD_VERBOSE
	$(CXX) $(CXXFLAGS) -c $(@:.o=.c) -E -MMD -MF $(@:.o=.d) -MT $(@) -o /dev/null
	$(CXX) $(CXXFLAGS) -c $(@:.o=.c) -o $(@)
else
	@echo [DEP] $(@:.o=.d)
	@$(CXX) $(CXXFLAGS) -c $(@:.o=.c) -E -MMD -MF $(@:.o=.d) -MT $(@) -o /dev/null
	@echo [CXX] $@
	@$(CXX) $(CXXFLAGS) -c $(@:.o=.c) -o $(@)
endif

.cpp.o:
ifdef BUILD_VERBOSE
	$(CXX) $(CXXFLAGS) -c $(@:.o=.cpp) -E -MMD -MF $(@:.o=.d) -MT $(@) -o /dev/null
	$(CXX) $(CXXFLAGS) -c $(@:.o=.cpp) -o $(@)
else
	@echo [DEP] $(@:.o=.d)
	@$(CXX) $(CXXFLAGS) -c $(@:.o=.cpp) -E -MMD -MF $(@:.o=.d) -MT $(@) -o /dev/null
	@echo [CXX] $@
	@$(CXX) $(CXXFLAGS) -c $(@:.o=.cpp) -o $(@)
endif

$(OBJECTS): Makefile | externals features-info # Makefile dep to rebuild all on Makefile change

externals:
ifdef BUILD_VERBOSE
	PREP_MIMALLOC=$(USE_MIMALLOC) $(EXTERNAL_PATH)/prepare-external.sh
else
	@PREP_MIMALLOC=$(USE_MIMALLOC) $(EXTERNAL_PATH)/prepare-external.sh
endif

features-info:
ifeq ($(USE_MIMALLOC),1)
	$(info [OPT] mimalloc enabled)
else
	$(info [OPT] mimalloc disabled)
endif

clean: clean-packaging clean-buildhelpers
ifdef BUILD_VERBOSE
	rm -rf $(OBJECTS_CLEANUP) $(DEPENDENCY_FILES) $(EXE) $(EXE).exe $(EXE_UNSTRIPPED) \
		$(EXE_UNSTRIPPED).exe
else
	@echo "[DELETE] OBJECTS, DEPENDENCY_FILES, EXECUTABLES"
	@rm -rf $(OBJECTS_CLEANUP) $(DEPENDENCY_FILES) $(EXE) $(EXE).exe $(EXE_UNSTRIPPED) \
		$(EXE_UNSTRIPPED).exe
endif

clean-externals:
ifdef BUILD_VERBOSE
	rm -rf $(EXTERNAL_PATH)/mimalloc
else
	@echo "[DELETE] EXTERNALS"
	@rm -rf $(EXTERNAL_PATH)/mimalloc
endif

clean-packaging:
ifdef BUILD_VERBOSE
	rm -rf \
		$(EXE)-*-static-* \
		$(PACKAGING_PATH)/$(PACKAGE_NAME)-*-static-*.tar.* \
	rm -rf \
		$(PACKAGING_PATH)/BUILDROOT \
		$(PACKAGING_PATH)/RPMS/* $(PACKAGING_PATH)/SPECS/rpm.spec
	bash -c "rm -rf $(PACKAGING_PATH)/$(PACKAGE_NAME)*.{deb,ddeb,build,buildinfo,changes}"
else
	@echo "[DELETE] PACKAGING_FILES"
	@rm -rf \
		$(EXE)-*-static-* \
		$(PACKAGING_PATH)/$(PACKAGE_NAME)-*-static-*.tar.* \
	@rm -rf \
		$(PACKAGING_PATH)/BUILDROOT \
		$(PACKAGING_PATH)/RPMS/* $(PACKAGING_PATH)/SPECS/rpm.spec
	@bash -c "rm -rf $(PACKAGING_PATH)/$(PACKAGE_NAME)*.{deb,ddeb,build,buildinfo,changes}"
endif

clean-all: clean clean-externals clean-packaging clean-buildhelpers

install: all
	@echo "Installing..."
	
	install -p -m u=rwx,g=rx,o=rx $(EXE) $(INST_PATH)/
	
uninstall:
	rm -f $(INST_PATH)/$(EXE_NAME)

# prepare generic part of build-root (not the .rpm or .deb specific part)
prepare-buildroot: | all clean-packaging
	@echo "[PACKAGING] PREPARE BUILDROOT"

	mkdir -p $(PACKAGING_PATH)/BUILDROOT/$(PKG_INST_PATH)

	# copy main executable
	cp --preserve $(EXE) $(PACKAGING_PATH)/BUILDROOT/$(PKG_INST_PATH)

	# copy contents of dist subdir
	for dir in $(shell find dist/ -mindepth 1 -type d -printf "%P\n"); do \
		mkdir -p $(PACKAGING_PATH)/BUILDROOT/$$dir; \
	done

	for file in $(shell find dist/ -mindepth 1 -type f -printf "%P\n"); do \
		cp --preserve dist/$$file $(PACKAGING_PATH)/BUILDROOT/$$file; \
	done
	
rpm: | prepare-buildroot
	@echo "[PACKAGING] PREPARE RPM PACKAGE"

	cp $(PACKAGING_PATH)/SPECS/rpm.spec.template $(PACKAGING_PATH)/SPECS/rpm.spec
	sed -i "s/__PACKAGE_NAME__/$(PACKAGE_NAME)/g" $(PACKAGING_PATH)/SPECS/rpm.spec
	sed -i "s/__FIND_EXE_NAME__/$(EXE_NAME)/g" $(PACKAGING_PATH)/SPECS/rpm.spec
	sed -i "s/__VERSION__/$(EXE_VER_MAJOR).$(EXE_VER_MINOR).$(EXE_VER_PATCHLEVEL)/g" \
		$(PACKAGING_PATH)/SPECS/rpm.spec
	
	rpmbuild $(PACKAGING_PATH)/SPECS/rpm.spec --bb --define "_topdir $(PWD)/$(PACKAGING_PATH)" \
		--define "__spec_install_pre /bin/true" --buildroot=$(PWD)/$(PACKAGING_PATH)/BUILDROOT
	
	@echo
	@echo "All done. Your package is here:"
	@find $(PACKAGING_PATH)/RPMS -name $(PACKAGE_NAME)*.rpm

deb: | prepare-buildroot
	@echo "[PACKAGING] PREPARE DEB PACKAGE"

	cp -r $(PACKAGING_PATH)/debian $(PACKAGING_PATH)/BUILDROOT
	
	cp $(PACKAGING_PATH)/BUILDROOT/debian/control.template \
		$(PACKAGING_PATH)/BUILDROOT/debian/control
	cp $(PACKAGING_PATH)/BUILDROOT/debian/rules.template \
		$(PACKAGING_PATH)/BUILDROOT/debian/rules

	sed -i "s/__PACKAGE_NAME__/$(PACKAGE_NAME)/g" $(PACKAGING_PATH)/BUILDROOT/debian/control
	sed -i "s/__PACKAGE_NAME__/$(PACKAGE_NAME)/g" $(PACKAGING_PATH)/BUILDROOT/debian/rules
	sed -i "s/__FIND_EXE_NAME__/$(EXE_NAME)/g" $(PACKAGING_PATH)/BUILDROOT/debian/rules
	
	cd $(PACKAGING_PATH)/BUILDROOT && \
		EDITOR=/bin/true VISUAL=/bin/true debchange --create --package $(PACKAGE_NAME) \
			--urgency low --noquery -M \
			--newversion "$(EXE_VER_MAJOR).$(EXE_VER_MINOR).$(EXE_VER_PATCHLEVEL)" \
			"Custom package build."
	
	cd $(PACKAGING_PATH)/BUILDROOT && \
		debuild -b -us -uc
	
	@echo
	@echo "All done. Your package is here:"
	@find $(PACKAGING_PATH) -name $(PACKAGE_NAME)*.deb

version:
	@echo $(EXE_VERSION)

help:
	@echo 'Optional Build Features:'
	@echo '   CYGWIN_SUPPORT=0|1      - Adapt build features to enable build in Cygwin'
	@echo '                             environment. (Default: 0)'
	@echo '   USE_MIMALLOC=0|1        - Use Microsoft mimalloc library for memory'
	@echo '                             allocation management. Recommended when using'
	@echo '                             musl-libc. (Default: 0)'
	@echo
	@echo 'Optional Compile/Link Arguments:'
	@echo '   CXX=<string>            - Path to alternative C++ compiler. (Default: g++)'
	@echo '   CXX_FLAVOR=<string>     - C++ standard compiler flag. (Default: c++17)'
	@echo '   CXXFLAGS_EXTRA=<string> - Additional C++ compiler flags.'
	@echo '   LDFLAGS_EXTRA=<string>  - Additional linker flags.'
	@echo '   BUILD_VERBOSE=1         - Enable verbose build output.'
	@echo '   BUILD_STATIC=1          - Generate a static binary without dependencies.'
	@echo '                             (Tested only on Alpine Linux.)'
	@echo '   BUILD_DEBUG=1           - Include debug info in executable.'
	@echo
	@echo 'Targets:'
	@echo '   all (default)     - Build executable'
	@echo '   clean             - Remove build artifacts'
	@echo '   clean-all         - Remove build artifacts and external sources'
	@echo '   install           - Install executable to /usr/local/bin'
	@echo '   uninstall         - Uninstall executable from /usr/local/bin'
	@echo '   rpm               - Create RPM package file'
	@echo '   deb               - Create Debian package file'
	@echo '   help              - Print this help message'
	@echo
	@echo 'Note: Use "make clean-all" when changing any optional build features.'

.PHONY: clean clean-all clean-externals clean-packaging clean-buildhelpers deb externals \
features-info help prepare-buildroot rpm version

.DEFAULT_GOAL := all

# Include dependency files
ifneq ($(DEPENDENCY_FILES),)
include $(DEPENDENCY_FILES)
endif
