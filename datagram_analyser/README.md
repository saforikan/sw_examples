# Datagram analyser
This is a simple DPDK-based application to analyse / verify datagrams received from a multiport 10G PCIe tap device.
This application is intended for personal / internal only - neither maintenance nor support should be expected.

## Build
1. Run `make` to build `datagram_analyser`

## Usage
1. Ensure that the tap is bound to use a DPDK PMD compatible driver through the `dpdk-devbind.py` utility. This application is based on the `igb_uio` driver.
2. Run the application with root permission: `sudo build/datagram_analyser`. It should automatically pick up the available device and start processing.
