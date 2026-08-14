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

### G. 🐛 Bug thật: `CONFIG_ML_ENABLE_CELLULAR=y` gần như chắc chắn không build được trên IDF 6.x

`ESP_IDF_6X_COMPAT.md` §6: cellular sources bị đưa vào `if(CONFIG_ML_ENABLE_CELLULAR)` để *tránh* lỗi missing-header — workaround là **không compile chúng**, chứ không phải sửa dependency thật.

Verify trên IDF 6.0.1: `driver/uart.h` chỉ nằm ở `esp_driver_uart`; `components/driver/CMakeLists.txt` **không** REQUIRES nó. **4/5 examples bật `CONFIG_ML_ENABLE_CELLULAR` trong `sdkconfig.defaults`** → 4/5 ô matrix sẽ đỏ ngay lần chạy CI đầu tiên. Fix:
```cmake
if(CONFIG_ML_ENABLE_CELLULAR)
    list(APPEND SRCS "src/ml_cellular.c" "src/ml_at_socket.c")
    list(APPEND REQUIRES esp_driver_uart esp_driver_gpio)   # ← thiếu
endif()
```

### H. `x25519.c` — diff KHÔNG phải divergence, mà là thiếu một fix upstream (đã ghi ở Context)

Fork thẳng từ `smartalock/main` hôm nay đã có fix này miễn phí. Không replay dòng này khi dựng repo `wireguard-lwip` mới.

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

## Phần 1 — Repo `fugo101/microlink`

### 1.1 `components/wireguard_lwip` giờ là submodule sạch, không symlink

```bash
git rm --cached components/wireguard_lwip                 # bỏ symlink 120000 cũ (nếu còn)
rm -rf components/microlink/components/wireguard_lwip     # bỏ hẳn bản copy lồng cũ
git submodule add https://github.com/fugo101/wireguard-lwip.git components/wireguard_lwip
```
`examples/*/CMakeLists.txt` (`EXTRA_COMPONENT_DIRS "../../components"`) **không đổi** — vẫn glob ra hai component ngang hàng, giờ một cái là component thường (`components/microlink`), một cái là submodule (`components/wireguard_lwip`), cả hai đều có `CMakeLists.txt` ở đúng một tầng.

⚠️ Submodule này **chỉ phục vụ build local/CI của chính repo microlink** (5 examples). Nó **không** phải nguồn sự thật cho consumer — zen-clock (và mọi consumer khác) luôn lấy `fugo101/wireguard_lwip` qua registry, độc lập hoàn toàn với submodule này. Không lặp lại vấn đề #3 ở Context (pin vô hình với build-cache) vì microlink's CI ở đây không có custom SCons cache như self-hosted runner của zen-clock — checkout fresh mỗi lần.

Reference cần sửa: `CLAUDE.md` (mô tả `components/wireguard_lwip/` là submodule, không phải thư mục con), `components/microlink/CMakeLists.txt` — không đổi (`PRIV_REQUIRES wireguard_lwip` vẫn short name, xem Phát hiện A).

### 1.2 `components/microlink/idf_component.yml`

```yaml
version: "3.0.0"  # x-release-please-version
description: >-
  Tailscale-protocol network stack for ESP32 (FuGo fork of CamM2325/microlink,
  ESP-IDF 6.x only): ts2021 control plane, WireGuard data plane, DISCO/STUN,
  DERP relay, optional 4G cellular with WiFi failover.
url: https://github.com/fugo101/microlink
repository: https://github.com/fugo101/microlink.git
issues: https://github.com/fugo101/microlink/issues
license: MIT
tags: [tailscale, wireguard, vpn, networking, cellular]

dependencies:
  idf:
    # Sàn 6.0: src/ml_noise.c include <mbedtls/private/chachapoly.h>, chỉ có trong
    # cây TF-PSA-Crypto từ ESP-IDF 6.0. Trần 7.0: patch compat 6.x-specific.
    version: ">=6.0,<7.0"
  espressif/cjson:
    version: "^1.7.19"
  fugo101/wireguard_lwip:
    # Repo riêng (fugo101/wireguard-lwip, fork thật từ smartalock/wireguard-lwip),
    # release-please độc lập — số này KHÔNG liên quan gì đến version của microlink.
    # Sửa tay khi cần nâng cấp; không có cơ chế tự sync giữa hai repo khác nhau.
    version: "^1.0.0"
```

