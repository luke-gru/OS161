# Build kernel
set -e
top="$HOME/workspace/os161"
cd $top/kern/conf
./config DUMBVM
cd ../compile/DUMBVM
bmake depend
bmake
bmake install
