<div align="center">

# 🔥 Прошивка ESP32-C3

Два способа записи: через ESP-IDF или готовым образом с `esptool`.

[![Target](https://img.shields.io/badge/target-ESP32--C3-18a558?logo=espressif&logoColor=white)](https://www.espressif.com/en/products/socs/esp32-c3)
![Flash](https://img.shields.io/badge/flash-4%20MB-7b2cbf)
![DIO 80 MHz](https://img.shields.io/badge/flash%20mode-DIO%20%7C%2080%20MHz-f57c00)
![esptool](https://img.shields.io/badge/esptool-4.11.0-168aad)

</div>

---

## 🧭 Выберите способ

| Что есть на компьютере | Действие | Сохранятся ли настройки NVS? |
|---|---|---|
| ESP-IDF 5.3 | Собрать исходники и записать через `idf.py` | Обычно да |
| Python и esptool | Записать полный образ одним файлом | **Нет**, NVS сбрасывается |
| Python и esptool | Записать три отдельных файла | **Да**, NVS не затрагивается |

> [!CAUTION]
> Комплект в этом каталоге собран **14 октября 2026 года**. Он может не содержать
> более поздних изменений исходников. Чтобы получить прошивку именно из текущего
> кода, соберите проект через ESP-IDF.

## 📦 Параметры комплекта

Бинарники находятся в [`firmware/release_2026-10-14/`](firmware/release_2026-10-14/);
архив для скачивания — [`firmware/electronic_load_esp32c3_2026-10-14.zip`](firmware/electronic_load_esp32c3_2026-10-14.zip).

| Параметр | Значение |
|---|---|
| Чип | ESP32-C3 |
| Размер flash | 4 MB |
| Режим / частота flash | DIO / 80 MHz |
| ESP-IDF / esptool при сборке | 5.3 / 4.11.0 |
| Полный образ | `electronic_load_esp32c3_full.bin`, адрес `0x0` |
| Отдельные разделы | bootloader `0x0`, таблица `0x8000`, приложение `0x10000` |

**Это не образ для ESP32-S2 или оригинальной ESP32.**

В комплекте есть контрольные суммы: [`SHA256SUMS.txt`](firmware/release_2026-10-14/SHA256SUMS.txt).
Проверить их можно после перехода в каталог комплекта:

```bash
sha256sum -c SHA256SUMS.txt
```

В Windows PowerShell сравните SHA256 для нужного файла:

```powershell
Get-FileHash .\electronic_load_esp32c3_full.bin -Algorithm SHA256
```

## 🔌 Подготовьте плату

1. Отключите аккумулятор и силовую нагрузку.
2. Подключите плату кабелем USB с линиями данных.
3. Найдите последовательный порт: например, `/dev/ttyACM0` или `/dev/ttyUSB0`
   в Linux, `COM5` в Windows.
4. Если плата не перешла в режим загрузки автоматически, удерживайте **BOOT**,
   нажмите и отпустите **RESET**, затем отпустите **BOOT**.

## 🧰 С ESP-IDF 5.3

Этот способ собирает текущие исходники проекта. Из корня проекта выполните:

```bash
idf.py -B build -DIDF_TARGET=esp32c3 -p PORT flash
```

Замените `PORT` на последовательный порт платы. Например:

```bash
idf.py -B build -DIDF_TARGET=esp32c3 -p /dev/ttyACM0 flash
```

В Windows используйте порт вида `COM5`. ESP-IDF при необходимости пересоберёт
проект и загрузит образ вместе с таблицей разделов.

## 🐍 Без ESP-IDF: только esptool

Для записи готовых бинарников нужны Python 3 и **esptool 4.11.0**. Сначала
распакуйте комплект и перейдите в каталог `release_2026-10-14`.

### Установите esptool

**Linux:**

```bash
python3 -m venv .venv
. .venv/bin/activate
python -m pip install esptool==4.11.0
```

**Windows PowerShell:**

```powershell
py -m venv .venv
.venv\Scripts\python.exe -m pip install esptool==4.11.0
```

### Вариант A: полный образ одним файлом

> [!WARNING]
> Полный образ содержит `0xFF` в области между таблицей разделов и приложением,
> включая раздел NVS по адресу `0x9000`. Запись образа от `0x0` стирает этот
> участок flash, поэтому сохранённые настройки шунта, PWM, PGA и PI будут
> сброшены к значениям по умолчанию.

**Linux:**

```bash
.venv/bin/python -m esptool --chip esp32c3 --port /dev/ttyACM0 --baud 460800 write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB 0x0 electronic_load_esp32c3_full.bin
```

Можно запустить комплектный скрипт:

```bash
./flash_linux.sh /dev/ttyACM0
```

**Windows PowerShell:**

```powershell
.venv\Scripts\python.exe -m esptool --chip esp32c3 --port COM5 --baud 460800 write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB 0x0 electronic_load_esp32c3_full.bin
```

Или:

```powershell
.\flash_windows.bat COM5
```

Замените порт на свой. Если на скорости 460800 возникают ошибки передачи,
повторите запись с `--baud 115200`.

### Вариант B: три файла с сохранением NVS

Этот вариант записывает только bootloader, таблицу разделов и приложение.
Область NVS между таблицей и приложением не затрагивается, поэтому сохранённые
настройки остаются во flash.

**Linux:**

```bash
python -m esptool --chip esp32c3 --port /dev/ttyACM0 --baud 460800 write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 electronic_load.bin
```

**Windows PowerShell:**

```powershell
py -m esptool --chip esp32c3 --port COM5 --baud 460800 write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 electronic_load.bin
```

При необходимости замените `python` на `python3` или `py` в зависимости от
способа запуска Python на компьютере.

## ✅ После записи

Дождитесь завершения записи и перезапуска платы либо нажмите **RESET**.
Нагрузка должна стартовать выключенной. Сначала проверьте дисплей, кнопки,
INA219 и точку доступа без подключённой силовой нагрузки. Первый запуск
выполняйте от источника с ограничением тока и при низком напряжении.

## 🧯 Если плата не прошивается

| Симптом | Что проверить |
|---|---|
| esptool не находит чип | Порт, USB-кабель с данными, режим BOOT |
| Linux: `Permission denied` | Права на `/dev/ttyACM*` или `/dev/ttyUSB*` |
| Ошибки передачи | Снизить скорость до `--baud 115200`, заменить USB-кабель |
| Не запускается после записи | Проверить, что выбран ESP32-C3 и корректный образ |
| Настройки сбросились | Вероятно, был записан полный образ, который очищает NVS |

---

Краткое описание проекта — в [`README.md`](README.md); полная документация —
в [`PROJECT_CONTEXT_AND_USER_MANUAL.txt`](PROJECT_CONTEXT_AND_USER_MANUAL.txt).
