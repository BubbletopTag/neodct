#stage 1 prepare cage

mkdir -p /run/user/0
chmod 700 /run/user/0
export XDG_RUNTIME_DIR=/run/user/0
mkdir -p /dev/shm
mount -t tmpfs tmpfs /dev/shm
#stage 2, launch cage
cage -- sh /tests/neodct-browser/start2.sh