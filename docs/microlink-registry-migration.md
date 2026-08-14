# Tách microlink + wireguard_lwip thành managed component trên ESP Component Registry

> Kế hoạch làm việc xuyên nhiều phiên — không phải tài liệu tham khảo tĩnh. Cập nhật file này
> khi có quyết định mới hoặc phase hoàn tất. Xem `docs/DECISIONS.md` để biết bối cảnh vendoring
> hiện tại (submodule + symlink) trước khi đọc kế hoạch này.

## Context

ZenClock hiện dùng microlink (Tailscale client cho ESP32) qua **git submodule + 2 symlink**:

```
.gitmodules                → vendor/microlink @ bac7d62 (github.com/fudio101/microlink)
components/microlink       → symlink 120000 → ../vendor/microlink/components/microlink
components/wireguard_lwip  → symlink 120000 → ../vendor/microlink/components/wireguard_lwip
                                              (bản thân nó cũng là symlink → microlink/components/wireguard_lwip)
```

Chạy được, nhưng khảo sát cho thấy nhiều vấn đề thật:

1. **Chuỗi symlink 2 tầng.** ESP-IDF không được khai báo gì — chỉ tình cờ auto-scan `components/`. Symlink git mode `120000` đòi `core.symlinks=true` + Developer Mode trên Windows.
2. **Pin trùng lặp thủ công.** `THIRD_PARTY.md` ghi tay `**Pinned at** | bac7d62`; không gate nào kiểm tra nó khớp `git submodule status`.
3. **Pin vô hình với mọi cơ chế kiểm soát sẵn có** — lỗ hổng đúng nghĩa, không chỉ chuyện gọn gàng. Build-cache key của CI hash `sdkconfig / platformio.ini / dependencies.lock`; pin submodule nằm trong gitlink tree object, không thuộc file nào trong ba file đó. Cộng với điểm mù response-file của SCons đã ghi trong `docs/DECISIONS.md`, **một lần bump pin có thể phục vụ object đã compile từ microlink cũ.**
4. **Dependency ẩn.** `components/microlink/CMakeLists.txt` khai `REQUIRES espressif__cjson`, nhưng repo microlink **không có `idf_component.yml` nào cả**. Trong zen-clock nó chỉ resolve gián tiếp qua `espressif/network_provisioning`.
5. **`wireguard_lwip` là bản copy diverged, không có lịch sử git thật.** `THIRD_PARTY.md` ghi nhận nó vendor từ `CamM2325/microlink`, vốn tự vendor từ `smartalock/wireguard-lwip` (Daniel Hope, BSD-3) rồi diverge tiếp — hai lớp diverge chồng lên nhau, không ai còn `git blame` được về đúng dòng nào là của ai.
6. **Repo microlink không có CI.** Không `.github/`, không test, không manifest.

Tiền lệ: `gmo-pepabo` — fork khác cũng của `CamM2325/microlink` — publish lên registry ngày 2026-08-03 thành hai component `gmo-pepabo/microlink` (MIT) + `gmo-pepabo/wireguard_lwip` (BSD-3), nhưng cả hai vẫn nằm chung 1 repo, cùng vấn đề #5.

### Quyết định lớn nhất: `wireguard_lwip` tách thành **repo riêng**, fork thật từ `smartalock/wireguard-lwip`

Đã verify trực tiếp (dùng remote `smartalock` có sẵn trong `vendor/microlink`, `git fetch smartalock` rồi diff, bỏ nhiễu CRLF):

```
0	src/crypto.c, crypto.h, tất cả crypto/refc/*, crypto/cortex/*    ← giống hệt upstream
5	src/wireguard-platform.h     ← WIREGUARD_MAX_PEERS 1 → 16 (thật)
0	src/crypto/refc/x25519.c     ← KHÔNG phải divergence của ta — bản vendor cũ THIẾU
                                    một fix mà chính smartalock tự vá sau đó (typo fix,
                                    comment "Fixed typo... MarcusGarfunkel"). Fork thẳng
                                    từ smartalock/main hôm nay tự động có fix này, không
                                    cần replay dòng này.
28	src/wireguard.h              ← callback DERP relay + magicsock UDP output (tính năng thật)
51	src/wireguardif.h            ← API tương ứng: set_derp_output, connect_derp,
                                    inject_packet, is_wireguard_packet, shutdown,
                                    periodic(), disable_socket_bind, set_udp_output
549	src/wireguardif.c            ← implementation của các API trên + ESP-IDF 6.x compat
                                    (netif->state guard, 5 call site theo CLAUDE.md) +
                                    RX-path fix (route qua netif->input) + allowed_source_ips[]
```
Cộng 2 file hoàn toàn mới: `lwip_compat.h` (shim IP-addr cho lwIP 2.2.0/ESP-IDF 6.x), `wireguard-platform-esp32.c` (port ESP32 thay cho `example/wireguard-platform.c` bare-metal của upstream).

**Kết luận: đây là diff cô đọng, có thể replay thành commit sạch trên một fork thật** — không phải viết lại từ đầu. Cách làm:

1. Fork thật trên GitHub: `smartalock/wireguard-lwip` → `fugo101/wireguard-lwip` (giữ tên gốc có gạch ngang, đúng convention fork; component publish lên registry lấy tên `wireguard_lwip` gạch dưới — khớp tên thư mục/dependency key đã dùng khắp nơi, hai tên này độc lập nhau, không nhất thiết trùng).
2. Clone về, checkout đúng `main` (đã có sẵn, verify remote OK).
3. Replay các thay đổi thật (bỏ hẳn phần x25519.c vì đã lỗi thời) thành **các commit Conventional Commits riêng biệt theo nhóm tính năng**, mỗi cụm một PR:
   - `feat: raise WIREGUARD_MAX_PEERS from 1 to 16`
   - `feat: DERP relay + magicsock UDP output callbacks`
   - `feat: packet injection API for magicsock demux`
   - `fix: wireguardif_shutdown cancels timers before free (use-after-free)`
   - `feat: wireguardif_periodic() for external task-driven processing`
   - `compat: ESP-IDF 6.x / GCC 15 (netif->state guard, mbedTLS, -Wunterminated-string-initialization, -Wstringop-overread)`
   - `fix: route decrypted RX through netif->input, not ip_input() (absorbed from CamM2325/microlink PR #20)`
   - `feat: allowed_source_ips[] multi-source allowlist`
   - `build: add ESP-IDF component packaging (CMakeLists.txt, idf_component.yml)`
4. **Mọi commit lên `main` đi qua PR, squash-merge, PR title là Conventional Commit** — đúng quy trình `git-workflow` mà zen-clock đang dùng, áp dụng ngay từ commit đầu tiên của repo mới này (không có lịch sử cũ không-conventional cần xử lý, vì đây là repo mới tinh).
5. Giữ nguyên `LICENSE` gốc (BSD-3-Clause, Daniel Hope) không đổi — đúng tinh thần tôn trọng tác giả. Không thêm license thứ hai cho các file mới (`lwip_compat.h`, `wireguard-platform-esp32.c`, `CMakeLists.txt`, `idf_component.yml`) — coi là đóng góp vào cùng codebase BSD-3, theo đúng cách các fork BSD-3 vẫn làm.

**Lợi ích so với thiết kế "wireguard_lwip là thư mục con trong microlink" trước đó:**
- `git blame`/`git log` chỉ ra chính xác dòng nào của Daniel Hope, dòng nào của FuGo — không còn "diverge-của-diverge" mù mờ.
- GitHub tự hiển thị "forked from smartalock/wireguard-lwip" — ghi công tác giả đúng nghĩa, không chỉ nằm trong một dòng `THIRD_PARTY.md`.
- Microlink giờ chỉ cần **một submodule sạch** `components/wireguard_lwip` trỏ thẳng vào `fugo101/wireguard-lwip.git` — vì component root của repo mới **chính là repo root** (không còn lồng 2 tầng như trước), nên **không cần symlink nào cả**. `EXTRA_COMPONENT_DIRS "../../components"` của 5 example tự tìm thấy `components/wireguard_lwip/CMakeLists.txt` ở đúng một tầng.
- Release/version của `wireguard_lwip` hoàn toàn độc lập với `microlink` — không còn cần chế độ monorepo 2-package phức tạp trong cùng 1 repo (đã thử thiết kế, bỏ vì thừa phức tạp một khi tách repo).

Đã kiểm tra: `smartalock/wireguard-lwip` (bản gốc thật của Daniel Hope) **chưa hề có trên ESP Component Registry** (namespace `smartalock` không tồn tại, 404). Chỉ có `trombik/esp_wireguard` (implementation khác) và `gmo-pepabo/wireguard_lwip` (một fork khác, không có DERP relay/magicsock/16-peer mà microlink cần). → Vẫn cần publish `fugo101/wireguard_lwip`.

### Quyết định đã chốt (cập nhật)

| Hạng mục | Quyết định |
|---|---|
| **wireguard_lwip** | Fork thật từ `smartalock/wireguard-lwip` → `fugo101/wireguard-lwip`, replay thay đổi qua PR, Conventional Commits ngay từ đầu |
| microlink | Transfer `fudio101/microlink` → org `fugo101`, giữ quan hệ fork với `CamM2325/microlink`, checkout ra `~/Projects/microlink`. `components/wireguard_lwip` là **submodule sạch** (không symlink) trỏ vào repo mới |
| Registry | `fugo101/microlink` (MIT) + `fugo101/wireguard_lwip` (BSD-3), **hai repo độc lập, hai release-please độc lập** |
| Version microlink | **3.0.0** — ESP-IDF 6.x-only là breaking change thật, tiếp nối tag `v2.1.0` cũ |
| Version wireguard_lwip | **1.0.0** — publish lần đầu dưới tên này, không có lịch sử v1/v2 nào để tiếp nối; code đã production-tested nên xứng bản ổn định, không phải `0.x` |
| zen-clock | Bỏ submodule + 2 symlink, registry dep `fugo101/microlink: "^3.0.0"`, `override_path` khi hack |
| CI | Mỗi repo (microlink, wireguard-lwip) tự có build/lint riêng; microlink thêm build matrix 5 examples + config-variant; upstream drift watcher cho cả hai chiều (CamM2325 lẫn smartalock) |
| CD | Mỗi repo tự release-please → tag → publish OIDC riêng, **không cần trick "publish đúng thứ tự trong 1 step"** — miễn `wireguard_lwip` publish trước lần đầu microlink cần pin nó |
| Commit | Mọi commit lên `main` (cả 3 repo) qua PR, squash-merge, PR title Conventional Commit — không ngoại lệ |

---

## Phát hiện đã verify — đọc trước khi làm

