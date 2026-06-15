# IKEA UPPÅTVIND → Matter over Thread
### Seeed XIAO ESP32-C6 + esp-matter SDK + ESP-IDF v5.5.4

---

## Schemat podłączenia

```
XIAO ESP32-C6          UPPÅTVIND PCB
─────────────          ─────────────
GPIO4 (OD) ──────────► TP4  (przycisk, active-low)
GPIO5 (IN) ◄────────── TP7  (LED PWM: ~0/20/100/320 µs)
GND        ────────── GND
5V (VBUS)  ◄── 1N5819 ── TP3 (5V z UPPÅTVIND)
```

**GPIO4** = open-drain output, pull-up wewnętrzny. Puls 100ms low = jedno kliknięcie.  
**GPIO5** = input z pull-down. Szerokość impulsu PWM → tryb: 0µs=Off, ~20=Low, ~100=Med, ~320=High.

---

## 1. Instalacja zależności systemowych (Debian/Raspberry Pi OS arm64)

```bash
sudo apt update
sudo apt install -y \
    git wget curl flex bison gperf \
    python3 python3-pip python3-venv python3-dev \
    cmake ninja-build ccache \
    libffi-dev libssl-dev libusb-1.0-0-dev \
    dfu-util pkg-config zlib1g-dev \
    libglib2.0-dev libdbus-1-dev \
    screen picocom
```

---

## 2. Instalacja ESP-IDF v5.5.4

```bash
mkdir -p ~/esp && cd ~/esp

git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
git checkout v5.5.4
git submodule update --init --recursive

# Instalacja toolchaina dla RISC-V (ESP32-C6)
./install.sh esp32c6

cd ~
```

> ⚠️ Na Raspberry Pi (arm64) instalacja może potrwać 20–30 minut (kompilacja toolchaina ze źródeł).

---

## 3. Instalacja esp-matter SDK (branch main)

```bash
cd ~/esp

# Shallow clone – znacznie szybszy
git clone --depth 1 https://github.com/espressif/esp-matter.git
cd esp-matter
git submodule update --init --depth 1

# Submoduły connectedhomeip dla esp32 (tylko niezbędne platformy)
cd ./connectedhomeip/connectedhomeip
./scripts/checkout_submodules.py --platform esp32 linux --shallow
cd ../..

# Instalacja narzędzi Matter (ZAP, chip-cert, chip-tool…)
./install.sh
cd ~
```

---

## 4. Konfiguracja środowiska (każda nowa sesja terminala)

```bash
source ~/esp/esp-idf/export.sh
source ~/esp/esp-matter/export.sh

export ESP_MATTER_PATH=~/esp/esp-matter
```

> 💡 Warto dodać powyższe do `~/.bashrc` lub osobnego skryptu `~/esp/activate.sh`.

---

## 5. Klonowanie / przygotowanie projektu

```bash
git clone https://github.com/TWOJ_USER/uppatvind-matter.git
# lub skopiuj katalog projektu
cd uppatvind-matter
```

---

## 6. Build

```bash
cd ~/uppatvind-matter

# Wyczyść poprzedni build (ważne przy zmianie sdkconfig)
idf.py fullclean

# Ustaw target + załaduj sdkconfig.defaults (Thread build)
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults" set-target esp32c6

# Kompilacja (na RPi ~30-60 minut przy pierwszym buildzie)
idf.py build
```

---

## 7. Flash przez USB

XIAO ESP32-C6 pojawia się jako `/dev/ttyACM0` lub `/dev/ttyUSB0`.

```bash
# Sprawdź port
ls /dev/tty{ACM,USB}*

# Opcjonalnie: wymaż flash przy pierwszym wgraniu
idf.py -p /dev/ttyACM0 erase_flash

# Flash + monitor
idf.py -p /dev/ttyACM0 flash monitor
```

Przy pierwszym uruchomieniu w logu zobaczysz:
```
I (XXXX) chip[DL]: BLE advertising started
I (XXXX) uppatvind: Fan endpoint ID: 1
I (XXXX) chip[SVR]: SetupQRCode: [MT:...]
I (XXXX) chip[SVR]: ManualPairingCode: [XXXXXXXXXX]
```