**`targets:` — bỏ hẳn.** Hard gate: manager từ chối cài trên target ngoài danh sách. Chỉ `esp32s3` được test thật. `gmo-pepabo` cũng publish `targets: []`. → Follow-up 3.1.0 sau khi CI xanh trên `esp32` + `esp32s3`.

**`examples:` — không expose ở 3.0.0.** Registry chỉ auto-discover từ `<component>/examples/`; 5 example ở gốc repo, di chuyển sẽ phá `EXTRA_COMPONENT_DIRS` và tạo vòng lặp chicken-and-egg ở lần publish đầu.

### 1.3 CI — `.github/workflows/ci.yml`

Ba job, `actions/checkout@v7` (recursive submodules cho `components/wireguard_lwip`), không SHA-pin, `dependabot.yml` giống zen-clock.

**Job `examples`** — matrix 5 examples × `{v6.0.1, release-v6.0}`, `fail-fast: false`, target `esp32s3`, `espressif/esp-idf-ci-action@v1`. `v6.0.1` khớp ESP-IDF bên trong `espressif32@7.0.1` mà zen-clock pin; `release-v6.0` là ô cảnh báo sớm, `continue-on-error: true`.

**Job `config-variants`** — 5 examples đã phủ sẵn `ML_ENABLE_CELLULAR` (4/5), `ML_ENABLE_CONFIG_HTTPD` (5/5), `ML_ENABLE_NET_SWITCH` (failover_connect). Chỉ còn thiếu `zerocopy` (`CONFIG_ML_ZERO_COPY_WG=y`, 17 điểm ifdef + `ml_zerocopy.c`), `minimal` (tắt hết), `cellular-zerocopy` (giao của hai cờ). Overlay ở `ci/sdkconfig.ci.*`, copy vào thư mục example rồi build với `-DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci"` (truyền SDKCONFIG_DEFAULTS THAY THẾ ngầm định, phải liệt kê lại).
⚠️ `esp-idf-ci-action` nội suy `command` vào `bash -c '…'` **nháy đơn** — chỉ dùng nháy kép trong chuỗi.

**Job `manifests`** — `compote -W component pack` (thay `manifest lint` không tồn tại), cộng bước shell assert `version:` khớp `.release-please-manifest.json`.

**Build không cần credential thật** — mọi credential là Kconfig string `default ""`, dùng như macro hợp lệ C.

**Sửa kèm:** `examples/failover_connect/main/CMakeLists.txt` thiếu `REQUIRES` — set khớp 4 example còn lại, dù không ảnh hưởng chức năng (main không set REQUIRES thì IDF link toàn bộ), vì đây là ví dụ duy nhất sẽ không phát hiện nếu `microlink` rơi khỏi build.

**Không có clang-format gate** (đã chốt — xem §1.4).

### 1.4 clang-format — bỏ hẳn (đã xác nhận)

```
git diff --stat 216da33..HEAD -- components/   →  14 files changed, 125 insertions(+), 52 deletions(-)
find components -name '*.c' -o -name '*.h' | xargs wc -l   →  22656
```
Fork chỉ đổi 0,55% lượng C/H mà nó ship (con số này đo trước khi tách `wireguard_lwip` ra repo riêng — sau khi tách, tỷ lệ còn thấp hơn nữa vì phần lớn LOC đó chính là `wireguard_lwip`). Format toàn repo phá `git cherry -v upstream/main main`, cơ chế chống double-apply mà `UPSTREAM_PRS.md` dựa vào. Không thêm `.clang-format`, không thêm CI job format nào.

