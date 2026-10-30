/*
 * IKEA UPPÅTVIND → Matter over Thread
 * Hardware: Seeed XIAO ESP32-C6
 *
 * GPIO4 → TP4 : symulacja przycisku (open-drain, active-low, puls 100ms)
 * GPIO5 ← TP7 : odczyt LED PWM – szerokość impulsu HIGH określa tryb
 *
 * Device type : Air Purifier (Matter 0x002D)
 * Cluster     : FanControl (0x0202), FanModeSequence=0 → Off/Low/Med/High
 *
 * WAŻNE: Wartości progów PWM (THRESH_*) są wstępne i MUSZĄ być
 * skalibrowane po zmierzeniu rzeczywistych impulsów na TP7.
 * Uruchom z #define DEBUG_PWM 1 i obserwuj logi przed pierwszym użyciem.
 */

#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>          // esp_timer_get_time() – wymagany jawny include
#include <esp_rom_sys.h>        // esp_rom_delay_us()
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_matter.h>         // dociąga endpoint/cluster/attribute
#include <esp_matter_console.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app-common/zap-generated/ids/Attributes.h>

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <platform/ESP32/OpenthreadLauncher.h>
#endif

static const char *TAG = "uppatvind";

// Makro z examples/common/utils/common_macros.h (esp-matter) – tutaj własna
// kopia, żeby projekt nie zależał od katalogu examples.
#ifndef ABORT_APP_ON_FAILURE
#define ABORT_APP_ON_FAILURE(x, ...) do {          \
        if (!(x)) {                                \
            __VA_ARGS__;                           \
            vTaskDelay(pdMS_TO_TICKS(5000));       \
            abort();                               \
        }                                          \
    } while (0)
#endif

// ─── Kalibracja PWM ──────────────────────────────────────────────────────────
// Ustaw na 1 podczas pierwszego uruchomienia – wypisuje surowe µs do logu.
// Po kalibracji ustaw na 0 i dostosuj THRESH_* do zmierzonych wartości.
#define DEBUG_PWM 1

// Progi [µs] między poziomami – wartości WSTĘPNE z dokumentacji TP7.
// Referencja: github.com/jonathonlui/esphome-ikea-uppatvind
// Jeśli jednostki są ms (nie µs), pomnóż przez 1000.
#define THRESH_LOW_US    10
#define THRESH_MED_US    60
#define THRESH_HIGH_US   210
#define PULSE_TIMEOUT_US 2000   // max czas oczekiwania na koniec impulsu HIGH

// ─── GPIO ────────────────────────────────────────────────────────────────────
#define GPIO_BTN_OUT  GPIO_NUM_4
#define GPIO_PWM_IN   GPIO_NUM_5
#define BTN_PULSE_MS  100

// ─── Matter ──────────────────────────────────────────────────────────────────
using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace chip::app::Clusters;

static uint16_t g_fan_endpoint_id = 0;

enum class FanMode : uint8_t {
    kOff    = 0,
    kLow    = 1,
    kMedium = 2,
    kHigh   = 3,
};

// volatile wystarczy dla uint8_t na ESP32 (zapis/odczyt atomowy sprzętowo)
static volatile FanMode g_current_mode = FanMode::kOff;
static volatile bool    s_is_cycling   = false;