---

## 8. Pobranie Thread Dataset z IKEA Dirigera i wgranie do urządzenia

### A) Pobierz aktywny dataset z Dirigera

**Przez Home Assistant (jeśli masz):**
```bash
# W HA Developer Tools → Services → openthread_border_router.get_dataset
# Lub przez terminal HA:
ha openthread get-active-dataset
```

**Przez SSH do Dirigera (jeśli masz dostęp):**
```bash
ssh root@<dirigera-ip>
# Dirigera używa ot-ctl / otnctl
ot-ctl dataset active -x
# Wynik: hex string, np. 0e080000000000010000...
```

**Przez aplikację IKEA Home Smart:**  
Dirigera wyświetla Thread network name w ustawieniach → "Połącz z siecią Thread".  
Aktywny dataset hex można też odczytać przez API Dirigera (port 8443):
```bash
curl -k -H "Authorization: Bearer <token>" \
  https://<dirigera-ip>:8443/v1/openthread/dataset
```

### B) Wgraj dataset do urządzenia przez konsolę Matter

Po uruchomieniu urządzenia (monitor aktywny):

```
# W konsoli szeregowej (chip> prompt):
matter esp ot_cli dataset set active <HEX_DATASET>

# Przykład:
matter esp ot_cli dataset set active 0e080000000000010000000300001235060004001fffe00208dead00beef00cafe0708fddead00beef00010610112233445566778899aabbccddeeff030f4f70656e5468726561642d455350010212340410445f2b5ca6f2a93a55ce570a70eee1570c0402a0f7f8

# Zapisz i aktywuj:
matter esp ot_cli dataset commit active
matter esp ot_cli ifconfig up
matter esp ot_cli thread start
```

### C) Komisjonowanie przez aplikację IKEA Home Smart

1. Otwórz **IKEA Home Smart** → Dodaj urządzenie
2. Zeskanuj QR kod z loga (lub wpisz Manual Pairing Code)
3. Aplikacja skojarzy urządzenie z Dirigerą jako Thread border router
4. W aplikacji pojawi się wentylator z trybami: Off / Low / Medium / High

---

## 9. Weryfikacja Thread network

W konsoli urządzenia:
```
matter esp ot_cli state        # Router / Child / Leader
matter esp ot_cli ipaddr       # Adresy IPv6
matter esp ot_cli neighbor table  # Sąsiedzi Thread (Dirigera)
matter esp ot_cli ping <ipv6>  # Test łączności
```

---

## 10. Troubleshooting

| Problem | Rozwiązanie |
|---------|------------|
| `ABORT: Failed to create fan endpoint` | Sprawdź wersję esp-matter – `endpoint::fan` musi być dostępny |
| Urządzenie nie pojawia się w IKEA app | Sprawdź czy dataset Thread pasuje do Dirigery (`ot_cli dataset active`) |
| `OOM` / crash podczas BLE | Zwiększ `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL` lub ogranicz inne taski |
| GPIO5 zawsze zwraca 0 | Sprawdź progowe wartości PWM – zmierz oscyloskopem TP7 |
| Przycisk nie reaguje | Sprawdź open-drain + pull-up na GPIO4; mierz napięcie na TP4 |
| Build error: `fan::create` not found | Upewnij się że `esp-matter` jest na branch `main`, nie `release` |

---

## Uwagi projektowe

- **Brak air_purifier example** w esp-matter – używamy `endpoint::fan::create()` który zawiera identyczny FanControl cluster (0x0202). Device type ID można zmienić ręcznie jeśli IKEA wymaga `0x002D` (Air Purifier) zamiast `0x002B` (Fan).
- **FanModeSequence = 3** → Off/Low/Med/High (bez Auto), co odpowiada fizycznym trybom UPPÅTVIND.
- **Komisjonowanie BLE** jest wymagane przy pierwszym parze; po komisjonowaniu BLE jest wyłączane (`USE_BLE_ONLY_FOR_COMMISSIONING=y`) oszczędzając RAM i moc.
- **OTA** jest przygotowane przez tablicę partycji z `ota_0`/`ota_1` – aktualizacje przez `idf.py ota` lub Matter OTA cluster.
