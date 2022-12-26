#!/bin/bash
#
# Build static executable in Alpine container and prune container after build.
# Call this script from the repository root dir.
#
# Note: "--privileged" is required to enable user change on RHEL7.
# Note: For podman's docker interface (e.g. RHEL8) remove "sudo -u builduser",
#       because it already runs under the calling UID.

PACKAGE_NAME=$(cat build_helpers/packagename)
PACKAGE_VERSION=$(make version)
FIND_EXE_NAME=$(cat build_helpers/findexename)
CONTAINER_NAME="${PACKAGE_NAME}-static"
IMAGE_NAME="alpine:3.14"

rm -f packaging/putils-\${PACKAGING_VERSION}-static-$(uname -m).tar.gz

docker rm $CONTAINER_NAME

docker pull $IMAGE_NAME && \
docker run --name $CONTAINER_NAME --privileged -it -v $PWD:$PWD -w $PWD $IMAGE_NAME \
    sh -c "\
    apk add bash boost-dev build-base cmake gcc g++ git libexecinfo-dev make \
        libexecinfo-static sudo && \
    adduser -u $UID -D -H builduser && \
    sudo -u builduser make clean-all && \
    sudo -u builduser make -j $(nproc) \
        USE_MIMALLOC=1 BUILD_STATIC=1" && \
docker rm $CONTAINER_NAME && \
cd bin/ && \
./${FIND_EXE_NAME} --version && \
cp ${FIND_EXE_NAME} ${FIND_EXE_NAME}-${PACKAGE_VERSION}-static-$(uname -m) && \
tar -czf ../packaging/${PACKAGE_NAME}-${PACKAGE_VERSION}-static-$(uname -m).tar.gz ${FIND_EXE_NAME} && \
cd .. && \
echo "All Done. Your tar file is here:" && \
find ./packaging -maxdepth 1 -name "*-static-*.tar.gz" && \
exit 0

echo CANCELLED DUE TO ERROR && \
exit 1
