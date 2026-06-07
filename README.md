# OpenKey cho Linux

OpenKey trong repo này là addon gõ tiếng Việt cho `fcitx5` trên Linux.
Phần xử lý tiếng Việt dùng lại core ở `Sources/OpenKey/engine`, còn phần Linux được build thành addon `openkey.so` để `fcitx5` nạp vào.

Ngoài addon chính, repo hiện có helper `openkey-nonpreedit-server` viết bằng Go. Helper này được cài vào `libexec`, được addon tự spawn khi cần, dùng `/dev/uinput` để bơm `BackSpace` thật trong mode `NonPreedit`, và sẽ tự tắt khi `fcitx5`/addon tắt.

README này chỉ ghi cho bản Linux, tập trung vào:

- cơ chế hoạt động
- cách cài
- dependencies cần có
- lỗi thường gặp khi cài xong nhưng không gõ được

## OpenKey Linux hoạt động như thế nào

OpenKey Linux không chạy như một app riêng. Nó hoạt động theo mô hình:

1. `fcitx5` nhận phím bạn gõ.
2. Addon `openkey.so` chuyển chuỗi phím ASCII sang core OpenKey.
3. Core OpenKey xử lý Telex/VNI/Simple Telex, kiểm tra dấu, chính tả, bảng mã.
4. Kết quả được trả lại ứng dụng theo một trong các cách sau:

- `surrounding text`: sửa trực tiếp từ đang gõ nếu ứng dụng hỗ trợ tốt
- `preedit`: hiện vùng gõ tạm, phù hợp hơn với browser/Electron
- `NonPreedit`: không hiện preedit, dùng backspace thật rồi commit lại chữ đã sửa
- `backspace rewrite`: fallback khi helper `NonPreedit` không sẵn sàng

Ở chế độ tự động, OpenKey chọn mode khi focus vào ô nhập liệu. Mặc định OpenKey ưu tiên `NonPreedit` nếu helper server kết nối được. Riêng browser trên X11 sẽ dùng `preedit` để tránh lỗi rewrite/backspace trong thanh nhập liệu. Nếu helper không chạy được hoặc không có quyền `/dev/uinput`, OpenKey sẽ rơi về `backspace rewrite`.

Vì vậy nếu "cài xong nhưng không gõ được", nguyên nhân thường không nằm ở core OpenKey mà nằm ở một trong các lớp sau:

- `fcitx5` chưa chạy đúng
- môi trường input method chưa trỏ sang `fcitx5`
- frontend GTK/Qt chưa có
- addon chưa được add vào `fcitx5`
- thiếu Go khi build nên helper `NonPreedit` không được tạo
- `/dev/uinput` chưa có hoặc không đủ quyền
- ứng dụng hiện tại không hợp với mode đang dùng

## Tính năng chính

- Kiểu gõ `Telex`, `VNI`, `Simple Telex`
- Bảng mã `Unicode`, `TCVN3`, `VNI Windows`
- Kiểm tra chính tả
- Quick Telex
- Modern orthography
- Macro
- Chuyển mode theo từng ứng dụng
- Hotkey đổi mode compose, mặc định là `Alt+Space`

## Yêu cầu

Bạn cần tối thiểu:

- Linux có `fcitx5`
- `cmake >= 3.15`
- compiler C++17
- `pkg-config`
- thư viện dev của `fcitx5`
- Go `1.21+` nếu muốn build helper `openkey-nonpreedit-server`

Nếu muốn `NonPreedit` hoạt động ổn định thì cần:

- có thiết bị `/dev/uinput`
- user hiện tại có quyền truy cập `/dev/uinput`

## Cài dependencies

Tên gói có thể khác nhẹ giữa các version distro, nhưng các lệnh dưới đây là điểm bắt đầu đúng cho đa số máy.

### Ubuntu / Debian

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential \
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

Nếu thiếu gói dev chi tiết, có thể thử:

```bash
sudo apt-get install -y fcitx5-dev
```

Nếu máy chưa dùng `fcitx5` làm input method mặc định:

```bash
im-config -n fcitx5
```

Sau đó đăng xuất và đăng nhập lại.

### Fedora

```bash
sudo dnf install \
  gcc-c++ \
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

### Arch Linux

```bash
sudo pacman -S --needed \
  base-devel \
  cmake \
  pkgconf \
  extra-cmake-modules \
  gettext \
  go \
  fcitx5-im \
  fcitx5-configtool
