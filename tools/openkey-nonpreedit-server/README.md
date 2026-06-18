# OpenKey NonPreedit Server

Server Go để nhận transaction `PLAN` từ addon `NonPreedit` mode, tự bơm
`BackSpace` thật qua `/dev/uinput`, rồi phát lại `DONE` cho addon.

Addon hiện sẽ:

- tự tính `newWord` như cũ trong C++
- tự spawn server nếu socket `/tmp/openkey-nonpreedit.sock` chưa sẵn sàng
- giao phần `BACKSPACE + timer` sang server khi helper kết nối được
- fallback sang mode rewrite cũ nếu helper không sẵn sàng

## Chạy thử

```bash
cd tools/openkey-nonpreedit-server
go run . -socket /tmp/openkey-nonpreedit.sock
```

Nếu muốn đổi socket:

```bash
OPENKEY_NONPREEDIT_SERVER_SOCK=/tmp/openkey-nonpreedit.sock
```

Mặc định server sẽ cố chạy với process priority cao hơn (`nice -10`) trên
Linux. Nếu không có quyền `CAP_SYS_NICE` hoặc root, thao tác này sẽ fail
best-effort và server vẫn chạy bình thường. Có thể tắt bằng:

```bash
OPENKEY_NONPREEDIT_SERVER_PRIORITY=0
go run . -priority=false -socket /tmp/openkey-nonpreedit.sock
```

Với bản cài ở `/usr/libexec`, có thể cấp quyền priority bằng:

```bash
sudo setcap cap_sys_nice+ep /usr/libexec/openkey-nonpreedit-server
getcap /usr/libexec/openkey-nonpreedit-server
```

Mặc định server im lặng. Bật log runtime khi cần debug:

```bash
OPENKEY_NONPREEDIT_SERVER_LOG=1
# hoặc
OPENKEY_NONPREEDIT_SERVER_DEBUG=1
```

Server tạo virtual keyboard qua `/dev/uinput` và đăng ký keycode chuẩn `1..255`
để tránh GNOME/Mutter thu hẹp global keymap khi thiết bị ảo được thêm vào.
Khi addon spawn server, server sẽ nhận `SIGTERM` lúc `fcitx5` tắt.

## Protocol

Client -> server:

```text
PLAN <session> <tx> <backspaces> <inter_usec> <commit_delay_usec>
```

Server -> client:

```text
DONE <session> <tx>
```
