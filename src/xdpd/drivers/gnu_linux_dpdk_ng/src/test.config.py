#!/usr/bin/env python

import yaml

CONFIG="./xdpd-driver-dpdk.conf.yaml"

with open(CONFIG, "r") as f:
    d = yaml.load(f.read())

print d