### 1.5 release-please (microlink, độc lập — không còn phức tạp monorepo)

Đơn giản hơn nhiều so với thiết kế đã bỏ (2-package trong 1 repo): giờ chỉ **một package tại repo root**, tiếp nối đúng lịch sử `v2.1.0` cũ.

```json
{
  "release-type": "simple",
  "bump-minor-pre-major": false,
  "bootstrap-sha": "<full SHA của bac7d62>",
  "changelog-sections": [
    { "type": "feat",     "section": "Features" },
    { "type": "fix",      "section": "Bug Fixes" },
    { "type": "perf",     "section": "Performance" },
    { "type": "refactor", "section": "Refactoring" },
    { "type": "docs",     "section": "Documentation" },
    { "type": "compat",   "section": "ESP-IDF compatibility" },
    { "type": "chore",    "section": "Miscellaneous", "hidden": true },
    { "type": "style",    "section": "Styles",        "hidden": true },
    { "type": "test",     "section": "Tests",         "hidden": true },
    { "type": "ci",       "section": "CI",            "hidden": true },
    { "type": "build",    "section": "Build",         "hidden": true }
  ],
  "packages": {
    ".": {
      "release-type": "simple",
      "changelog-path": "CHANGELOG.md",
      "extra-files": [
        { "type": "generic", "path": "components/microlink/idf_component.yml" }
      ]
    }
  }
}
```
`.release-please-manifest.json`: `{".": "2.1.0"}`.

**`extra-files` dùng `type: generic`, KHÔNG dùng `type: yaml`** — `generic-yaml.ts` tự ghi rằng nó *"reformat the document and removes all comments"* và có thể nuốt mất cấu trúc; `generic.ts` là line-based, chỉ thay substring semver đầu tiên trên dòng có `x-release-please-version`, giữ nguyên phần còn lại — comment và cấu trúc YAML sống sót. Dòng pin `fugo101/wireguard_lwip: "^1.0.0"` **không có annotation** — sửa tay khi cần nâng version, đúng như ghi trong §1.2.

**Lịch sử commit không theo Conventional Commits:** 18 commit từ `v2.1.0` release-please parse được **con số 0**. Ba commit gần nhất là cherry-pick nguyên văn với subject của upstream và **không được rewrite** (rewrite là phá `git cherry`). Xử lý bằng `bootstrap-sha` = full SHA của `bac7d62`, không rewrite history. Land commit tái cấu trúc với footer `Release-As: 3.0.0` + `BREAKING CHANGE:` (ESP-IDF 6.x-only).

**Từ nay trở đi: mọi commit trên `main` theo Conventional Commits, không ngoại lệ** — giống policy zen-clock (`.claude/skills/git-workflow/SKILL.md`). Bật squash-merge, PR title = commit message trên `main`, enforce bằng `amannn/action-semantic-pull-request`. Kể cả PR chỉ chứa cherry-pick từ upstream — PR title vẫn phải là Conventional Commit hợp lệ (ví dụ `fix(wireguard_lwip): don't pre-install unvalidated peer endpoints (upstream #21)`); nội dung cherry-pick bên trong nhánh feature (nếu giữ qua `git cherry-pick -x`) vẫn giữ nguyên author/message gốc, chỉ tầng squash-lên-main được chuẩn hoá. Đánh đổi: squash đổi patch-id, `git cherry -v upstream/main main` không còn khớp chính xác sau squash — bù bằng lớp thứ hai đã có sẵn (`git log --grep="cherry picked from"` / trailer `Upstream-PR:`).

### 1.6 CD — publish lên registry

