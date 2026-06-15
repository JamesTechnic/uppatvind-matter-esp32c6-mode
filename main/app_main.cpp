/*
 * IKEA UPPÅTVIND → Matter over Thread
 * Hardware: Seeed XIAO ESP32-C6
 *
 * GPIO4 → TP4 : symulacja przycisku (open-drain, active-low, puls 100ms)
 * GPIO5 ← TP7 : odczyt LED PWM (pulseIn µs): ~0=off, ~20=low, ~100=med, ~320=high
 *
 * Device type : Air Purifier (Matter 0x002D)
 * Cluster     : FanControl (0x0202), FanModeSequence=3 → Off/Low/Med/High
 *
 * Changelog v2:
 *   - Naprawiona pętla sprzężenia zwrotnego (feedback loop)
 *   - Naprawiony race condition przy przekazywaniu FanMode do zadania FreeRTOS
 *   - Dodana flaga s_is_cycling blokująca nakładanie się zadań btn_cycle
 *   - Zmniejszony priorytet physical_state_task z 5 na 1
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
#define GPIO_BTN_OUT     GPIO_NUM_4
#define GPIO_PWM_IN      GPIO_NUM_5
#define BTN_PULSE_MS     100
#define PULSE_TIMEOUT_US 2000

// Progi pulseIn [µs]:  off<10, low 10–60, med 60–210, high>210
#define THRESH_LOW_US   10
#define THRESH_MED_US   60
#define THRESH_HIGH_US  210

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

// Stan globalny – czytany/pisany tylko z kontekstu Matter task lub btn_cycle
static volatile FanMode g_current_mode = FanMode::kOff;

// Flaga: trwa sekwencja klikania przycisku → ignoruj nowe żądania Matter
// (volatile bo dostępna z różnych zadań FreeRTOS)
static volatile bool s_is_cycling = false;

// ─── Odczyt PWM ──────────────────────────────────────────────────────────────
static uint32_t read_pwm_pulse_us(void)
{
    // Czekaj na zbocze narastające (max 5ms)
    const int64_t t_start = esp_timer_get_time();
    while (gpio_get_level(GPIO_PWM_IN) == 0) {
        if ((esp_timer_get_time() - t_start) > 5000) return 0; // timeout → Off
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

// Kliknij N razy żeby przejść current→target (cykl: Off→Low→Med→High→Off)
static void cycle_to_mode(FanMode current, FanMode target)
{
    if (current == target) return;
    int clicks = ((int)target - (int)current + 4) % 4;
    ESP_LOGI(TAG, "Klikam %d razy: %d→%d", clicks, (int)current, (int)target);
    for (int i = 0; i < clicks; i++) {
        press_button();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ─── Aktualizacja atrybutu Matter ────────────────────────────────────────────
// Wywołać przez ScheduleWork – zawsze z kontekstu Matter task
static void set_matter_fan_mode_cb(intptr_t arg)
{
    FanMode mode = (FanMode)arg;
    node_t *node = node::get();
    if (!node) return;
    endpoint_t *ep = endpoint::get(node, g_fan_endpoint_id);
    if (!ep) return;
    cluster_t *cl = cluster::get(ep, FanControl::Id);
    if (!cl) return;
    attribute_t *attr = attribute::get(cl, FanControl::Attributes::FanMode::Id);
    if (!attr) return;
    esp_matter_attr_val_t val = esp_matter_uint8((uint8_t)mode);
    attribute::set_val(attr, &val);
}

// ─── Task: polling fizycznego stanu UPPÅTVIND ────────────────────────────────
// Priorytet 1 (najniższy) – nie może głodzić stosu Thread/Matter
static void physical_state_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(3000)); // poczekaj na pełny start Matter

    FanMode last_physical = FanMode::kOff;

    while (true) {
        // Nie sprawdzaj stanu podczas sekwencji klikania –
        // GPIO5 zmieniałoby się w wyniku naszego własnego działania
        if (!s_is_cycling) {
            uint32_t pulse_us = read_pwm_pulse_us();
            FanMode physical = pwm_to_mode(pulse_us);

            if (physical != last_physical) {
                ESP_LOGI(TAG, "Fizyczna zmiana: %d→%d (pulse=%luµs)",
                         (int)last_physical, (int)physical,
                         (unsigned long)pulse_us);
                last_physical = physical;
                g_current_mode = physical;

                // Poinformuj Matter z właściwego kontekstu (nie przerwanie ISR)
                chip::DeviceLayer::PlatformMgr().ScheduleWork(
                    set_matter_fan_mode_cb, (intptr_t)physical);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ─── Callback atrybutu Matter ─────────────────────────────────────────────────
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

        // POPRAWKA 1: Jeśli stan jest już taki sam – nic nie rób.
        // To łamie pętlę sprzężenia zwrotnego:
        //   fizyczny przycisk → physical_state_task → set_matter_fan_mode_cb
        //   → attribute::set_val → ten callback → wyjście tutaj (nie klika)
        if (requested == g_current_mode) {
            return ESP_OK;
        }

        // POPRAWKA 3: Jeśli trwa sekwencja klikania – odrzuć nowe żądanie.
        // Alternatywą byłaby kolejka, ale dla oczyszczacza odrzucenie jest OK –
        // użytkownik po chwili zobaczy aktualny stan i kliknie ponownie.
        if (s_is_cycling) {
            ESP_LOGW(TAG, "Zajęty – ignoruję żądanie trybu %d", (int)requested);
            return ESP_ERR_BUSY;
        }

        ESP_LOGI(TAG, "Matter→FanMode: %d→%d", (int)g_current_mode, (int)requested);

        // POPRAWKA 2: Przekazuj FanMode przez wartość (rzutowanie na intptr_t),
        // nie przez wskaźnik do zmiennej statycznej – eliminuje race condition
        // gdy dwa żądania przyjdą szybko jedno po drugim.
        xTaskCreate(
            [](void *arg) {
                s_is_cycling = true;
                FanMode tgt = (FanMode)(intptr_t)arg;

                cycle_to_mode(g_current_mode, tgt);
                g_current_mode = tgt;

                s_is_cycling = false;
                vTaskDelete(nullptr);
            },
            "btn_cycle",
            2048,
            (void *)(intptr_t)requested, // wartość, nie wskaźnik
            5,
            nullptr
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
    // GPIO4: open-drain output + pull-up (symulacja przycisku, active-low)
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << GPIO_BTN_OUT),
        .mode         = GPIO_MODE_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn_cfg));
    gpio_set_level(GPIO_BTN_OUT, 1); // nieaktywny (wysoki)

    // GPIO5: input z pull-down (odczyt PWM LED)
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
    // Uwaga: esp_matter::start() inicjalizuje OpenThread automatycznie
    // na podstawie sdkconfig (CONFIG_OPENTHREAD_ENABLED=y).
    // Nie ma potrzeby ręcznego esp_openthread_init() jak w czystym ESP-IDF.
    node::config_t node_config;
    node_t *node = node::create(
        &node_config, app_attribute_update_cb, app_identification_cb);
    ABORT_APP_ON_FAILURE(node != nullptr,
        ESP_LOGE(TAG, "Nie udało się stworzyć węzła Matter"));

    // ── Endpoint: Air Purifier (Matter device type 0x002D) ───────────────────
    // Zawiera FanControl cluster (0x0202)
    // FanModeSequence=3 → Off/Low/Med/High (bez Auto)
    endpoint::air_purifier::config_t ap_config;
    ap_config.fan_control.fan_mode_sequence = 3;
    ap_config.fan_control.fan_mode          = 0; // domyślnie Off

    endpoint_t *ap_ep = endpoint::air_purifier::create(
        node, &ap_config, ENDPOINT_FLAG_NONE, nullptr);
    ABORT_APP_ON_FAILURE(ap_ep != nullptr,
        ESP_LOGE(TAG, "Nie udało się stworzyć endpointu air_purifier"));

    g_fan_endpoint_id = endpoint::get_id(ap_ep);
    ESP_LOGI(TAG, "Air Purifier endpoint ID: %d", g_fan_endpoint_id);

    // ── Start Matter (inicjalizuje też OpenThread i BLE) ─────────────────────
    err = esp_matter::start(app_attribute_update_cb);
    ABORT_APP_ON_FAILURE(err == ESP_OK,
        ESP_LOGE(TAG, "esp_matter::start() failed: %s", esp_err_to_name(err)));

    ESP_LOGI(TAG, "Matter started – UPPÅTVIND Thread node gotowy");
    ESP_LOGI(TAG, "Zeskanuj QR kod powyżej w aplikacji IKEA Home Smart");

    // ── Task: polling fizycznego stanu (priorytet 1 – nie głodzi Thread/Matter)
    xTaskCreate(physical_state_task, "phys_state", 4096, nullptr, 1, nullptr);
}
