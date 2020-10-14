#!/usr/bin/env bash
# install OpenMPI, Horovod and gcc8

set -e

# Install Open MPI
#apt-get install -y --no-install-recommends openmpi-bin openmpi-common libopenmpi-dev
OPENMPI_VERSION=openmpi-2.1.1
OPENMPI_DOWNLOAD_URL=https://www.open-mpi.org/software/ompi/v2.1/downloads/${OPENMPI_VERSION}.tar.gz

mkdir /tmp/openmpi
cd /tmp/openmpi
curl -fSsL -O ${OPENMPI_DOWNLOAD_URL}
tar zxf ${OPENMPI_VERSION}.tar.gz
cd ${OPENMPI_VERSION}
./configure --enable-mpirun-prefix-by-default
make -j $(nproc) all
make install
ldconfig
cd /
rm -rf /tmp/openmpi

# Create a wrapper for OpenMPI to allow running as root by default
mv /usr/local/bin/mpirun /usr/local/bin/mpirun.real
echo '#!/bin/bash' > /usr/local/bin/mpirun
echo 'mpirun.real --allow-run-as-root "$@"' >> /usr/local/bin/mpirun
chmod a+x /usr/local/bin/mpirun

# Configure OpenMPI to run good defaults:
echo "btl_tcp_if_exclude = lo,docker0" >> /usr/local/etc/openmpi-mca-params.conf

#Check mpi version
echo 'OpenMPI version:'
mpirun --version

#Install Horovod
HOROVOD_WITH_TENSORFLOW=1
python3 -m pip install --no-cache-dir horovod==0.19.1



