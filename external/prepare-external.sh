#!/bin/bash
#
# Prepare git clones and checkout the required tags of external sources.
# Mimalloc will only be prepared when PREP_MIMALLOC=1 is set.

EXTERNAL_BASE_DIR="$(pwd)/$(dirname $0)"

# Prepare git clone and required tag.
prepare_mimalloc()
{
	local REQUIRED_TAG="v2.0.6"
	local CURRENT_TAG
	local CLONE_DIR="${EXTERNAL_BASE_DIR}/mimalloc"
	local INSTALL_DIR="${EXTERNAL_BASE_DIR}/mimalloc/build"
	
	# change to external subdir if we were called from somewhere else
	cd "$EXTERNAL_BASE_DIR" || exit 1
	
	# clone if directory does not exist yet
	if [ ! -d "$CLONE_DIR" ]; then
		echo "Cloning mimalloc git repo..."
		git clone https://github.com/microsoft/mimalloc.git $CLONE_DIR
		if [ $? -ne 0 ]; then
			exit 1
		fi
	fi
	
	# directory exists, check if we already have the right tag.
	# (this is the fast path for dependency calls from Makefile)
	cd "$CLONE_DIR" && \
		CURRENT_TAG="$(git describe --tags)" && \
		if [ "$CURRENT_TAG" = "$REQUIRED_TAG" ] && [ -f build/libmimalloc.a ] ; then
			# Already at the right tag, so nothing to do
			return 0;
		fi && \
		cd "$EXTERNAL_BASE_DIR"

	# we need to change tag...
	
	# clean up and uninstall any previous build before we switch to new tag
	if [ -f "$INSTALL_DIR/Makefile" ]; then
		echo "Cleaning up previous build..."
		cd "$INSTALL_DIR" && \
		make clean
		
		[ $? -ne 0 ] && exit 1
	fi
	
	# check out required tag
	# (fetching is relevant in case we update to a new required tag.)
	echo "Checking out mimalloc tag ${REQUIRED_TAG}..."
	
	cd "$CLONE_DIR" && \
		git fetch --recurse-submodules -q --all && \
		git checkout -q ${REQUIRED_TAG} && \
		git submodule -q update --recursive && \
		cd "$EXTERNAL_BASE_DIR"
	
	[ $? -ne 0 ] && exit 1
	
	echo "Configuring build and running install..."
	mkdir -p "$INSTALL_DIR" && \
		cd "$INSTALL_DIR" && \
		cmake .. && \
		make -j $(nproc) && \
		cd "$EXTERNAL_BASE_DIR" && \
		return 0

	[ $? -ne 0 ] && exit 1
}


########### End of function definitions ############

if [ "$PREP_MIMALLOC" = "1" ]; then
	prepare_mimalloc
fi
