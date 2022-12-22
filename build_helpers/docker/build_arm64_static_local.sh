#!/bin/bash
#
# Build static arm64 executable in Alpine container and prune container after build.
# Call this script from the repository root dir.
#
# Note: "--privileged" is required to enable user change on RHEL7.
#
# For cross-compile on Ubuntu x86_64:
#  * Install binfmt support and qemu to enable running non-native executables:
#    $ sudo apt-get install qemu binfmt-support qemu-user-static
#  * Register multiarch support:
#    $ docker run --rm --privileged multiarch/qemu-user-static --reset -p yes 
#  * Try running an arm64 executable:
#    $ docker run --rm -t arm64v8/ubuntu uname -m 

PACKAGE_NAME=$(cat build_helpers/packagename)
PACKAGE_VERSION=$(make version)
FIND_EXE_NAME=$(cat build_helpers/findexename)
CONTAINER_NAME="${PACKAGE_NAME}-static"
IMAGE_NAME="alpine:3.14"

docker rm $CONTAINER_NAME

#docker pull $IMAGE_NAME && \
docker run --platform linux/arm64 --name $CONTAINER_NAME --privileged -it -v $PWD:$PWD -w $PWD $IMAGE_NAME \
    sh -c "\
    apk add bash build-base gcc g++ git libexecinfo-dev make \
        libexecinfo-static sudo tar && \
    adduser -u $UID -D -H builduser && \
    sudo -u builduser make clean-all && \
    sudo -u builduser make -j $(nproc) \
        USE_MIMALLOC=1 BUILD_STATIC=1 && \
    cd bin/ && \
    sudo -u builduser ./${FIND_EXE_NAME} --version && \
    sudo -u builduser cp ${FIND_EXE_NAME} ${FIND_EXE_NAME}-${PACKAGE_VERSION}-static-\$(uname -m) && \
    sudo -u builduser tar -czf ../packaging/${PACKAGE_NAME}-${PACKAGE_VERSION}-static-\$(uname -m).tar.gz ${FIND_EXE_NAME}" && \
docker rm $CONTAINER_NAME && \
echo "All Done. Your tar file is here:" && \
find ./packaging -maxdepth 1 -name "*-static-*.tar.gz" && \
exit 0

echo CANCELLED DUE TO ERROR && \
exit 1