Không còn cần trick "thứ tự trong 1 step CI" như thiết kế cũ — `wireguard_lwip` giờ ở repo khác, release/publish theo nhịp riêng của nó. microlink chỉ cần đảm bảo: **tại thời điểm publish, version `wireguard_lwip` mà nó pin (`^1.0.0`) đã tồn tại trên registry** — điều này đúng theo định nghĩa, vì pin chỉ được viết/sửa sau khi thấy version đó đã publish.

```yaml
  publish-component:
    needs: release-please
    if: needs.release-please.outputs.release_created == 'true'
    permissions:
      contents: read
      id-token: write     # BẮT BUỘC: action mint OIDC token đổi lấy registry token
    steps:
      - uses: actions/checkout@v7
        with:
          ref: ${{ needs.release-please.outputs.tag_name }}
      - uses: espressif/upload-components-ci-action@v2
        with:
          namespace: fugo101
          components: "microlink:components/microlink"
```
Thêm job `dry_run: "true"` trên PR để bắt manifest hỏng trước khi cắt tag.

### 1.7 Upstream drift watcher (chiều `CamM2325/microlink`)

`.github/workflows/upstream-drift.yml`, cron thứ Hai hàng tuần + `workflow_dispatch`. Một issue duy nhất, update tại chỗ, tự close khi sạch.

Logic: `git cherry -v HEAD upstream/main | sed -n 's/^+ //p'` cho commit upstream ta thiếu, cộng `gh pr list --repo CamM2325/microlink --state open` lọc bỏ số PR đã ghi trong `UPSTREAM_PRS.md`.

Trạng thái hiện tại: `git rev-list --left-right --count upstream/main...HEAD` → `0 15`, `UPSTREAM_PRS.md` đã ghi #20–#25, #3, #14 → lần chạy đầu sẽ im lặng hoàn toàn.

### 1.8 Docs

| File | Sửa |
|---|---|
| `README.md:52` | `ESP-IDF v5.0 or later (tested with v5.3)` → **`ESP-IDF 6.0.x (bắt buộc; 5.x không hỗ trợ)`**, kèm lý do `<mbedtls/private/chachapoly.h>` |
| `README.md` | Thêm mục Installation dẫn đầu bằng recipe registry; mục Provenance nêu rõ `wireguard_lwip` giờ là submodule trỏ `fugo101/wireguard-lwip` (fork thật từ Daniel Hope), không còn "vendored copy" |
| `ESP_IDF_6X_COMPAT.md:3` | `Branch: esp-idf-6x-compat` sai — đã merge vào `main` |
| `ESP_IDF_6X_COMPAT.md:129-137` | Thay recipe submodule+symlink cũ bằng recipe registry + ghi chú submodule `wireguard_lwip` mới |
| `update.sh` | Xoá — `UPSTREAM_PRS.md` đã ghi nó nguy hiểm |
| `THIRD_PARTY.md` | Mục `wireguard_lwip` viết lại hoàn toàn: trỏ sang repo `fugo101/wireguard-lwip`, không cần audit table thủ công nữa (git log của repo mới tự làm việc đó) |

---

## Phần 2 — Repo zen-clock

Không đổi nhiều so với thiết kế trước — zen-clock luôn tiêu thụ `fugo101/microlink` qua registry, tách repo `wireguard_lwip` không ảnh hưởng phía này (vẫn là dependency transitive, ẩn hoàn toàn khỏi zen-clock).

### 2.1 Gỡ submodule + symlink

```bash
git submodule deinit -f vendor/microlink      # còn .gitmodules thì mới deinit được
git rm components/microlink components/wireguard_lwip
git rm vendor/microlink
git rm .gitmodules                            # chỉ có 1 entry → xoá cả file
git config --local --remove-section submodule.vendor/microlink 2>/dev/null || true
rm -rf .git/modules/vendor .git/modules/_microlink   # CÓ HAI thư mục — _microlink là rác từ path cũ
```
⚠️ `git rm vendor/microlink` **xoá working tree** → phải clone và push `~/Projects/microlink` xong trước.
⚠️ Clone cũ của người khác **và workspace của self-hosted runner** vẫn còn `vendor/microlink` + `.git/modules/vendor`; `git pull` không dọn. Cần chạy tay một lần.

