#!/bin/bash
# Script to setup environment for XRT
# This script is installed in /opt/xilinx/xrt and must
# be sourced from that location

# Check OS version requirement
OSDIST=`lsb_release -i |awk -F: '{print tolower($2)}' | tr -d ' \t'`
OSREL=`lsb_release -r |awk -F: '{print tolower($2)}' |tr -d ' \t'`

if [[ $OSDIST == "ubuntu" ]]; then
    if [[ $OSREL != "16.04" ]] &&  [[ $OSREL != "18.04" ]]; then
        echo "ERROR: Ubuntu release version must be 16.04 or later"
        return 1
    fi
fi

if [[ $OSDIST == "centos" ]] || [[ $OSDIST == "redhat"* ]]; then
    if [[ $OSREL != "7.4"* ]] &&  [[ $OSREL != "7.5"* ]] && [[ $OSREL != "7.6"* ]]; then
        echo "ERROR: Centos or RHEL release version must be 7.4 or later"
        return 1
    fi
fi

XILINX_XRT=$(readlink -f $(dirname ${BASH_SOURCE[0]}))

if [[ $XILINX_XRT != *"/opt/xilinx/xrt" ]]; then
    echo "Invalid location: $XILINX_XRT"
    echo "This script must be sourced from XRT install directory"
    return 1
fi

export XILINX_XRT
export LD_LIBRARY_PATH=$XILINX_XRT/lib:$LD_LIBRARY_PATH
export PATH=$XILINX_XRT/bin:$PATH

echo "XILINX_XRT      : $XILINX_XRT"
echo "PATH            : $PATH"
echo "LD_LIBRARY_PATH : $LD_LIBRARY_PATH"
