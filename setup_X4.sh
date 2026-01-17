sudo apt-get update
sudo apt-get install -y pkg-config libnl-3-dev libnl-route-3-dev libibumad-dev librdmacm-dev

# Some of the vendored MLNX 4.x-era code triggers new GCC warnings; don't treat warnings as errors.
export CFLAGS="${CFLAGS:-} -Wno-error"
export CXXFLAGS="${CXXFLAGS:-} -Wno-error"

cd ~/zxm/Justitia/libibverbs-41mlnx1/
./autogen.sh
./configure --prefix=/usr libdir=/usr/lib64
make
sudo make install
cd ~/zxm/Justitia/libmlx5-41mlnx1
./autogen.sh
./configure --prefix=/usr libdir=/usr/lib64
make
sudo make install
cd ~/Justitia/perftest-4.2/
./autogen.sh
./configure --prefix=/usr libdir=/usr/lib64
make