### 2.2 `src/idf_component.yml`

```yaml
  fugo101/microlink:
    version: "^3.0.0"
    public: true
    # CHỈ DÙNG LOCAL — không bao giờ commit:
    #   override_path: "../../microlink/components/microlink"
    # Path tính theo thư mục CHỨA FILE NÀY (src/), không phải project root, và phải trỏ
    # vào thư mục COMPONENT (có CMakeLists.txt), không phải repo root.
```
`fugo101/wireguard_lwip` vào gián tiếp qua manifest của microlink — **không khai ở đây**.

### 2.3 Còn lại

- **CMakeLists — không đổi** (Phát hiện A). Chỉ thêm comment cảnh báo.
- **`platformio.ini`** → `check_src_filters = +<src/*> +<components/*>`. `managed_components/` không lọt scope vì đây là allow-list dựng từ base rỗng.
- **`scripts/format.py`** → bỏ `"vendor"`, `"components/microlink"`, `"components/wireguard_lwip"`; **giữ `"managed_components"`** — nó chuyển từ thừa thành load-bearing.
- **CI** → bỏ `submodules: recursive` ở cả hai workflow, **giữ `fetch-depth: 0`**. Thêm gate chặn `override_path`:
  ```bash
  git ls-files '*idf_component.yml' | xargs -r grep -nE '^[[:space:]]*override_path[[:space:]]*:'
  ```
  Dùng `git ls-files` (không phải `find`) để `managed_components/**` ngoài scope; regex neo vào YAML key nên dòng comment ở §2.2 không bị bắt. Đặt **trước** gate docs-only skip.
- **Build-cache key giữ nguyên** nhưng nay đã *đúng*: version + `component_hash` của microlink nằm trong `dependencies.lock` mà key đã hash. Nêu rõ trong PR — nó bịt lỗ hổng ở Context #3.
- ⚠️ **`dependencies.lock` phải commit cùng commit đó**, không thì gate `git diff --exit-code` fail 100%.
- **Docs**: `CLAUDE.md` (bảng component, mục Dependencies, 2 cảnh báo mới về short-name REQUIRES và `sdkconfig.cm`), `README.md` (xoá hẳn callout `--recursive`), `THIRD_PARTY.md` (giữ nguyên attribution BSD-3, cập nhật đường link sang `fugo101/wireguard-lwip` — giờ là fork thật, dễ trỏ hơn), `docs/DECISIONS.md` (entry mới + sửa cảnh báo `fugo101` vs `fudio101` ở dòng 449).

### GitHub auth constraint

~~Token `fudio101` thiếu scope `workflow`~~ — **đã refresh** (`gh auth refresh -h github.com -s workflow -s admin:org`, 2026-08-14): scope hiện tại `admin:org, gist, repo, workflow`. Push file dưới `.github/workflows/` qua HTTPS giờ hoạt động bình thường ở cả 3 repo; `admin:org` thêm để phục vụ Phase 3 (transfer repo, thao tác quyền org). `gh auth switch --hostname github.com --user fudio101` đã chạy để đặt `fudio101` làm active account (trước đó active account trôi sang `nguyenndt-qualgo`) — kiểm tra lại `gh auth status` nếu account active bị đổi giữa các phiên làm việc khác nhau.

---

## Thứ tự thực hiện — chia phase, làm dần

**Kỷ luật thực thi:** dừng lại sau mỗi **phase**, báo cáo kết quả, chờ xác nhận rồi mới sang phase tiếp theo. Các step đánh số bên trong một phase được làm liền mạch trong cùng phase đó, không cần dừng giữa chừng.

### Phase 1 — Chuẩn bị `wireguard-lwip` — ✅ DONE

Chi tiết đầy đủ (PR, commit, phát hiện, điều chỉnh so với phác thảo) đã gộp vào "Phần 0" ở trên.

