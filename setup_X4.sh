set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

sudo apt-get update
sudo apt-get install -y pkg-config libnl-3-dev libnl-route-3-dev librdmacm-dev

# If MLNX OFED is installed, Ubuntu's libibumad-dev conflicts with libibumad-devel.
if dpkg -s libibumad-devel >/dev/null 2>&1; then
	echo "Detected MLNX OFED 'libibumad-devel'; skipping Ubuntu 'libibumad-dev'."
else
	sudo apt-get install -y libibumad-dev
fi

# Some of the vendored MLNX 4.x-era code triggers new GCC warnings; don't treat warnings as errors.
export CFLAGS="${CFLAGS:-} -Wno-error"
export CXXFLAGS="${CXXFLAGS:-} -Wno-error"

cd "$ROOT_DIR/libibverbs-41mlnx1/"
./autogen.sh
./configure --prefix=/usr libdir=/usr/lib64
make -j"$(nproc)"
sudo make install
cd "$ROOT_DIR/libmlx5-41mlnx1"
./autogen.sh
./configure --prefix=/usr libdir=/usr/lib64
make -j"$(nproc)"
sudo make install

# Ensure libibverbs loads the just-installed provider from /usr/lib64.
# Some distros search /usr/etc/libibverbs.d first; keep both in sync.
sudo install -d /usr/etc/libibverbs.d /etc/libibverbs.d
echo 'driver /usr/lib64/libmlx5' | sudo tee /usr/etc/libibverbs.d/mlx5.driver >/dev/null
echo 'driver /usr/lib64/libmlx5' | sudo tee /etc/libibverbs.d/mlx5.driver >/dev/null

sudo ldconfig

cd "$ROOT_DIR/perftest-4.2/"
./autogen.sh
./configure --prefix=/usr libdir=/usr/lib64
make -j"$(nproc)"
