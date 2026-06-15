/*
 * IKEA UPPÅTVIND → Matter over Thread
 * Hardware: Seeed XIAO ESP32-C6
 *
 * GPIO4 → TP4 : symulacja przycisku (open-drain, active-low, puls 100ms)
 * GPIO5 ← TP7 : odczyt LED PWM (pulseIn µs): ~0=off, ~20=low, ~100=med, ~320=high
 *
 * Device type : Air Purifier (0x002D)
 * Clusters    : FanControl (0x0202) – tryby Off/Low/Medium/High
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_matter.h>
#include <esp_matter_endpoint.h>
#include <esp_matter_cluster.h>
#include <esp_matter_attribute_utils.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app-common/zap-generated/ids/Attributes.h>

static const char *TAG = "uppatvind";

// ─── GPIO ────────────────────────────────────────────────────────────────────
#define GPIO_BTN_OUT    GPIO_NUM_4   // TP4: symulacja przycisku (open-drain)
#define GPIO_PWM_IN     GPIO_NUM_5   // TP7: odczyt stanu LED PWM
#define BTN_PULSE_MS    100          // czas pulsu [ms]

// Progi pulseIn [µs] – środki przedziałów między poziomami
// off≈0µs, low≈20µs, med≈100µs, high≈320µs
#define THRESH_LOW_US   10
#define THRESH_MED_US   60
#define THRESH_HIGH_US  210
#define PULSE_TIMEOUT_US 2000

// ─── Matter ──────────────────────────────────────────────────────────────────
using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace chip::app::Clusters;

static uint16_t g_fan_endpoint_id = 0;

// FanMode enum (Matter spec §2.4.6.1)
enum class FanMode : uint8_t {
    kOff    = 0,
    kLow    = 1,
    kMedium = 2,
    kHigh   = 3,
};

static FanMode g_current_mode = FanMode::kOff;

// ─── Odczyt PWM ──────────────────────────────────────────────────────────────
static uint32_t read_pwm_pulse_us(void)
{
    // Czekaj na zbocze narastające (max 5ms)
    const int64_t t_start = esp_timer_get_time();
    while (gpio_get_level(GPIO_PWM_IN) == 0) {
        if ((esp_timer_get_time() - t_start) > 5000) return 0;
        esp_rom_delay_us(1);
    }
    // Mierz czas stanu HIGH
    const int64_t t_high = esp_timer_get_time();
    while (gpio_get_level(GPIO_PWM_IN) == 1) {
        if ((esp_timer_get_time() - t_high) > PULSE_TIMEOUT_US) break;
        esp_rom_delay_us(1);
    }
    return (uint32_t)(esp_timer_get_time() - t_high);
}

static FanMode pwm_to_mode(uint32_t us)
{
    if (us < THRESH_LOW_US)  return FanMode::kOff;
    if (us < THRESH_MED_US)  return FanMode::kLow;
    if (us < THRESH_HIGH_US) return FanMode::kMedium;
    return FanMode::kHigh;
}

// ─── Symulacja przycisku ──────────────────────────────────────────────────────
static void press_button(void)
{
    gpio_set_level(GPIO_BTN_OUT, 0);
    vTaskDelay(pdMS_TO_TICKS(BTN_PULSE_MS));
    gpio_set_level(GPIO_BTN_OUT, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static void cycle_to_mode(FanMode current, FanMode target)
{
    if (current == target) return;
    int clicks = ((int)target - (int)current + 4) % 4;
    ESP_LOGI(TAG, "Klikam %d razy: tryb %d→%d", clicks, (int)current, (int)target);
    for (int i = 0; i < clicks; i++) {
        press_button();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ─── Aktualizacja atrybutu Matter ────────────────────────────────────────────
static esp_err_t update_matter_fan_mode(FanMode mode)
{
    node_t *node = node::get();
    if (!node) return ESP_FAIL;
    endpoint_t *ep = endpoint::get(node, g_fan_endpoint_id);
    if (!ep) return ESP_FAIL;
    cluster_t *cl = cluster::get(ep, FanControl::Id);
    if (!cl) return ESP_FAIL;
    attribute_t *attr = attribute::get(cl, FanControl::Attributes::FanMode::Id);
    if (!attr) return ESP_FAIL;
    esp_matter_attr_val_t val = esp_matter_uint8((uint8_t)mode);
    return attribute::set_val(attr, &val);
}

// ─── Task: polling fizycznego stanu UPPÅTVIND ────────────────────────────────
static void physical_state_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(3000)); // poczekaj na start Matter

    FanMode last_physical = FanMode::kOff;
    while (true) {
        uint32_t pulse_us = read_pwm_pulse_us();
        FanMode physical = pwm_to_mode(pulse_us);

        if (physical != last_physical) {
            ESP_LOGI(TAG, "Fizyczna zmiana: %d→%d (pulse=%luµs)",
                     (int)last_physical, (int)physical, (unsigned long)pulse_us);
            last_physical = physical;
            g_current_mode = physical;

            // Aktualizuj Matter z właściwego kontekstu
            chip::DeviceLayer::PlatformMgr().ScheduleWork(
                [](intptr_t arg) {
                    update_matter_fan_mode((FanMode)arg);
                },
                (intptr_t)physical
            );
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ─── Callback atrybutu Matter (sterowanie z aplikacji) ───────────────────────
static esp_err_t app_attribute_update_cb(
    attribute::callback_type_t type,
    uint16_t endpoint_id,
    uint32_t cluster_id,
    uint32_t attribute_id,
    esp_matter_attr_val_t *val,
    void *priv_data)
{
    if (type != PRE_UPDATE) return ESP_OK;
    if (cluster_id != FanControl::Id) return ESP_OK;
    if (endpoint_id != g_fan_endpoint_id) return ESP_OK;

    if (attribute_id == FanControl::Attributes::FanMode::Id) {
        FanMode requested = (FanMode)val->val.u8;
        ESP_LOGI(TAG, "Matter→FanMode: %d→%d", (int)g_current_mode, (int)requested);

        static FanMode s_target;
        s_target = requested;
        xTaskCreate(
            [](void *arg) {
                FanMode tgt = *(FanMode *)arg;
                cycle_to_mode(g_current_mode, tgt);
                g_current_mode = tgt;
                vTaskDelete(nullptr);
            },
            "btn_cycle", 2048, &s_target, 5, nullptr
        );
    }
    return ESP_OK;
}

// ─── Callback identyfikacji ──────────────────────────────────────────────────
static esp_err_t app_identification_cb(
    identification::callback_type_t type,
    uint16_t endpoint_id,
    uint8_t effect_id,
    uint8_t effect_variant,
    void *priv_data)
{
    ESP_LOGI(TAG, "Identify: endpoint=%d effect=%d", endpoint_id, effect_id);
    return ESP_OK;
}

// ─── Inicjalizacja GPIO ──────────────────────────────────────────────────────
static void gpio_init(void)
{
    // GPIO4: open-drain output (symulacja przycisku)
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << GPIO_BTN_OUT),
        .mode         = GPIO_MODE_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn_cfg));
    gpio_set_level(GPIO_BTN_OUT, 1); // nieaktywny

    // GPIO5: input z pull-down (odczyt PWM)
    gpio_config_t pwm_cfg = {
        .pin_bit_mask = (1ULL << GPIO_PWM_IN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pwm_cfg));
}

// ─── app_main ────────────────────────────────────────────────────────────────
extern "C" void app_main(void)
{
    // NVS – wymagane przez Matter
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    gpio_init();

    // ── Matter node ──────────────────────────────────────────────────────────
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    ABORT_APP_ON_FAILURE(node != nullptr,
        ESP_LOGE(TAG, "Nie udało się stworzyć węzła Matter"));

    // ── Endpoint: Air Purifier (device type 0x002D) ──────────────────────────
    // endpoint::air_purifier::create() zawiera FanControl cluster (0x0202)
    // FanModeSequence=3 → Off/Low/Med/High (bez Auto)
    endpoint::air_purifier::config_t ap_config;
    ap_config.fan_control.fan_mode_sequence = 3; // Off/Low/Med/High
    ap_config.fan_control.fan_mode          = 0; // domyślnie Off

    endpoint_t *ap_ep = endpoint::air_purifier::create(
        node, &ap_config, ENDPOINT_FLAG_NONE, nullptr);
    ABORT_APP_ON_FAILURE(ap_ep != nullptr,
        ESP_LOGE(TAG, "Nie udało się stworzyć endpointu air_purifier"));

    g_fan_endpoint_id = endpoint::get_id(ap_ep);
    ESP_LOGI(TAG, "Air Purifier endpoint ID: %d", g_fan_endpoint_id);

    // ── Start Matter ─────────────────────────────────────────────────────────
    err = esp_matter::start(app_attribute_update_cb);
    ABORT_APP_ON_FAILURE(err == ESP_OK,
        ESP_LOGE(TAG, "esp_matter::start() failed: %s", esp_err_to_name(err)));

    ESP_LOGI(TAG, "Matter started – UPPÅTVIND Thread node gotowy");
    ESP_LOGI(TAG, "Zeskanuj QR kod powyżej w aplikacji IKEA Home Smart");

    // ── Task: polling fizycznego stanu ───────────────────────────────────────
    xTaskCreate(physical_state_task, "phys_state", 4096, nullptr, 5, nullptr);
}
