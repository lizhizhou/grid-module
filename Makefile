export LOPHILO_DIR:=${HOME}/lophilo
export ARCH:=$(shell /usr/bin/dpkg-architecture -qDEB_HOST_ARCH_CPU)
export TARGET_OS:=${HOME}/lophilo.nfs
export TOOLCHAIN_PATH:=${LOPHILO_DIR}/codesourcery/arm926ej-s
export PATH:=${TOOLCHAIN_PATH}:${PATH}
export BUILD_DIR:=${LOPHILO_DIR}/obj/linux
export BUILD_DIR_DEBUG:=${LOPHILO_DIR}/obj/linux-debug
export SRC_DIR:=${LOPHILO_DIR}/linux
export KBUILD_VERBOSE:=0

obj-m += lophilo.o

.PHONY: clean install checkstyle

all: install

lophilo-standard.ko: lophilo.c
	make ARCH=arm -C ${BUILD_DIR} M=$(PWD) modules
	mv lophilo.ko lophilo-standard.ko

lophilo-debug.ko: lophilo.c
	make ARCH=arm -C ${BUILD_DIR_DEBUG} M=$(PWD) modules
	mv lophilo.ko lophilo-debug.ko

clean: 
	make ARCH=arm -C ${BUILD_DIR} M=$(PWD) clean

install: lophilo-debug.ko lophilo-standard.ko lophilo_user
	sudo cp lophilo-standard.ko ${TARGET_OS} 
	sudo cp lophilo-debug.ko ${TARGET_OS}
	sudo cp lophilo_user ${TARGET_OS} 

checkstyle:
	# the root requirement doesn't seem to be there in 3.4
	${SRC_DIR}/scripts/checkpatch.pl -f lophilo.c

lophilo_user: lophilo_user.c
	gcc -g lophilo_user.c -o lophilo_user
