# Clang-Tidy Warning Suppressions

Reference for suppressing warnings in this ESP32 embedded C project.

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
