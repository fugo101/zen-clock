# Clang-Tidy Warning Suppressions

Reference for suppressing warnings in this ESP32 embedded C project.

---

## `pio check` ignores `.clang-tidy` unless told not to

**Verified directly (2026-08-12), not documented anywhere in PlatformIO's own docs at the
time of writing:** PlatformIO's `clangtidy` check tool always invokes clang-tidy with its own
`--checks=*` flag first (confirmed via `pio check -v`), which enables **every** clang-tidy
check across every module (`llvm-*`, `modernize-*`, `cppcoreguidelines-*`, etc.) regardless of
what `.clang-tidy` says — clang-tidy uses whichever `-checks=`/`--checks=` occurrence comes
**last** on the command line, and `.clang-tidy`'s own `Checks:` key is only consulted when
there is no `-checks=` on the command line at all. Running plain `pio check -e
lilygo-t-display-s3` therefore reports ~600+ findings from checks this project never enabled —
and this is exactly what CLion's "Static Code Analysis" action runs under the hood, with no UI
affordance to pass extra flags.

**Fix:** `platformio.ini`'s `[env:lilygo-t-display-s3]` sets `check_tool`, `check_src_filters`,
and `check_flags` (the last one carrying an explicit `-checks=` override that mirrors
`.clang-tidy`'s `Checks:` string) — this applies to *every* caller of `pio check`, CLI or
CLion, with zero extra steps. `.clang-tidy` is still what clangd uses for live inline linting
in the editor. **The two `Checks:` strings cannot be derived from one another** — both files
are static and neither can shell out to the other at parse time — so this is a deliberate,
enforced duplication, not an oversight: run `python3 scripts/pio_check.py` (instead of `pio
check` directly) for CI or a full local run; it fails loudly if the two strings have drifted
apart, then runs the actual check with `--fail-on-defect medium` (a flag `platformio.ini`
cannot express, since there is no `check_fail_on_defect` project option).

---

## Suppression Methods

### 1. Inline — single line

```c
int x = foo(); // NOLINT(check-name)
```

### 2. Next line

```c
// NOLINTNEXTLINE(check-name)
void my_function(void *arg)
```

Use when the comment cannot go on the same line (e.g. multi-line function signatures).

### 3. Unknown check — suppress all on that line

```c
for (;;) // NOLINT
```

### 4. Global — disable in `.clang-tidy`

```yaml
Checks: "-*,...,-check-name"
```

Use for checks that don't apply to the entire project.

---

## Finding the Check Name

The check name always appears in `[...]` at the end of the warning:

```
wifi_manager.c:42:5: warning: message [bugprone-infinite-loop]
```

Copy that name into `NOLINT(...)`.

---

## Common Warnings

### Endless loop

**Cause:** Intentional `for (;;)` or `while (1)` in FreeRTOS tasks.

**Inline fix:**
```c
for (;;) // NOLINT
{
    vTaskDelay(pdMS_TO_TICKS(10));
}
```

**Note:** `bugprone-infinite-loop` is already disabled globally in `.clang-tidy`. If the warning still appears, check the exact name from the diagnostic and add NOLINT.

---

### Unused parameter — `misc-unused-parameters`

**Cause:** FreeRTOS callbacks require `void *arg` in their signature.

**Fix:** cast to void in the body:
```c
static void my_task(void *arg)
{
    (void)arg;
}
```

---

### Parameter cannot be const — `readability-non-const-parameter`

**Cause:** Callback signature dictated by an external API.

**Inline fix:**
```c
static void on_event(void *arg) // NOLINT(readability-non-const-parameter)
```

---

### Function too complex — `readability-function-cognitive-complexity`

**Cause:** State machines, event handlers, FreeRTOS task loops.

**Inline fix:**
```c
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void wifi_task(void *arg)
```

**Current threshold:** 50 (configured in `.clang-tidy`).

---

### Narrowing conversion — `bugprone-narrowing-conversions`

**Cause:** `strlen()` returns `size_t` (unsigned) passed to an `int` parameter.

**Fix:** explicit cast:
```c
func(..., (int)strlen(s_password), ...);
```

---

### Parameter always equals a constant value

**Cause:** Internal function called from one place with a fixed constant.

**Inline fix at call site:**
```c
ap_count = do_aggregated_scan(s_ap_list, MAX_UNIQUE_APS); // NOLINT
```

**Better fix:** remove the parameter and use the constant directly inside the function.

---

### Enum zero-value init — `*-invalid-enum-default-initialization`

**Cause:** ESP-IDF struct partially initialized; unset enum fields zero-initialized, but the enum has no `0` enumerator.

**Two patterns:**

1. `deconfigure = true` path — other fields irrelevant, no valid value to assign:
```c
// NOLINTNEXTLINE(*-invalid-enum-default-initialization)
const ledc_timer_config_t timer_cfg = {
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .timer_num  = handle->timer_num,
    .deconfigure = true,
};
```

2. Default/optional field (e.g. `clk_src = 0` means "use default"):
```c
// NOLINTNEXTLINE(*-invalid-enum-default-initialization)
const adc_oneshot_unit_init_cfg_t adc_cfg = {
    .unit_id = BAT_ADC_UNIT,
};
```

**Note:** Both are false positives — ESP-IDF uses `0` as an implicit "default" sentinel that isn't a named enumerator.

---

### Null pointer dereference — `clang-analyzer-core.NullDereference`

**Cause:** `void *data` from an ESP event handler can be NULL.

**Fix:** add a null guard:
```c
if (!data) { break; }
const wifi_sta_config_t *cfg = (wifi_sta_config_t *)data;
```

---

### Identifier case style — `readability-identifier-naming`

**Cause:** Variable or function name does not match the configured convention.

**Current config** (`.clang-tidy`):
- Functions, variables: `lower_case`
- Macros, enum constants: `UPPER_CASE`
- Static constants: `UPPER_CASE`

**Fix:** rename to match convention, or suppress:
```c
int myVar; // NOLINT(readability-identifier-naming)
```

---

### Signed bitwise on `ESP_LOGx` — `bugprone-signed-bitwise`

```
Clang-Tidy: Use of a signed integer operand with a binary bitwise operator
```
reported on every `ESP_LOGI(...)` / `ESP_LOGW(...)` line.

**Disabled globally in `.clang-tidy`** (`-bugprone-signed-bitwise`).

Watch the check name here. The rule is historically known as `hicpp-signed-bitwise`, and this
project enables no `hicpp-*` checks — but current LLVM ships it under **both** names as aliases,
and `.clang-tidy` enables `bugprone-*`, so it was *our own* config turning it on. Toggling CLion's
"Prefer .clang-tidy files over IDE settings" therefore made no difference in either position.
Verified with the clang-tidy that CLion bundles
(`CLion.app/Contents/bin/clang/mac/aarch64/bin/clang-tidy --list-checks` lists both names).

Do *not* add `NOLINT` comments for this. The finding is not in our code at all — the signed
operands are inside the IDF header, and the diagnostic only points at our line because that is
where the macro expands. In `esp_log.h:253`:

```c
esp_log(ESP_LOG_CONFIG_INIT((configs) | ESP_LOG_CONFIGS_DEFAULT | ESP_LOG_CONFIG_CONSTRAINED_ENV), ...)
```

- `configs` is `ESP_LOG_INFO`, an `esp_log_level_t` enum constant — in C an enum constant has
  type `int`, i.e. **signed**
- `ESP_LOG_CONFIG_CONSTRAINED_ENV` is `(1 << ESP_LOG_OFFSET_CONSTRAINED_ENV)` — the literal `1`
  is a signed `int`
- likewise `ESP_LOG_LEVEL_MASK` is `((1 << ESP_LOG_LEVEL_LEN) - 1)`

So the OR chain is `int | int | int`. Suppressing it at the call site would mean a `NOLINT` on
essentially every log statement in the codebase.

**The same check also fires on our event-group code** for the same reason — `BIT0`…`BIT4` are
defined as plain hex literals (`0x00000001`), which are `int`, so `BIT_STOP | BIT_GOT_IP` is
also "signed bitwise". Harmless: every operand is a small positive constant, nothing shifts into
the sign bit, and the result is assigned to `EventBits_t` (`uint32_t`). The rule exists to catch
sign extension of *negative* values, which cannot occur here.

---

### Identical branches on `ESP_LOGx` / ternary assignments — `bugprone-branch-clone`

**Disabled globally in `.clang-tidy`** (`-bugprone-branch-clone`), found and fixed when wiring
`pio check` into CI (2026-08-12). Verified false-positive on two unrelated patterns in this
codebase, not a check worth suppressing call-site by call-site:

1. **Every `ESP_LOGx(...)` call.** `ESP_LOGW`/`ESP_LOGE`/`ESP_LOGI`/`ESP_LOGD` expand through
   `ESP_LOG_LEVEL_LOCAL` → `ESP_LOG_LEVEL`, which is itself a long `if/else if` chain comparing
   the compile-time-fixed log level against every possible level, each branch calling
   `esp_log(...)` with an almost-identical shape (same call, different embedded format-string
   level letter). `bugprone-branch-clone` walks into that expansion and reports "repeated
   branch body in conditional chain" at the call site, in *our* file — verified directly with
   `clang-tidy` invoked without PlatformIO's summarizer, which shows the diagnostic's macro
   expansion trace running straight through `esp_log.h`. Every `ESP_LOGx` call in the project
   triggers this; disabling call-site by call-site was not an option.
2. **Ternary-assignment action mapping**, e.g. `src/app_handlers.c`:
   ```c
   if (btn_id == BSP_BTN_BOOT)
   {
     action = (event == BSP_BTN_SHORT) ? NAV_ACTION_UP : NAV_ACTION_SELECT;
   }
   else
   {
     action = (event == BSP_BTN_SHORT) ? NAV_ACTION_DOWN : NAV_ACTION_BACK;
   }
   ```
   Flagged as "if with identical then and else branches" despite `NAV_ACTION_UP/DOWN/SELECT/
   BACK` being four genuinely distinct enum values (checked `nav.h` directly to rule out an
   accidental duplicate value — they are 0/1/2/3) — the two branches produce different,
   necessary behavior. The check appears to match on statement *shape* here rather than fully
   resolving the leaf enum constants.

---

### Unused parameter through a cast — `misc-unused-parameters` false positive

Three confirmed false positives, all on a parameter that **is** used, just via a cast rather
than a direct read — a plain C-style cast, not only the `auto`-typed alias form:

```c
static int rssi_compare(const void *a, const void *b) // NOLINT(misc-unused-parameters)
{
  const auto ap_a = (const wifi_ap_record_t *) a; // `a` is used right here
  ...
}
```
```c
// NOLINTNEXTLINE(misc-unused-parameters) - out_ssid is used below via nvs_get_str
bool wifi_cred_load(char *out_ssid, size_t ssid_len, char *out_pass, size_t pass_len)
```
```c
// The button id and event are packed into usr_data at registration
static void button_cb(void *arg, void *usr_data) // NOLINT(misc-unused-parameters)
{
  const uintptr_t packed = (uintptr_t) usr_data;
  ...
}
```
All three parameters are read a few lines later in the function body — the third one (a plain
`(int)(intptr_t) arg` cast, no `auto` involved) rules out "only `auto`-typed aliasing confuses
the check" as the root cause; it is broader than that. Root cause not fully isolated (this
version of `misc-unused-parameters` seems to lose track of a parameter once it is only ever
read inside a cast expression rather than a bare reference), confirmed false rather than
assumed by re-reading each function body directly. Suppressed at the three call sites found;
if another shows up, check the function body before trusting the warning.

