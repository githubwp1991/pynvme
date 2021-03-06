#
#  BSD LICENSE
#
#  Copyright (c) Crane Che <cranechu@gmail.com>
#  All rights reserved.
#
#  Redistribution and use in source and binary forms, with or without
#  modification, are permitted provided that the following conditions
#  are met:
#
#    * Redistributions of source code must retain the above copyright
#      notice, this list of conditions and the following disclaimer.
#    * Redistributions in binary form must reproduce the above copyright
#      notice, this list of conditions and the following disclaimer in
#      the documentation and/or other materials provided with the
#      distribution.
#    * Neither the name of Intel Corporation nor the names of its
#      contributors may be used to endorse or promote products derived
#      from this software without specific prior written permission.
#
#  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
#  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
#  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
#  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
#  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
#  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
#  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
#  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
#  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
#  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
#  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

#SPDK infrastructure
SPDK_ROOT_DIR := $(abspath $(CURDIR)/spdk)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

C_SRCS = driver.c 
LIBNAME = pynvme

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk

#find the first NVMe device as the DUT
pciaddr=$(shell lspci | grep 'Non-Volatile memory' | grep -o '..:..\..' | head -1)

#reserve 3G to others, get all remaining memory for DPDK
memsize=$(shell free -m | awk 'NR==2{print ($$2-$$2%4)*3/4}')


#cython part
clean: cython_clean
cython_clean:
	@sudo rm -rf build *.o nvme.*.so cdriver.c driver_wrap.c __pycache__ .pytest_cache cov_report .coverage.* test.log

all: cython_lib
.PHONY: all spdk

spdk:
	cd spdk/dpdk; git checkout spdk-18.08; cd ../..
	cd spdk; make clean; ./configure --disable-tests --without-vhost --without-virtio --without-isal; make; cd ..

doc:
	pydocmd simple nvme++ > README.md

reset:
	sudo HUGEMEM=${memsize} ./spdk/scripts/setup.sh reset
	-sudo rm /var/tmp/spdk.sock*
	-sudo fuser -k 4420/tcp

setup: reset
	sudo HUGEMEM=${memsize} ./spdk/scripts/setup.sh

cython_lib:
	@python3 setup.py build_ext -i

tags: 
	ctags -e --c-kinds=+l -R --exclude=.git --exclude=test --exclude=dpdk --exclude=ioat --exclude=bdev --exclude=snippets --exclude=env 

test: setup
	sudo python3 -m pytest driver_test.py --pciaddr=${pciaddr} -v -x -r Efsx |& tee -a test.log
	cat test.log | grep "193 passed, 8 skipped, 1 xfailed, 1 warnings" || exit -1

nvmt: setup      # create a NVMe/TCP target on 2 cores, based on memory bdev, for local test only
	sudo ./spdk/app/nvmf_tgt/nvmf_tgt -m 3 &
	sleep 5
	sudo ./spdk/scripts/rpc.py construct_malloc_bdev -b Malloc0 64 512
	sudo ./spdk/scripts/rpc.py nvmf_create_transport -t TCP -p 4
	sudo ./spdk/scripts/rpc.py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
	sudo ./spdk/scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc0
	sudo ./spdk/scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t tcp -a 127.0.0.1 -s 4420

