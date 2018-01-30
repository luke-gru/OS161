# Build kernel
set -e
top="$HOME/workspace/os161"
cd $top/kern/conf
./config $1
cd ../compile/$1
bmake depend
bmake
bmake install