---

### `IRAM_ATTR` attribute misread as a variable — `readability-identifier-naming`

```c
// NOLINTNEXTLINE(readability-identifier-naming) - misreads the IRAM_ATTR attribute macro as a variable
static void IRAM_ATTR gpio_isr_handler(void *arg)
```

`readability-identifier-naming`'s case-style check treats `IRAM_ATTR` (ESP-IDF's placement
attribute macro, `__attribute__((section(...)))`) as if it were a variable named `IRAM_ATTR`
needing `lower_case`, because of where it sits syntactically between the return type and the
function name. **No call site in the project uses `IRAM_ATTR` on a definition today** — the one
that did (`gpio_isr_handler` in `components/bsp/src/bsp_buttons.c`) went away when button timing
moved to `espressif/button`. The quirk is kept documented because the next ISR written here will
hit it again; the snippet above is historical, not a live reference.

---

### `app_main` can be made static — `misc-use-internal-linkage`

**Do not follow this one.** `app_main` is the application entry point and the IDF startup task
reaches it by external linkage:

```c
// framework-espidf/components/freertos/app_startup.c:198
extern void app_main(void);
app_main();
```

Marking it `static` gives it internal linkage and the firmware fails to link.

Note the same trap as `bugprone-signed-bitwise` below: `misc-use-internal-linkage` is **not**
enabled by this repo's `.clang-tidy` (the `misc-*` family is opted into one check at a time —
only `misc-redundant-expression` and `misc-unused-parameters`). If you see it, it is coming from
the IDE's own profile, not from the project. Suppressed at the definition with a
`NOLINTNEXTLINE(misc-use-internal-linkage)` plus a comment, so it stays suppressed regardless of
which config turned it on.

The check is correct in general — any file-scope helper that really is single-translation-unit
should be `static`. It is only wrong for entry points and for symbols an external caller resolves.

---

### Variable set but never read

Clangd reports this outside `.clang-tidy` entirely. Treat it as a real finding, not a style nit:
a file-scope pointer that is only ever assigned is state the reader has to account for and
nothing benefits from.

Encountered on `s_sta_netif` in `wifi_manager.c`, which held the return of
`esp_netif_create_default_wifi_sta()` and was never read. Fixed by dropping the variable and
discarding the return — the handle is recoverable via
`esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")` if a deinit path is ever added.

---

## Do Not Suppress

| Warning | Reason |
|---|---|
| `cert-err34-c` | Detects string-to-number conversion errors |
| `cert-flp30-c` | Float loop counters cause undefined behavior |
| `clang-analyzer-core.NullDereference` | Real crash on device |
| `bugprone-narrowing-conversions` | Silent data loss |