// ─── Odczyt PWM z TP7 ────────────────────────────────────────────────────────
static uint32_t read_pwm_pulse_us(void)
{
    // Czekaj na zbocze narastające (max 5ms)
    const int64_t t_start = esp_timer_get_time();
    while (gpio_get_level(GPIO_PWM_IN) == 0) {
        if ((esp_timer_get_time() - t_start) > 5000) return 0; // sygnał ciągle LOW → Off
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
    ESP_LOGI(TAG, "Klikam %d razy: %d→%d", clicks, (int)current, (int)target);
    for (int i = 0; i < clicks; i++) {
        press_button();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ─── Aktualizacja atrybutu Matter (zawsze z Matter task przez ScheduleWork) ──
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
    // attribute::update() (a nie set_val) zapisuje wartość i RAPORTUJE ją do
    // kontrolera – dzięki temu zmiana z fizycznego przycisku trafia do aplikacji.
    // Wywołanie jest bezpieczne: jesteśmy w callbacku ScheduleWork (wątek Matter).
    attribute::update(g_fan_endpoint_id, FanControl::Id,
                      FanControl::Attributes::FanMode::Id, &val);
}

// ─── Task: polling fizycznego stanu UPPÅTVIND ────────────────────────────────
// Priorytet 1 – nie głodzi stosu Thread/Matter
static void physical_state_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(3000));

    FanMode last_physical = FanMode::kOff;

    while (true) {
        if (!s_is_cycling) {
            uint32_t pulse_us = read_pwm_pulse_us();
            FanMode physical = pwm_to_mode(pulse_us);

#if DEBUG_PWM
            // Wypisuj surowe wartości µs – użyj do kalibracji progów!
            // Zmień DEBUG_PWM na 0 po potwierdzeniu poprawnych wartości.
            ESP_LOGI(TAG, "[KALIBRACJA] pulse=%lu µs → tryb=%d",
                     (unsigned long)pulse_us, (int)physical);
#endif

            if (physical != last_physical) {
                ESP_LOGI(TAG, "Fizyczna zmiana: %d→%d", (int)last_physical, (int)physical);
                last_physical = physical;
                g_current_mode = physical;

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
        // Sprzęt ma tylko Off/Low/Medium/High (FanModeSequence 0).
        if (val->val.u8 > (uint8_t)FanMode::kHigh) {
            ESP_LOGW(TAG, "Nieobsługiwany tryb wentylatora: %d", (int)val->val.u8);
            return ESP_ERR_INVALID_ARG;
        }
        FanMode requested = (FanMode)val->val.u8;

        // Łamanie pętli sprzężenia zwrotnego:
        // zmiana z fizycznego przycisku → physical_state_task → ScheduleWork
        // → set_matter_fan_mode_cb → attribute::set_val → ten callback
        // → wychodzi tutaj, NIE klika przycisku ponownie
        if (requested == g_current_mode) {
            return ESP_OK;
        }

        // Odrzuć żądanie jeśli trwa sekwencja klikania
        if (s_is_cycling) {
            ESP_LOGW(TAG, "Zajęty – ignoruję żądanie trybu %d", (int)requested);
            return ESP_ERR_BUSY;
        }

        ESP_LOGI(TAG, "Matter→FanMode: %d→%d", (int)g_current_mode, (int)requested);

        // POPRAWKA: s_is_cycling = true PRZED xTaskCreate,
        // nie wewnątrz taska – eliminuje okno czasowe gdy dwa żądania
        // mogłyby jednocześnie przejść przez sprawdzenie flagi
        s_is_cycling = true;

        xTaskCreate(
            [](void *arg) {
                FanMode tgt = (FanMode)(intptr_t)arg; // wartość, nie wskaźnik
                cycle_to_mode(g_current_mode, tgt);
                g_current_mode = tgt;
                s_is_cycling = false;
                vTaskDelete(nullptr);
            },
            "btn_cycle",
            3072,
            (void *)(intptr_t)requested,
            5,
            nullptr
        );
    }
    return ESP_OK;
}

// ─── Callback zdarzeń Matter/CHIP (podawany do esp_matter::start) ─────────────
static void app_event_cb(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Komisjonowanie zakończone");
        break;
    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGW(TAG, "Komisjonowanie nieudane – fail-safe timer wygasł");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "Sesja komisjonowania rozpoczęta");
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
        ESP_LOGI(TAG, "Sesja komisjonowania zakończona");
        break;
    case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
        ESP_LOGI(TAG, "Fabric usunięty");
        break;
    case chip::DeviceLayer::DeviceEventType::kThreadStateChange:
        ESP_LOGI(TAG, "Zmiana stanu sieci Thread");
        break;
    default:
        break;
    }
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
    // GPIO4: open-drain + pull-up (symulacja przycisku, active-low)
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << GPIO_BTN_OUT),
        .mode         = GPIO_MODE_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn_cfg));
    gpio_set_level(GPIO_BTN_OUT, 1);

    // GPIO5: input floating (bez pull-up/down).
    // TP7 ma własny driver – wewnętrzny pull-down ESP32 może zniekształcić sygnał.
    // Jeśli pin "pływa" przy odłączonym TP7, dodaj zewnętrzny rezystor 10kΩ do GND.
    gpio_config_t pwm_cfg = {
        .pin_bit_mask = (1ULL << GPIO_PWM_IN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE, // floating – TP7 sam steruje poziomem
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pwm_cfg));
}

