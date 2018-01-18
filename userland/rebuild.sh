# Build userland
set -e
top="$HOME/workspace/os161"
bmake distclean
bmake all
bmake install
