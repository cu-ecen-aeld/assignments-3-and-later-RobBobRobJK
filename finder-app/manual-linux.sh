#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.
set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-linux-gnu-

if [ $# -lt 1 ]
then
    echo "Using default directory ${OUTDIR} for output"
else
    OUTDIR=$1
    echo "Using passed directory ${OUTDIR} for output"
fi

mkdir -p ${OUTDIR} || { echo "Could not create ${OUTDIR}"; exit 1; }

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/linux-stable" ]; then
    echo "CLONING GIT LINUX STABLE VERSION ${KERNEL_VERSION} IN ${OUTDIR}"
    git clone ${KERNEL_REPO} --depth 1 --single-branch --branch ${KERNEL_VERSION}
fi

if [ ! -e ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ]; then
    cd linux-stable
    echo "Checking out version ${KERNEL_VERSION}"
    git checkout ${KERNEL_VERSION}

    echo "Cleaning kernel build tree"
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} mrproper

    echo "Configuring kernel for virt platform"
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig

    echo "Building kernel image"
    make -j$(nproc) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} all

    echo "Building kernel modules"
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} modules

    echo "Building device tree"
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} dtbs
fi

echo "Adding the Image in outdir"
cp ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ${OUTDIR}/

echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"
if [ -d "${OUTDIR}/rootfs" ]; then
    echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"
    sudo rm -rf ${OUTDIR}/rootfs
fi

mkdir -p ${OUTDIR}/rootfs/{bin,dev,etc,home,lib,lib64,proc,sbin,sys,tmp,usr,var}
mkdir -p ${OUTDIR}/rootfs/usr/{bin,lib,sbin}
mkdir -p ${OUTDIR}/rootfs/var/log

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/busybox" ]; then
    git clone git://busybox.net/busybox.git
    cd busybox
    git checkout ${BUSYBOX_VERSION}
    make distclean
    make defconfig
else
    cd busybox
fi

make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE}
make CONFIG_PREFIX=${OUTDIR}/rootfs ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} install

echo "Library dependencies"
${CROSS_COMPILE}readelf -a ${OUTDIR}/rootfs/bin/busybox | grep "program interpreter"
${CROSS_COMPILE}readelf -a ${OUTDIR}/rootfs/bin/busybox | grep "Shared library"

SYSROOT=/usr/aarch64-linux-gnu

cp -a ${SYSROOT}/lib/ld-linux-aarch64.so.1 ${OUTDIR}/rootfs/lib/
cp -a ${SYSROOT}/lib/libm.so.6 ${OUTDIR}/rootfs/lib64/
cp -a ${SYSROOT}/lib/libm.so.6 ${OUTDIR}/rootfs/lib/
cp -a ${SYSROOT}/lib/libresolv.so.2 ${OUTDIR}/rootfs/lib64/
cp -a ${SYSROOT}/lib/libresolv.so.2 ${OUTDIR}/rootfs/lib/
cp -a ${SYSROOT}/lib/libc.so.6 ${OUTDIR}/rootfs/lib64/
cp -a ${SYSROOT}/lib/libc.so.6 ${OUTDIR}/rootfs/lib/

sudo mknod -m 666 ${OUTDIR}/rootfs/dev/null c 1 3
sudo mknod -m 600 ${OUTDIR}/rootfs/dev/console c 5 1

cd ${FINDER_APP_DIR}
make clean
make CROSS_COMPILE=${CROSS_COMPILE}
cp writer ${OUTDIR}/rootfs/home/

cp ${FINDER_APP_DIR}/finder.sh ${OUTDIR}/rootfs/home/
cp ${FINDER_APP_DIR}/finder-test.sh ${OUTDIR}/rootfs/home/
cp ${FINDER_APP_DIR}/autorun-qemu.sh ${OUTDIR}/rootfs/home/

mkdir -p ${OUTDIR}/rootfs/home/conf
cp ${FINDER_APP_DIR}/conf/username.txt ${OUTDIR}/rootfs/home/conf/
cp ${FINDER_APP_DIR}/conf/assignment.txt ${OUTDIR}/rootfs/home/conf/

sed -i 's|\.\./conf/assignment\.txt|conf/assignment.txt|g' ${OUTDIR}/rootfs/home/finder-test.sh

sudo chown -R root:root ${OUTDIR}/rootfs

cd ${OUTDIR}/rootfs
find . | cpio -H newc -ov --owner root:root > ${OUTDIR}/initramfs.cpio
cd ${OUTDIR}
gzip -f initramfs.cpio

echo "Done! Kernel image: ${OUTDIR}/Image"
echo "Done! Initramfs: ${OUTDIR}/initramfs.cpio.gz"
