# Top source directory on local system---assumed to be git clone
TOP_SRCDIR=${HOME}/src/zrlio/urdma

# Directory to deploy to
DEPLOY_DIR=/var/tmp/urdma

# App executable
SERVER_APP=ib_write_lat
CLIENT_APP=ib_write_lat

# Management IP addresses
SERVER_NODE=mundilfari.ofa.iol.unh.edu
CLIENT_NODE=thrymr.ofa.iol.unh.edu

SERVER_DPDK_IP=mundilfari-iw.ofa.iol.unh.edu

# Other arguments --- passed to client, *not* to EAL
SERVER_EXTRA_ARGS="-R -n 5 -s 512"
CLIENT_EXTRA_ARGS="-R -n 5 -s 512"