### Phase 2 — Chuẩn bị `microlink`

| # | Việc |
|---|---|
| 2.1 | Thay symlink cũ bằng submodule sạch `components/wireguard_lwip` → `fugo101/wireguard-lwip.git` (§1.1) |
| 2.2 | `fix: declare esp_driver_uart/esp_driver_gpio for the cellular build` (Phát hiện G) — **phải trước CI**, verify bằng `idf.py build` local ở `examples/cellular_connect` |
| 2.3 | `build: add idf_component.yml` (version 3.0.0, pin `fugo101/wireguard_lwip: "^1.0.0"`), verify bằng `compote component pack` local |
| 2.4 | `ci: build matrix, config variants, manifest validation` (không clang-format) |
| 2.5 | `ci: upstream drift watcher` (chiều CamM2325) |
| 2.6 | `feat!: … Release-As: 3.0.0` + release-please config + docs rewrite |
| 2.7 | Chuẩn bị sẵn (nhưng CHƯA áp dụng) toàn bộ thay đổi phía zen-clock — giữ trên branch riêng, không merge tới khi Phase 4 có package thật để trỏ vào |

### Phase 3 — Xin quyền + hạ tầng thủ công (bottleneck thời gian, làm song song Phase 1+2)

| # | Việc |
|---|---|
| 3.1 | **Xin duyệt namespace `fugo101`** tại components.espressif.com/settings/permissions — ⚠️ không self-serve, Espressif duyệt tay, lead time không biết trước. **Nộp đơn ngay, không chờ Phase 1/2 xong.** |
| 3.2 | Transfer `fudio101/microlink` → org `fugo101`; `git remote set-url` lại SSH alias |
| 3.3 | Cài release-please GitHub App lên cả hai repo mới; set secret |
| 3.4 | Sau khi namespace được duyệt: tạo 2 component (`microlink`, `wireguard_lwip`) trong registry UI, gắn trusted uploader (OIDC) cho từng cái |

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

**microlink** (Phase 2, chạy local trước khi viết CI):
```bash
cd examples/basic_connect    && idf.py build
cd examples/cellular_connect && idf.py build    # xác nhận bug ở Phát hiện G
compote -W component pack --project-dir components/microlink --name microlink
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

1. **Bug cellular (Phát hiện G)** là suy luận tĩnh, chưa chạy thật — chạy `idf.py build` ở `examples/cellular_connect` để xác nhận trước khi viết matrix.
2. **Namespace `fugo101` chưa tồn tại** (HTTP 404 đã kiểm tra), cần Espressif duyệt tay — long pole của cả kế hoạch.
3. **`~/Projects/microlink` chưa tồn tại**; cả hai repo mới (`wireguard-lwip`, và transfer của `microlink`) đều chưa được tạo/transfer.
4. ~~clang-format~~ — đã chốt: bỏ hẳn (§1.4).
5. ~~Copyright `fudio101`~~ — đã chốt: giữ nguyên, không đổi sang FuGo (áp dụng cho `microlink`; `wireguard-lwip` giữ nguyên LICENSE gốc của Daniel Hope, không có dòng copyright FuGo nào để cân nhắc).
6. ~~Token GitHub thiếu scope `workflow`~~ — đã refresh, xem GitHub auth constraint.
7. **IDF 6.1 có phá fork không** — trần `<7.0` là canh bạc. Thêm `espressif/idf:release-v6.1` làm ô matrix thứ ba (`continue-on-error`) sẽ biến canh bạc thành dữ liệu. *(Phase 2, chưa làm)*
8. ~~Ranh giới PR replay~~ — đã thực thi, gộp thành 5 commit thay vì 9 nhóm phác thảo, xem "Phần 0".
9. ~~`gh repo fork` có hoạt động đúng không~~ — có, `gh repo fork smartalock/wireguard-lwip --org fugo101` chạy thành công.