```

## Cài OpenKey

### Cách nhanh nhất trên Debian / Ubuntu

Repo có sẵn script:

```bash
./scripts/install.sh
```

Script này sẽ:

- cài dependencies bằng `apt`
- build addon
- build và cài `openkey-nonpreedit-server`
- cài vào hệ thống
- cấu hình quyền cơ bản cho `/dev/uinput`
- restart `fcitx5`

Nếu sau khi chạy script mà vẫn báo `permission denied` với `/dev/uinput`, hãy đăng xuất rồi đăng nhập lại để group mới có hiệu lực.

### Cài thủ công cho mọi distro

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
sudo cmake --install build
```

Nếu CMake tìm thấy `go`, lệnh build sẽ tạo thêm binary `openkey-nonpreedit-server` và lệnh install sẽ cài nó vào `libexec`. Nếu không có Go, addon vẫn build được nhưng mode tự động sẽ dùng fallback thay vì helper `NonPreedit`.

Khởi động lại `fcitx5`:

```bash
fcitx5 -rd
```

### Cài vào user local, không đụng system-wide

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build -j
cmake --install build
fcitx5 -rd
```

## Bật OpenKey trong Fcitx5

Sau khi cài xong:

1. Mở `fcitx5-configtool`
2. Chọn `Add input method`
3. Tìm `OpenKey`
4. Add vào danh sách input method

Nếu chưa thấy `OpenKey`:

- chạy lại `fcitx5 -rd`
- đăng xuất / đăng nhập lại
- kiểm tra addon đã được cài đúng chưa

## File cấu hình quan trọng

- `~/.config/fcitx5/conf/openkey.conf`: cấu hình chính
- `~/.config/fcitx5/conf/openkey-appmodes-x11.conf`: mode theo ứng dụng trên X11
- `~/.config/fcitx5/conf/openkey-appmodes-wayland.conf`: mode theo ứng dụng trên Wayland

Nếu bật macro, file macro được lấy theo đường dẫn bạn set trong cấu hình OpenKey.

## NonPreedit server và uinput

`openkey-nonpreedit-server` là helper nền dùng cho mode `NonPreedit`. Addon tự spawn helper theo đường dẫn đã được CMake nhúng vào lúc build, mặc định là:

```text
${CMAKE_INSTALL_FULL_LIBEXECDIR}/openkey-nonpreedit-server
```

Helper mở `/dev/uinput`, tạo virtual keyboard, đăng ký các key chuẩn `1..255` để tránh GNOME/Mutter thu hẹp global keymap, rồi chỉ phát `BackSpace` theo transaction mà addon gửi qua Unix socket.

Mặc định socket là:

```text
/tmp/openkey-nonpreedit.sock
```

Các biến môi trường hữu ích khi debug:

```bash
FCITX_OPENKEY_DEBUG=1
OPENKEY_NONPREEDIT_SERVER_LOG=1
OPENKEY_NONPREEDIT_SERVER_SOCK=/tmp/openkey-nonpreedit.sock
OPENKEY_NONPREEDIT_SERVER_BIN=/path/to/openkey-nonpreedit-server
```

Khi `fcitx5` tắt bình thường, addon sẽ gửi `SIGTERM` và reap helper. Trên Linux, helper child cũng được gắn parent-death signal, nên nếu `fcitx5` thoát bất ngờ thì helper cũng nhận `SIGTERM`.

## Wayland / GNOME

Trên GNOME Wayland, một số app không cung cấp tên app rõ ràng cho `fcitx5`, làm việc chọn mode kém chính xác. Repo có sẵn GNOME Shell extension để bridge app đang focus qua D-Bus.

Cài extension:

```bash
./extension/install.sh
gnome-extensions enable openkey-bridge@openkey.dev
```

Sau đó relogin hoặc restart GNOME Shell nếu cần.

Không phải máy nào cũng cần extension này. Chỉ nên cài nếu bạn thấy mode tự chọn bị sai nhiều trên GNOME Wayland.

## Lỗi thường gặp

### 1. Cài xong nhưng không thấy OpenKey trong `fcitx5-configtool`

Nguyên nhân thường gặp:

- chưa `cmake --install`
- `fcitx5` chưa reload
- cài local nhưng session chưa thấy đường dẫn mới

Cách xử lý:

```bash
fcitx5 -rd
```

Nếu vẫn chưa thấy, đăng xuất rồi đăng nhập lại.

### 2. Gõ không ra tiếng Việt

Thường là do `fcitx5` chưa thật sự là input method đang chạy.

Kiểm tra:

- chỉ dùng một bộ gõ chính, tránh chạy song song `ibus` hoặc bộ gõ khác
- trên Debian/Ubuntu, chạy `im-config -n fcitx5`
- đảm bảo đã cài frontend GTK/Qt của `fcitx5`
- relogin sau khi đổi input method

### 3. Chỉ gõ được ở terminal, không gõ được ở app GUI

Nguyên nhân thường là thiếu frontend:

- thiếu `fcitx5-frontend-gtk3`
- thiếu `fcitx5-frontend-qt5`
- session GUI chưa nhận biến môi trường sau khi cài

Cách xử lý tốt nhất là cài đủ frontend rồi relogin.

### 4. Gõ bị lặp chữ, ăn backspace sai, hoặc app xử lý dấu kỳ lạ

Một số app, nhất là browser / Electron / app trên Wayland, không xử lý backspace hoặc commit text giống nhau. OpenKey có cơ chế tự chọn mode và lưu mode theo ứng dụng, nhưng vẫn có app khó chiều.

Cách xử lý:

- thử đổi mode bằng `Alt+Space`
- với GNOME Wayland, cân nhắc cài extension bridge
- kiểm tra `/dev/uinput` và quyền user nếu đang dùng `NonPreedit`
- restart `fcitx5`

### 5. Không gõ được trong một số ô nhập liệu web

Đây thường là vấn đề mode chứ không phải addon hỏng.

Nếu lỗi chỉ xảy ra ở thanh địa chỉ của browser, nhất là Microsoft Edge trên
Wayland, nguyên nhân có thể là autocomplete / inline suggestion của browser ăn
mất `Backspace` đầu tiên. Triệu chứng thường gặp:

- `go` + `x` ra `goõ` thay vì `gõ`
- `d` + `d` ra `dđ` thay vì `đ`
- `e` + `e` ra `eê` thay vì `ê`

Workaround nhanh nhất trên Edge:

1. Mở `edge://settings/privacy/services/search/searchFilters`
2. Tắt tùy chọn  
   `Show suggestions from history, favorites and other data on this device using your typed characters`