// ─── app_main ────────────────────────────────────────────────────────────────
extern "C" void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    gpio_init();

    node::config_t node_config;
    node_t *node = node::create(
        &node_config, app_attribute_update_cb, app_identification_cb);
    ABORT_APP_ON_FAILURE(node != nullptr,
        ESP_LOGE(TAG, "Nie udało się stworzyć węzła Matter"));

    // Air Purifier (0x002D) – natywnie obsługiwany przez IKEA Dirigera
    endpoint::air_purifier::config_t ap_config;
    // FanModeSequenceEnum: 0 = Off/Low/Med/High (3 = Off/Low/High/Auto – to NIE to samo!)
    ap_config.fan_control.fan_mode_sequence = 0;
    ap_config.fan_control.fan_mode          = 0; // domyślnie Off

    endpoint_t *ap_ep = endpoint::air_purifier::create(
        node, &ap_config, ENDPOINT_FLAG_NONE, nullptr);
    ABORT_APP_ON_FAILURE(ap_ep != nullptr,
        ESP_LOGE(TAG, "Nie udało się stworzyć endpointu air_purifier"));

    g_fan_endpoint_id = endpoint::get_id(ap_ep);
    ESP_LOGI(TAG, "Air Purifier endpoint ID: %d", g_fan_endpoint_id);

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    // WYMAGANE: esp_matter::start() NIE konfiguruje radia/OpenThread samo.
    // Bez tego stos Thread nie wystartuje i komisjonowanie nigdy nie dojdzie
    // do sieci Thread. Musi być wywołane PRZED esp_matter::start().
    esp_openthread_platform_config_t ot_config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config  = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config  = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };
    set_openthread_platform_config(&ot_config);
#endif

    // Uwaga: start() przyjmuje callback ZDARZEŃ (app_event_cb), a nie callback
    // atrybutów – ten ostatni został już podany w node::create().
    err = esp_matter::start(app_event_cb);
    ABORT_APP_ON_FAILURE(err == ESP_OK,
        ESP_LOGE(TAG, "esp_matter::start() failed: %s", esp_err_to_name(err)));

    ESP_LOGI(TAG, "Matter started – zeskanuj QR kod w aplikacji IKEA Home Smart");

    // Priorytet 1 – nie głodzi Thread/Matter (busy-wait w read_pwm_pulse_us)
    xTaskCreate(physical_state_task, "phys_state", 4096, nullptr, 1, nullptr);

#if CONFIG_ENABLE_CHIP_SHELL
    // Konsola szeregowa: polecenia "matter esp ot_cli ..." z README działają
    // dopiero po zarejestrowaniu i uruchomieniu konsoli.
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::factoryreset_register_commands();
    esp_matter::console::attribute_register_commands();
#if CONFIG_OPENTHREAD_CLI
    esp_matter::console::otcli_register_commands();
#endif
    esp_matter::console::init();
#endif
}