### A. `REQUIRES microlink` / `PRIV_REQUIRES wireguard_lwip` **giữ short name, không đổi**

Bằng chứng 1 — tarball `gmo-pepabo__microlink-v0.1.0.zip` đã publish, `CMakeLists.txt` nguyên văn dùng `PRIV_REQUIRES wireguard_lwip` (short name) dù manifest khai `gmo-pepabo/wireguard_lwip: version: ==0.1.0`.

Bằng chứng 2 — `_choose_component()` (`idf_component_manager/cmake_component_requirements.py`) rewrite short↔mangled theo cả hai chiều, chạy qua `handle_project_requirements()` (`core.py:954`) trên **mọi** component của **mọi** build. Trong `tools/cmake/*.cmake` không có xử lý namespace nào — 100% ở tầng Python.

| in-repo build (submodule) | registry build |
|---|---|
| known = `wireguard_lwip` → exact match | known = `fugo101__wireguard_lwip` → `endswith('__wireguard_lwip')` → rewrite |

→ **Không sửa REQUIRES ở bất kỳ đâu.** Thêm comment cảnh báo để lần sau không ai "sửa".

### B. Manifest-declared dependency được **tự inject** vào REQUIRES

`managed_components/espressif__network_provisioning/idf_component.yml` khai `espressif/cjson` nhưng `CMakeLists.txt` **không nhắc tên** ở REQUIRES với IDF ≥ 6 — manager tự inject (`core.py:911`). → Đây là lý do microlink *hiện* phải hardcode `espressif__cjson`: nó không có manifest để inject.

### C. `override_path` tính theo **thư mục chứa manifest** (`src/`), không phải project root

`idf_component_tools/sources/local.py:79-89` → chuỗi đúng là `"../../microlink/components/microlink"`. Override cũng serialize vào `dependencies.lock` thành `type: local` + absolute path (gate lockfile bắt được độc lập), và hard-error nếu trỏ vào thư mục không phải component.

### D. `compote manifest lint` **không tồn tại** trong toolchain đang pin

ESP-IDF 6.0.1 ship `idf_component_manager 2.5.0`; `cli/manifest.py` chỉ có `schema`, `create`, `add-dependency`. → Dùng **`compote component pack`** thay thế — chạy đúng validation mà registry dùng, offline, không cần credential.

### E. `CONFIG_ML_*` vẫn sống sót, nhưng qua workaround cần canh chừng

Sau migration `CONFIG_ML_*` đến từ `managed_components/fugo101__microlink/Kconfig` — thư mục **chưa tồn tại ở pass 0** của clean build. Manager snapshot sdkconfig → `sdkconfig.cm` ở pass 0, restore ở pass ≥1 (`core.py:957-1000`). Hoạt động, nhưng là workaround — nếu regress thì `CONFIG_ML_DEVICE_NAME` / `*_BUFFER_SIZE_KB=128` âm thầm reset về default mà firmware **vẫn compile sạch**. → Phải verify tường minh.

### F. Component chỉ được quét **một tầng, không đệ quy**

`__project_component_dir()` (`tools/cmake/project.cmake:451`): nếu thư mục có `CMakeLists.txt` thì dừng ngay, coi nó là component; nếu không thì glob một tầng con. → Đây là lý do submodule `components/wireguard_lwip` phải trỏ **thẳng vào repo có CMakeLists.txt ở root** — không được lồng thêm một tầng nữa, và cũng là lý do quyết định "wireguard_lwip tách repo riêng, component root = repo root" ở trên vừa đúng kỹ thuật vừa đơn giản hơn thiết kế lồng cũ.

### G. 🐛 Bug thật: `CONFIG_ML_ENABLE_CELLULAR=y` KHÔNG build được trên IDF 6.x — **đã verify cứng**

`ESP_IDF_6X_COMPAT.md` §6: cellular sources bị đưa vào `if(CONFIG_ML_ENABLE_CELLULAR)` để *tránh* lỗi missing-header — workaround là **không compile chúng**, chứ không phải sửa dependency thật.

Bằng chứng trên đúng IDF đang pin (`~/.platformio/packages/framework-espidf` = **6.0.1**, khớp `espressif32@7.0.1`):

| Header | Chủ sở hữu duy nhất | Với `REQUIRES driver` |
|---|---|---|
| `driver/uart.h` | `components/esp_driver_uart/include/` | **không tới được** — `driver` không REQUIRES *cũng không* PRIV_REQUIRES `esp_driver_uart` |
| `driver/gpio.h` | `components/esp_driver_gpio/include/` | **không tới được** — `esp_driver_gpio` chỉ nằm ở **PRIV_REQUIRES** của `driver`, private không lan truyền |

`components/driver/CMakeLists.txt` (6.0.1) nguyên văn: `PRIV_REQUIRES esp_timer esp_mm esp_driver_gpio esp_ringbuf esp_pm` / `REQUIRES esp_hal_i2c esp_hal_twai esp_hal_touch_sens`. Include của cả hai header nằm ở `src/ml_cellular.c:24-25` (`uart.h` + `gpio.h`) và `src/ml_at_socket.c:30` (`uart.h`) → **phải khai cả hai**, không chỉ `esp_driver_uart`.

**4/5 examples bật `CONFIG_ML_ENABLE_CELLULAR=y`** (`cellular_connect`, `cellular_heartbeat`, `failover_connect`, `rebind_test`; chỉ `basic_connect` là không) → 4/5 ô matrix đỏ ngay lần CI đầu. zen-clock không bị vì `# CONFIG_ML_ENABLE_CELLULAR is not set` — đó là lý do bug này sống sót đến giờ.