Lưu ý:

- workaround này chủ yếu nhắm vào `address bar`
- ô nhập liệu trong trang web có thể vẫn hoạt động bình thường
- nếu chỉ tắt setting này mà hết lỗi, thì vấn đề nằm ở lớp suggestion của
  browser chứ không phải logic Telex/VNI

Nên thử:

- đổi mode compose bằng `Alt+Space`
- đóng app rồi mở lại
- nếu đang GNOME Wayland, cài bridge extension

### 6. `/dev/uinput` không tồn tại

Kiểm tra:

```bash
ls -l /dev/uinput
```

Nếu chưa có:

```bash
sudo modprobe uinput
```

Nếu distro không tự nạp module này lúc boot, bạn cần tự cấu hình hệ thống để nạp `uinput`.

### 7. `/dev/uinput: permission denied`

Đây là lỗi rất hay gặp khi `NonPreedit` cần helper bơm `BackSpace` qua `uinput`.

Cách xử lý:

- thêm user vào group có quyền dùng `uinput`
- tạo udev rule cho `/dev/uinput`
- đăng xuất / đăng nhập lại sau khi thêm group

Script `./scripts/install.sh` đã cố gắng làm sẵn phần này trên Debian/Ubuntu.

Nếu helper không mở được `/dev/uinput`, triệu chứng có thể là mode tự động rơi về fallback, hoặc trong bản debug sẽ thấy log kiểu `uinput backspace failed`.

### 8. Gõ được lúc đầu, sau đó một số app không nhận đúng mode

OpenKey lưu mode theo ứng dụng riêng cho từng backend:

```bash
~/.config/fcitx5/conf/openkey-appmodes-x11.conf
~/.config/fcitx5/conf/openkey-appmodes-wayland.conf
```

Nếu file của backend hiện tại chứa mode không phù hợp với app hiện tại, bạn có thể sửa tay hoặc xóa file đó để OpenKey học lại từ đầu.

### 9. Không chắc lỗi nằm ở OpenKey hay ở `fcitx5`

Trên Debian/Ubuntu, repo có script cài lại stack `fcitx5`:

```bash
./scripts/reinstall_fcitx5.sh
```

Script này phù hợp khi môi trường `fcitx5` trên máy đã rối từ trước.

## Gỡ cài đặt

```bash
./scripts/uninstall.sh
```

Nếu bạn đã cài bằng build khác hoặc prefix khác, xem help:

```bash
./scripts/uninstall.sh --help
```

## Build để test nhanh

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

Có thể chạy riêng test adapter:

```bash
./build/tests/openkey_adapter_tests
```

## Mã nguồn liên quan

- `src/`: addon Linux cho `fcitx5`
- `Sources/OpenKey/engine/`: core xử lý tiếng Việt
- `tools/openkey-nonpreedit-server/`: helper Go cho mode `NonPreedit`
- `extension/`: GNOME Shell bridge cho Wayland
- `scripts/`: script cài, gỡ và sửa môi trường `fcitx5`

## Giấy phép

Dự án dùng giấy phép `GPL`.
