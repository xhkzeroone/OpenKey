# Đóng gói OpenKey

Tài liệu này ghi cách build package cài đặt cho `fcitx5-openkey` từ source trong repo.

Hiện repo chưa cấu hình CPack/debian/rpm spec sẵn, nên cách nhanh nhất là:

1. Build bằng CMake.
2. Install vào thư mục staging qua `DESTDIR`.
3. Dùng `fpm` đóng `.deb` hoặc `.rpm` từ staging đó.

## Cài công cụ đóng gói

### Debian / Ubuntu

```bash
sudo apt update
sudo apt install -y ruby ruby-dev build-essential
sudo gem install --no-document fpm
```

Kiểm tra:

```bash
fpm --version
```

Nếu máy chưa có dependency build của OpenKey, cài thêm:

```bash
sudo apt install -y \
  cmake \
  pkg-config \
  extra-cmake-modules \
  gettext \
  golang-go \
  fcitx5 \
  fcitx5-config-qt \
  fcitx5-frontend-gtk3 \
  fcitx5-frontend-qt5 \
  libfcitx5core-dev \
  libfcitx5config-dev \
  libfcitx5utils-dev
```

Nếu distro không có các gói dev tách riêng, thử:

```bash
sudo apt install -y fcitx5-dev
```

### Fedora

```bash
sudo dnf install -y \
  ruby \
  ruby-devel \
  gcc-c++ \
  make \
  rpm-build

sudo gem install --no-document fpm
```

Dependency build:

```bash
sudo dnf install -y \
  cmake \
  pkgconf-pkg-config \
  extra-cmake-modules \
  gettext \
  golang \
  fcitx5 \
  fcitx5-configtool \
  fcitx5-gtk \
  fcitx5-qt \
  fcitx5-devel
```

## Build staging tree

Chạy từ root repo:

```bash
rm -rf build-pkg pkgroot dist

cmake -S . -B build-pkg \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr

cmake --build build-pkg -j
DESTDIR="$PWD/pkgroot" cmake --install build-pkg --strip

mkdir -p dist
```

Sau bước này, staging tree sẽ có dạng:

```text
pkgroot/usr/lib/.../fcitx5/openkey.so
pkgroot/usr/libexec/openkey-nonpreedit-server
pkgroot/usr/share/fcitx5/addon/openkey.conf
pkgroot/usr/share/fcitx5/inputmethod/openkey.conf
pkgroot/usr/share/icons/...
```

Đường dẫn `lib` có thể khác nhau theo distro, ví dụ `usr/lib/x86_64-linux-gnu` trên Debian/Ubuntu hoặc `usr/lib64` trên Fedora.

## Build .deb

```bash
fpm -s dir -t deb \
  -n fcitx5-openkey \
  -v 0.1.0 \
  --architecture native \
  --description "OpenKey Vietnamese input method for Fcitx5" \
  --url "https://github.com/open-key/OpenKey" \
  --license GPL-3.0 \
  --depends fcitx5 \
  --depends fcitx5-frontend-gtk3 \
  --depends fcitx5-frontend-qt5 \
  -C pkgroot \
  -p dist/fcitx5-openkey_0.1.0_amd64.deb \
  .
```

Cài thử package:

```bash
sudo apt install ./dist/fcitx5-openkey_0.1.0_amd64.deb
fcitx5 -rd
```

Kiểm tra nội dung package:

```bash
dpkg-deb -c dist/fcitx5-openkey_0.1.0_amd64.deb
```

## Build .rpm

```bash
fpm -s dir -t rpm \
  -n fcitx5-openkey \
  -v 0.1.0 \
  --architecture native \
  --description "OpenKey Vietnamese input method for Fcitx5" \
  --url "https://github.com/open-key/OpenKey" \
  --license GPL-3.0 \
  --depends fcitx5 \
  -C pkgroot \
  -p dist/fcitx5-openkey-0.1.0.x86_64.rpm \
  .
```

Cài thử package:

```bash
sudo dnf install ./dist/fcitx5-openkey-0.1.0.x86_64.rpm
fcitx5 -rd
```

Kiểm tra nội dung package:

```bash
rpm -qpl dist/fcitx5-openkey-0.1.0.x86_64.rpm
```

## Build bằng GitHub Actions

Repo có workflow `.github/workflows/package-linux.yml` để build package Linux tự động.

Workflow tạo:

- `.deb` trên Ubuntu runner.
- `.rpm` trong Ubuntu container.

Cả `.deb` và `.rpm` đều build kèm helper Go `openkey-nonpreedit-server`.

Chạy thủ công:

1. Vào tab `Actions` trên GitHub.
2. Chọn workflow `Package Linux`.
3. Bấm `Run workflow`.

Sau khi chạy xong, tải package ở phần `Artifacts` của workflow run:

- `fcitx5-openkey-deb`
- `fcitx5-openkey-rpm`

Nếu push tag dạng `v*`, workflow cũng tự upload package vào GitHub Release cùng tag:

```bash
git tag v0.1.0
git push origin v0.1.0
```

Version package được đọc từ dòng `project(fcitx5-openkey VERSION ...)` trong `CMakeLists.txt`.

## Ghi chú release

- Version hiện lấy thủ công từ `project(fcitx5-openkey VERSION ...)` trong `CMakeLists.txt`.
- Nếu đổi version, nhớ đổi cả tên file output trong lệnh `fpm`.
- `pkgroot/`, `dist/`, và `build-pkg/` là output tạm; không commit các thư mục này.
- Sau khi cài package, cần restart `fcitx5` bằng `fcitx5 -rd`.
- Nếu dùng mode `NonPreedit`, máy vẫn cần `/dev/uinput` và quyền truy cập phù hợp như hướng dẫn trong `README.md`.