⚠️ **Fix ban đầu SAI, đã tự sửa ở Phase 2 — xem Phát hiện J.** REQUIRES không được phép gate theo Kconfig; đoạn `if(CONFIG_ML_ENABLE_CELLULAR) ... list(APPEND REQUIRES ...)` dưới đây tưởng đúng nhưng **không hoạt động trong bất kỳ môi trường nào** (đã thử cả PlatformIO local và ESP-IDF Docker CI thật). Fix đúng: unconditional REQUIRES — xem Phát hiện J.

### H. `x25519.c` — diff KHÔNG phải divergence, mà là thiếu một fix upstream (đã ghi ở Context)

Fork thẳng từ `smartalock/main` hôm nay đã có fix này miễn phí. Không replay dòng này khi dựng repo `wireguard-lwip` mới.

### I. `allowed_source_ips[]` — xem Finding I ở "Phần 0"

### J. 🐛 `REQUIRES`/`PRIV_REQUIRES` **không thể** gate theo `CONFIG_*` — bài học từ Phase 2

Component-requirements expansion (quyết định `REQUIRES`/`PRIV_REQUIRES`, từ đó quyết định public include-dir nào lan truyền tới ai) chạy ở một pass CMake **trước khi** Kconfig được resolve. Chỉ `SRCS` mới được gate theo `CONFIG_*` (ở pass registration, chạy sau). Đây là hạn chế kiến trúc của ESP-IDF, không phải bug của PlatformIO hay của Docker CI — verify bằng cách thử CẢ HAI, cùng thất bại giống nhau (`driver/uart.h: No such file or directory` dù đã thêm `esp_driver_uart` vào REQUIRES có điều kiện).

Fix đúng: đưa `esp_driver_uart`/`esp_driver_gpio` vào REQUIRES **vô điều kiện**, giống `driver`/`esp_driver_tsens` đã luôn vô điều kiện ngay phía trên — REQUIRES vốn phải thô hơn (coarser-grained) SRCS, đây là bình thường trong ESP-IDF, không phải thoả hiệp.

---

## Phần 0 — Repo `fugo101/wireguard-lwip` — ✅ DONE

Fork thật từ `smartalock/wireguard-lwip`, clone tại `~/Projects/wireguard-lwip`. Component root = repo root (không lồng, không symlink). Replay xong qua 6 PR sạch, squash-merge, đã merge vào `main`:

| PR | Commit |
|---|---|
| [#1](https://github.com/fugo101/wireguard-lwip/pull/1) | `feat(platform): raise WIREGUARD_MAX_PEERS from 1 to 16` |
| [#2](https://github.com/fugo101/wireguard-lwip/pull/2) | `feat(wireguard): add DERP relay and magicsock UDP output API surface` (header-only) |
| [#3](https://github.com/fugo101/wireguard-lwip/pull/3) | `feat(wireguard): implement DERP relay, magicsock output, dual-stack IP compat, and lwIP thread-safety fixes` — file lớn nhất, `wireguardif.c` + `lwip_compat.h` mới, trailer `Upstream-PR: CamM2325/microlink#20` |
| [#4](https://github.com/fugo101/wireguard-lwip/pull/4) | `feat(esp32): add ESP-IDF platform port` — `wireguard-platform-esp32.c` mới |
| [#5](https://github.com/fugo101/wireguard-lwip/pull/5) | `build: add ESP-IDF component packaging` — `CMakeLists.txt` + `idf_component.yml` v1.0.0, `Release-As: 1.0.0` |
| [#6](https://github.com/fugo101/wireguard-lwip/pull/6) | `ci: add manifest validation, release-please, and upstream drift workflows` |

**Kết quả:** `idf_component.yml` version `1.0.0`, license BSD-3-Clause, publish tên component `wireguard_lwip` (gạch dưới; tên repo `wireguard-lwip` gạch ngang — độc lập nhau). `CMakeLists.txt` REQUIRES `lwip esp_timer` (`esp_hw_support` cho `esp_random.h` đã nằm trong common requirements, không cần khai). `files: exclude` cho `src/crypto/cortex/**` + `example/**` — đã verify bằng `compote manifest lint` + `compote component pack` local, cả hai pass, archive sạch.

**CI:** job `manifest validation` chạy pass thật trên PR #6 (dùng `pip install idf-component-manager` mới nhất — có `compote manifest lint`, khác bản 2.5.0 bundle ESP-IDF không có lint — **nên áp dụng lại cho job `manifests` của microlink**). Job `publish-dry-run` set `continue-on-error: true` vì namespace `fugo101` chưa được duyệt (Phase 3, phụ thuộc ngoài, không phải bug). release-please + publish job đã viết sẵn, chưa chạy thật (cần App cài + namespace duyệt ở Phase 3).

**Branch `main` đã protect** (ruleset "Protect main"): chặn xoá/force-push, bắt buộc PR + chỉ squash-merge, linear history, required check `manifest validation`, admin bypass "always".

**Phát hiện quan trọng cho phase sau:** Finding I — `allowed_source_ips[WIREGUARD_MAX_SRC_IPS]` **không còn là divergence**, upstream `smartalock/main` đã có y hệt (cùng kiểu lỗi thời như Finding H/x25519.c). Divergence thật chỉ còn type generalization `ip4_addr_t`→`ip_addr_t` + fallback ưu tiên keypair hợp lệ.

---

## Phần 1 — Repo `fugo101/microlink` — ✅ DONE

Transfer `fudio101/microlink` → org `fugo101` xong (quan hệ fork với `CamM2325/microlink` giữ nguyên), clone tại `~/Projects/microlink`, remote `origin` dùng SSH alias `github-fudio101` (gh mặc định resolve `github.com` trần — phải sửa tay lại, giống bài học ở wireguard-lwip). 6 PR sạch, squash-merge, đã merge vào `main`:

| PR | Commit |
|---|---|
| [#8](https://github.com/fugo101/microlink/pull/8) | `build: replace wireguard_lwip symlink chain with a clean submodule` — trỏ `fugo101/wireguard-lwip.git`, không lồng, không symlink |
| [#9](https://github.com/fugo101/microlink/pull/9) | `fix: declare esp_driver_uart/esp_driver_gpio for the cellular build` — **fix này SAI, xem #11** |
| [#10](https://github.com/fugo101/microlink/pull/10) | `build: add idf_component.yml and manifest validation CI` — version 3.0.0, pin `fugo101/wireguard_lwip: "^1.0.0"`; job CI `manifest validation` đầu tiên tồn tại trong repo |
| [#11](https://github.com/fugo101/microlink/pull/11) | `ci: add examples build matrix, config variants, and upstream drift watcher` — build matrix thật đầu tiên chạy, bắt được 2 bug thật (xem Phát hiện J + đoạn dưới), cả hai fix nằm trong PR này |
| [#12](https://github.com/fugo101/microlink/pull/12) | `feat!: add release-please, formalize ESP-IDF 6.x-only as a breaking change` — `Release-As: 3.0.0` + `BREAKING CHANGE:`, docs rewrite (README/ESP_IDF_6X_COMPAT/UPSTREAM_PRS, `update.sh` xoá) |

**PR #8, #9 phải merge tay (admin), #10+ merge thường.** Tự tạo ruleset "Protect main" ngay sau transfer (bắt chước wireguard-lwip) NHƯNG làm SAI thứ tự — wireguard-lwip chỉ thêm ruleset ở CUỐI (sau khi CI đã tồn tại), còn ở đây ruleset (với required check `manifest validation`) được tạo TRƯỚC khi CI tồn tại, block chính PR đầu tiên. Sửa bằng cách merge tay PR #8/#9 (được user chấp thuận qua GitHub UI), rồi gộp #10 (idf_component.yml + job `manifest validation` tối thiểu) thành 1 PR tự chứa được check của chính nó — từ #10 trở đi mọi PR merge thường, không cần tay. **Bài học cho lần sau: tạo ruleset yêu cầu required-status-check SAU KHI CI đã tồn tại, không phải ngay sau transfer.**

**Hai bug thật CI thật bắt được ngay lần chạy đầu (PR #11), cả hai fix trong cùng PR đó:**
1. Fix của PR #9 (REQUIRES gate theo `CONFIG_ML_ENABLE_CELLULAR`) **không hoạt động** — verify bằng cả PlatformIO local lẫn ESP-IDF Docker CI thật, cùng lỗi `driver/uart.h: No such file or directory`. Nguyên nhân kiến trúc: xem Phát hiện J. Fix đúng: REQUIRES vô điều kiện.
2. Bug tiền-tồn-tại không liên quan đến migration, chỉ chưa từng có CI để bắt: `ml_config_httpd.c` so sánh `old_count` (uint8_t, max 255) trực tiếp với `ML_CONFIG_MAX_ALLOWED_PEERS` (Kconfig, default 512) → `-Werror=type-limits` vì 255 luôn ≤ 512. Fix: widen `old_count` thành `uint16_t` trước khi so sánh.

**Release PR đã mở tự động:** [#13](https://github.com/fugo101/microlink/pull/13) `chore(main): release 3.0.0`, changelog đúng, `.release-please-manifest.json` → `3.0.0`. **CHƯA merge** — chờ Phase 3 (namespace) + Phase 4 (thứ tự publish).

Branch protection cuối cùng: ruleset "Protect main" đủ rule (deletion/non-fast-forward/PR-squash-only/linear-history/required-check `manifest validation`/admin bypass "always") — đúng target state, chỉ khác thứ tự tạo so với kế hoạch ban đầu (xem bài học trên).

---

## Phần 2 — Repo zen-clock — chuẩn bị xong (2.7), CHƯA merge

Nhánh `build/microlink-registry-migration` (1 commit `feat!: migrate microlink + wireguard_lwip to the ESP Component Registry`), đã push lên `fugo101/zen-clock`, **không mở PR, không merge** — đúng như kế hoạch 2.7, vì `fugo101/microlink`/`fugo101/wireguard_lwip` chưa publish (namespace chưa duyệt). `pio run` trên nhánh này sẽ fail ở bước resolve dependency cho tới khi Phase 4 xong.

Nội dung commit khớp đúng §2.1-2.3 đã phác thảo, không có điều chỉnh nào so với plan:
- Gỡ `.gitmodules`, `vendor/microlink`, symlink `components/microlink` + `components/wireguard_lwip`.
- `src/idf_component.yml` thêm `fugo101/microlink: "^3.0.0"` + comment `override_path` local-only.
- `platformio.ini` → `check_src_filters = +<src/*> +<components/*>` (bỏ 2 exclude cũ, không còn cần vì thư mục không tồn tại nữa).
- `scripts/format.py` → bỏ `"vendor"`, `"components/microlink"`, `"components/wireguard_lwip"`, giữ `"managed_components"`.
- CI (`ci.yml` + `release-please.yml`) → bỏ `submodules: recursive`, thêm gate `override_path` (đặt trước gate docs-only skip, dùng `git ls-files` không phải `find`).
- Docs: `CLAUDE.md` (bảng component, mục Dependencies, cảnh báo short-name REQUIRES + `sdkconfig.cm`), `README.md` (bỏ `--recursive`), `THIRD_PARTY.md` (bỏ hẳn phần vendored-source audit thủ công, gộp vào bảng managed components), `docs/DECISIONS.md` (sửa cảnh báo `fugo101` vs `fudio101` — microlink giờ CŨNG ở `fugo101` sau transfer, không còn khác account với zen-clock).

**Verify local đã chạy, pass:** `scripts/check_secrets.py`, `scripts/format.py --check`, `pio test -e native`. **Chưa chạy** (và chưa thể chạy): `pio run` full build, `pio_check.py`, upload thật — tất cả chờ Phase 4 publish xong.

**Việc phát sinh ngoài kế hoạch:** `docs/microlink-registry-migration.md` (chính file này) hoá ra **chưa từng được commit** ở phiên trước — vẫn là untracked file. Đã commit qua PR riêng ([fugo101/zen-clock#41](https://github.com/fugo101/zen-clock/pull/41), `docs: add the microlink registry migration plan`), merge thường, xong trước khi tạo nhánh 2.7 (đã rebase nhánh 2.7 lên sau khi merge để tránh conflict/mất đồng bộ).

### GitHub auth constraint

~~Token `fudio101` thiếu scope `workflow`~~ — **đã refresh** (`gh auth refresh -h github.com -s workflow -s admin:org`, 2026-08-14): scope hiện tại `admin:org, gist, repo, workflow`. Push file dưới `.github/workflows/` qua HTTPS giờ hoạt động bình thường ở cả 3 repo; `admin:org` thêm để phục vụ Phase 3 (transfer repo, thao tác quyền org). `gh auth switch --hostname github.com --user fudio101` đã chạy để đặt `fudio101` làm active account (trước đó active account trôi sang `nguyenndt-qualgo`) — kiểm tra lại `gh auth status` nếu account active bị đổi giữa các phiên làm việc khác nhau.

---

## Thứ tự thực hiện — chia phase, làm dần

**Kỷ luật thực thi:** dừng lại sau mỗi **phase**, báo cáo kết quả, chờ xác nhận rồi mới sang phase tiếp theo. Các step đánh số bên trong một phase được làm liền mạch trong cùng phase đó, không cần dừng giữa chừng.

### Phase 1 — Chuẩn bị `wireguard-lwip` — ✅ DONE

Chi tiết đầy đủ (PR, commit, phát hiện, điều chỉnh so với phác thảo) đã gộp vào "Phần 0" ở trên.

### Phase 2 — Chuẩn bị `microlink` — ✅ DONE

Chi tiết đầy đủ (PR, commit, 2 bug thật CI bắt được, bài học ruleset-trước-CI, nhánh 2.7 của zen-clock) đã gộp vào "Phần 1" và "Phần 2" ở trên.

### Phase 3 — Xin quyền + hạ tầng thủ công — 🟡 phần tự động hoá xong, chờ 3.1 (việc của user)

| # | Việc | Trạng thái |
|---|---|---|
| 3.1 | **Xin duyệt namespace `fugo101`** qua Namespace Request Form tại <https://components.espressif.com/settings/permissions/> — không self-serve, Espressif duyệt tay, lead time không biết trước, kết quả thông báo qua email + hiện trên chính trang đó | ⛔ **CHƯA nộp** (verify 2026-08-14, xem dưới). **Việc của user — chặn 3.4 + toàn bộ Phase 4/5.** |
| 3.2 | Transfer `fudio101/microlink` → org `fugo101`; `git remote set-url` lại SSH alias | ✅ xong ở Phase 2 |
| 3.3 | release-please GitHub App + secret trên cả hai repo mới | ✅ xong — **không cần làm gì thêm**, xem dưới |
| 3.4 | Sau khi namespace được duyệt: tạo 2 component trong registry UI, gắn trusted uploader (OIDC) cho từng cái | ⛔ chặn bởi 3.1. Giá trị chính xác cần điền: xem bảng dưới |

#### 3.3 — đã xong từ trước, verify tường minh (2026-08-14)

Không cần cài App hay set secret gì thêm: hạ tầng release đã sẵn ở **tầng org**, nên hai repo mới thừa hưởng tự động.

| Hạng mục | Giá trị thật |
|---|---|
| GitHub App | `fugo-release-bot`, installation id `145217510` trên org `fugo101`, `repository_selection = all` → repo mới tự động có quyền, không cần thêm tay |
| Secret | **Org-level**, visibility `ALL`: `RELEASE_APP_ID`, `RELEASE_APP_PRIVATE_KEY` (+ `PAT_TOKEN` không dùng ở đây) |
| Repo-level secret | **Không có cái nào** ở cả `microlink`, `wireguard-lwip`, `zen-clock` — đây là bình thường, đừng "sửa" bằng cách thêm bản sao repo-level |
| Bằng chứng chạy thật | Job `release-please` (`push main`) **success** ở cả hai repo → Release PR được tạo: `wireguard-lwip` [#7](https://github.com/fugo101/wireguard-lwip/pull/7) `chore(main): release 1.0.0`, `microlink` [#13](https://github.com/fugo101/microlink/pull/13) `chore(main): release 3.0.0` |

⚠️ Nếu một phiên sau chạy `gh secret list --repo ...` và thấy trống rồi kết luận "3.3 chưa làm" → **sai**. Phải kiểm `gh secret list --org fugo101` và `gh api /orgs/fugo101/installations`.

#### Cách kiểm namespace đã tồn tại chưa — endpoint đúng

`https://components.espressif.com/api/namespaces/<ns>` **không phải endpoint thật** — nó trả `404 NotFound` cho *mọi* namespace, kể cả `espressif`. Dùng cách này thay thế:

```bash
curl -s "https://components.espressif.com/api/components?namespace=fugo101"
# chưa duyệt : {"error":"NamespaceNotFoundError","messages":["Namespace not found."]}   [404]
# đã duyệt   : JSON array các component (rỗng nếu chưa tạo component nào)
```
Đối chứng đã chạy: `gmo-pepabo` → `200` + array thật; `fugo101`, `fudio101`, và một tên bừa cùng trả `NamespaceNotFoundError` → **`fugo101` thật sự chưa tồn tại, không phải endpoint sai.**

#### 3.4 — giá trị chính xác cần điền (chuẩn bị sẵn, làm ngay khi namespace được duyệt)

Registry UI: username dropdown → **Permissions** → chọn namespace `fugo101` → bảng `Components` nút `+` để tạo component → click tên component → bảng `Trusted Uploaders` nút `+`.

| Component cần tạo | Trusted Uploader → Repository | Branch | Environment |
|---|---|---|---|
| `wireguard_lwip` | `fugo101/wireguard-lwip` — ⚠️ **repo tên gạch NGANG**, component tên gạch DƯỚI, hai thứ khác nhau | để trống | để trống |
| `microlink` | `fugo101/microlink` | để trống | để trống |

Vì sao để trống `Branch`: job `publish-component` chạy trong workflow run do `push` vào `main` kích hoạt, nên OIDC claim `ref` = `refs/heads/main` và điền `main` *về lý thuyết* cũng đúng — nhưng để trống bỏ được một biến số ở lần publish đầu. Publish lỗi **không mất version** (chưa có version nào được tạo), chỉ cần sửa trusted uploader rồi re-run job → có thể siết lại `main` sau khi lần đầu thành công.

#### Hạ tầng publish đã verify sẵn cho Phase 4

- `espressif/upload-components-ci-action@v2` tồn tại thật (tag mới nhất `v2.2.1`); input đang dùng ở cả hai workflow (`namespace`, `components`, `dry_run`) khớp đúng `action.yml`. Không set `api_token` → action tự dùng OIDC, đúng ý định.
- `components:` khai đúng: `wireguard_lwip:.` (component root = repo root) và `microlink:components/microlink`.
- `publish-component` của microlink **không cần** `submodules: recursive` dù repo có submodule `components/wireguard_lwip`: thư mục được pack là `components/microlink`, và nó lấy wireguard_lwip qua registry dep `^1.0.0`, không qua submodule.
- Hai Release PR đều `MERGEABLE`, mọi check *thật* xanh (microlink: 5 example × v6.0.1, `release-v6.0`, 3 config variant, `manifest validation`). `mergeStateStatus` là `UNSTABLE` chỉ vì `publish-dry-run` đỏ — đúng như thiết kế (`continue-on-error: true`, namespace chưa có), và nó **không phải required check** nên không chặn merge.
- Cả hai Release PR chỉ đổi `.release-please-manifest.json` + `CHANGELOG.md`, **không** đổi `idf_component.yml` — đúng, vì version trong manifest đã được set tay đúng bằng version đích ngay từ PR tạo file (`3.0.0` / `1.0.0`). `extra-files` sẽ bắt đầu có tác dụng từ lần release *sau*.

### Phase 4 — Publish theo thứ tự phụ thuộc

| # | Việc |
|---|---|
| 4.1 | Merge Release PR của `wireguard-lwip` trước → tag `v1.0.0` → publish job của repo đó chạy |
| 4.2 | Xác nhận `fugo101/wireguard_lwip` xuất hiện trên components.espressif.com |
| 4.3 | Merge Release PR của `microlink` → tag `v3.0.0` → publish job chạy (pin `^1.0.0` đã tồn tại từ 4.2) |
| 4.4 | Smoke test ở project rác: chỉ khai `fugo101/microlink: "^3.0.0"`, xác nhận cả hai rơi vào `managed_components/` đúng tên trước khi đụng zen-clock thật |

### Phase 5 — Migration zen-clock

| # | Việc |
|---|---|
| 5.1 | Lấy branch đã chuẩn bị ở 2.7, trỏ vào package thật vừa publish, chạy full verification |
| 5.2 | Một PR, review, merge |

**Rủi ro publish nửa vời:** registry append-only, version immutable. `wireguard_lwip` publish trước và độc lập nên rủi ro thấp hơn thiết kế cũ — nếu `microlink` publish lỗi, `wireguard_lwip` đã sẵn sàng không cần làm lại gì. Nếu phát hiện lỗi sau khi đã publish → `compote component yank` rồi publish bản patch tiếp theo.

**Rollback zen-clock:** `git checkout main && git submodule update --init --recursive`, miễn branch chưa merge.

---

## Verification

**wireguard-lwip** — ✅ đã chạy, pass (`compote manifest lint` + `compote -W component pack`, archive sạch). Xem "Phần 0".

**microlink** (Phase 2, chạy local trước khi viết CI). Không có `idf.py` → dùng wrapper PlatformIO trong scratchpad (xem điều chỉnh #2 ở Phase 2) và venv cho `compote`:
```bash
# build: project pio tạm, symlink components/{microlink,wireguard_lwip}, main+sdkconfig.defaults
# copy từ examples/<tên>; chạy 2 lần cho cellular_connect (trước/sau fix 2.2) và 1 lần basic_connect
pio run -e esp32s3-microlink-example

python3 -m venv "$SCRATCH/venv" && "$SCRATCH/venv/bin/pip" -q install idf-component-manager
"$SCRATCH/venv/bin/compote" manifest lint components/microlink/idf_component.yml
"$SCRATCH/venv/bin/compote" -W component pack --project-dir components/microlink --name microlink
```

**zen-clock** (Phase 5):
```bash
rm -rf .pio/build managed_components && pio run -e lilygo-t-display-s3   # COLD
ls managed_components/                    # phải có fugo101__microlink, fugo101__wireguard_lwip
grep -c 'type: local' dependencies.lock   # PHẢI = 0

python3 -c "
import json; d=json.load(open('.pio/build/lilygo-t-display-s3/project_description.json'))
assert 'fugo101__microlink' in d['build_components']
assert 'microlink' not in d['build_components']
for n in ('src','ui'): assert 'fugo101__microlink' in d['build_component_info'][n]['reqs'], n
print('OK')"

# Regression nguy hiểm nhất vì nó IM LẶNG — firmware vẫn compile sạch nếu sai
grep -E '^CONFIG_ML_DEVICE_NAME=|^CONFIG_ML_H2_BUFFER_SIZE_KB=|^CONFIG_ML_JSON_BUFFER_SIZE_KB=' \
  sdkconfig.lilygo-t-display-s3     # kỳ vọng "zen-clock" / 128 / 128
grep -cE '^(# )?CONFIG_ML_' sdkconfig.lilygo-t-display-s3   # kỳ vọng 12

python3 scripts/check_secrets.py && python3 scripts/format.py --check
pio test -e native && python3 scripts/pio_check.py
git diff --exit-code dependencies.lock
git ls-files -s | awk '$1=="120000" || $1=="160000"'   # KHÔNG output

pio run -t upload && pio device monitor
# ML_STATE_CONNECTED, icon ⇄ xanh, System Info có TS IP thật, reconnect WiFi → microlink_rebind() ~7s
```

---

## Điểm chưa verify được / cần quyết định

1. ~~Bug cellular (Phát hiện G)~~ — **đã verify cứng bằng CI thật (Docker ESP-IDF), không chỉ suy luận tĩnh** — nhưng fix ban đầu SAI (REQUIRES gate theo Kconfig không hoạt động, xem Phát hiện J), đã tự phát hiện và sửa trong PR #11.
2. **Namespace `fugo101` chưa tồn tại** — kiểm lại 2026-08-14 bằng endpoint ĐÚNG (`api/components?namespace=`, xem Phase 3; lần kiểm trước dùng `api/namespaces/<ns>` là endpoint không tồn tại nên kết quả 404 đó vô nghĩa): trả `NamespaceNotFoundError`. Cần Espressif duyệt tay qua Namespace Request Form — long pole của cả kế hoạch, chặn 3.4 + Phase 4 + Phase 5. **Việc của user, nên nộp ngay.**
3. ~~`~/Projects/microlink` chưa tồn tại~~ — đã transfer + clone xong, xem "Phần 1".
4. ~~clang-format~~ — đã chốt: bỏ hẳn (§1.4).
5. ~~Copyright `fudio101`~~ — đã chốt: giữ nguyên, không đổi sang FuGo (áp dụng cho `microlink`; `wireguard-lwip` giữ nguyên LICENSE gốc của Daniel Hope, không có dòng copyright FuGo nào để cân nhắc).
6. ~~Token GitHub thiếu scope `workflow`~~ — đã refresh, xem GitHub auth constraint.
7. **IDF 6.1 có phá fork không** — trần `<7.0` vẫn là canh bạc, CHƯA thêm ô matrix `release-v6.1`. Cái đã thêm là `release-v6.0` (early-warning ngược, cho biết IDF 6.0-generic có ổn không) — pass sạch ở PR #11, không phải dữ liệu cho câu hỏi 6.1. *(Còn mở, ưu tiên thấp — không chặn publish)*
8. ~~Ranh giới PR replay~~ — đã thực thi, gộp thành 5 commit thay vì 9 nhóm phác thảo, xem "Phần 0".
9. ~~`gh repo fork` có hoạt động đúng không~~ — có, `gh repo fork smartalock/wireguard-lwip --org fugo101` chạy thành công.
10. **`docs/microlink-registry-migration.md` từng bị bỏ sót, chưa commit từ phiên trước** — đã phát hiện + sửa ở Phase 2 (xem "Phần 2"). Bài học: sau mỗi lần dùng `Edit`/`Write` cho file plan trong repo, luôn `git status` để xác nhận nó đã thực sự nằm trong git, không chỉ nằm trên đĩa.
