# IKEA UPPÅTVIND → Matter over Thread
### Seeed XIAO ESP32-C6 + esp-matter SDK + ESP-IDF v5.5

Zamienia oczyszczacz powietrza IKEA UPPÅTVIND w natywne urządzenie Matter over Thread,
sterowane bezpośrednio z aplikacji **IKEA Home Smart** przez **IKEA Dirigera** jako Thread border router.
Bez Home Assistant, bez chmury, bez Wi-Fi.

---

## Schemat podłączenia

```
XIAO ESP32-C6          UPPÅTVIND PCB
─────────────          ─────────────
GPIO4 (OD) ──────────► TP4  (przycisk, active-low)
GPIO5 (IN) ◄────────── TP7  (LED PWM sygnał stanu)
GND        ────────── GND
5V (VBUS)  ◄── 1N5819 ── TP3 (5V z UPPÅTVIND)
```

**GPIO4** = open-drain output + pull-up. Puls 100ms low = jedno kliknięcie przycisku.
**GPIO5** = input z pull-down. Szerokość impulsu PWM → tryb pracy:

| Szerokość impulsu | Tryb     |
|-------------------|----------|
| < 10 µs           | Off      |
| 10–60 µs          | Low      |
| 60–210 µs         | Medium   |
| > 210 µs          | High     |

Referencja sprzętowa: https://github.com/jonathonlui/esphome-ikea-uppatvind

---

## Architektura oprogramowania

- **Device type**: Air Purifier (Matter 0x002D) – `endpoint::air_purifier::create()`
- **Cluster**: FanControl (0x0202), FanModeSequence=3 (Off/Low/Med/High)
- **Transport**: Thread only (IEEE 802.15.4, natywne radio ESP32-C6)
- **Komisjonowanie**: BLE → QR kod → IKEA Home Smart
- **Synchronizacja stanu**: polling GPIO5 co 200ms; zmiany fizyczne (przyciski na obudowie) automatycznie aktualizują stan Matter

---

## Instalacja krok po kroku

### 1. Narzędzia systemowe (Debian/Raspberry Pi OS arm64)

```bash
sudo apt update && sudo apt install -y \
    git wget curl flex bison gperf \
    python3 python3-pip python3-venv python3-dev \
    cmake ninja-build ccache \
    libffi-dev libssl-dev libusb-1.0-0-dev \
    dfu-util pkg-config zlib1g-dev \
    libglib2.0-dev libdbus-1-dev
```

### 2. ESP-IDF v5.5

```bash
mkdir -p ~/esp && cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
git checkout v5.5
git submodule update --init --recursive
./install.sh esp32c6
cd ~
```

> ⚠️ Na Raspberry Pi zajmuje ~20–30 minut.

### 3. esp-matter SDK (branch main)

```bash
cd ~/esp
git clone --depth 1 https://github.com/espressif/esp-matter.git
cd esp-matter
git submodule update --init --depth 1
cd ./connectedhomeip/connectedhomeip
./scripts/checkout_submodules.py --platform esp32 linux --shallow
cd ../..
./install.sh
cd ~
```

### 4. Aktywacja środowiska (każda nowa sesja)

```bash
source ~/esp/esp-idf/export.sh
source ~/esp/esp-matter/export.sh
export ESP_MATTER_PATH=~/esp/esp-matter
```

### 5. Klonowanie projektu

```bash
git clone https://github.com/JamesTechnic/uppatvind-matter-esp32c6-mode.git
cd uppatvind-matter-esp32c6-mode
```

### 6. Build

```bash
idf.py fullclean
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults" set-target esp32c6
idf.py build
```

> Pierwszy build na RPi: ~30–60 minut.

### 7. Flash

```bash
# Sprawdź port (zwykle /dev/ttyACM0)
ls /dev/tty{ACM,USB}*

# Pierwszy raz: wymaż flash
idf.py -p /dev/ttyACM0 erase_flash

# Wgraj i otwórz monitor
idf.py -p /dev/ttyACM0 flash monitor
```

---

## Komisjonowanie (parowanie z IKEA Home Smart)

Po uruchomieniu urządzenia w logu pojawi się:

```
I (XXXX) chip[SVR]: SetupQRCode: [MT:XXXXXXXXXX]
I (XXXX) chip[SVR]: ManualPairingCode: [XXXXXXXXXX]
```

A nad tym wydrukowany w ASCII art **QR kod**.

1. Otwórz aplikację **IKEA Home Smart**
2. Dodaj nowe urządzenie → **Skanuj QR kod**
3. Zeskanuj QR kod z ekranu monitora (lub wpisz ManualPairingCode)
4. Aplikacja automatycznie skonfiguruje Thread przez Dirigerę i sparuje urządzenie
5. W aplikacji pojawi się oczyszczacz powietrza z trybami: **Off / Low / Medium / High**

> ℹ️ Nie musisz ręcznie wpisywać żadnych danych Thread. Aplikacja IKEA Home Smart
> pobiera Thread dataset z Dirigery i przekazuje go do urządzenia automatycznie
> podczas procesu komisjonowania (BLE → Thread handoff).

---

## Weryfikacja połączenia Thread (opcjonalnie)

Po komisjonowaniu możesz sprawdzić stan sieci Thread przez monitor szeregowy:

```
matter esp ot_cli state          # Router / Child / Leader
matter esp ot_cli ipaddr         # Adresy IPv6 urządzenia
matter esp ot_cli neighbor table # Sąsiedzi w sieci Thread (Dirigera)
```

---

## FAQ

**Czy muszę podpiąć czerwoną diodę filtra?**
Nie. Czerwona dioda sygnalizuje zużycie filtra – to opcjonalny cluster
`HepaFilterMonitoring` (planowany jako v2). Urządzenie działa w pełni bez tego.

**Fizyczne przyciski na UPPÅTVIND przestaną działać?**
Nie – firmware polluje stan GPIO5 co 200ms i synchronizuje zmiany fizyczne
z Matter, więc przyciski na obudowie działają normalnie i aktualizują stan
w aplikacji IKEA.

**Dlaczego Thread a nie Wi-Fi?**
Dirigera jest Thread border routerem. Komunikacja Thread jest lokalna,
szyfrowana, nie wymaga chmury i działa bez internetu.

---

## Roadmap (v2)

- [ ] `HepaFilterMonitoring` cluster – odczyt czerwonej diody filtra (GPIO dodatkowy)
- [ ] OTA przez Matter OTA cluster
- [ ] Kalibracja progów PWM przez NVS (bez rekompilacji)
