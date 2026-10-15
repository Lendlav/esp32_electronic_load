# Прошивка ESP32-C3 на другом компьютере

## Готовый комплект

Каталог `firmware/release_2026-10-14/` содержит собранную прошивку:

- `electronic_load_esp32c3_full.bin` — полный объединённый образ: bootloader,
  таблица разделов и приложение. Записывается одним файлом с адреса `0x0`.
- `bootloader.bin`, `partition-table.bin`, `electronic_load.bin` — те же части
  отдельно, если инструмент прошивки требует несколько файлов и адресов.
- `SHA256SUMS.txt` — контрольные суммы файлов.

Образ собран для **ESP32-C3 с flash 4 MB**, режимом DIO и частотой flash 80 MHz.
Это не образ для ESP32-S2 или оригинальной ESP32.

## Подготовка платы

1. Отключите аккумулятор и силовую нагрузку на время прошивки.
2. Подключите ESP32-C3 к компьютеру USB-кабелем с линиями данных.
3. Узнайте имя последовательного порта: например, `/dev/ttyACM0` или
   `/dev/ttyUSB0` в Linux, `COM5` в Windows.
4. Если плата не входит в режим загрузки автоматически, удерживайте **BOOT**,
   нажмите и отпустите **RESET**, затем отпустите **BOOT**.

## Вариант 1: на компьютере есть ESP-IDF и `idf.py`

Если на компьютере есть исходный проект и настроено окружение ESP-IDF 5.3,
можно пересобрать проект и прошить плату из корня проекта:

```bash
idf.py -B build -DIDF_TARGET=esp32c3 -p PORT flash
```

Замените `PORT` на порт платы. Эта команда использует исходники и конфигурацию
проекта на компьютере и при необходимости пересобирает их.

Чтобы записать именно подготовленный бинарный образ, используйте `esptool` из
окружения ESP-IDF, находясь в каталоге `firmware/release_2026-10-14`:

```bash
python -m esptool --chip esp32c3 --port PORT --baud 460800 write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB 0x0 electronic_load_esp32c3_full.bin
```

## Вариант 2: без ESP-IDF и `idf.py`

ESP-IDF для записи готового бинарника не нужен. Нужны Python 3 и пакет esptool.
Сначала распакуйте архив и перейдите в каталог `release_2026-10-14`, где лежит
полный образ. Установите версию esptool, использованную для подготовки
комплекта:

Linux:

```bash
python3 -m venv .venv
. .venv/bin/activate
python -m pip install esptool==4.11.0
```

Windows PowerShell:

```powershell
py -m venv .venv
.venv\Scripts\python.exe -m pip install esptool==4.11.0
```

Затем выполните команду записи. В комплект также входят скрипты
`flash_linux.sh` и `flash_windows.bat`.

Linux:

```bash
.venv/bin/python -m esptool --chip esp32c3 --port /dev/ttyACM0 --baud 460800 write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB 0x0 electronic_load_esp32c3_full.bin
```

Или запустите скрипт комплекта: `./flash_linux.sh /dev/ttyACM0`.

Если порт называется `/dev/ttyUSB0`, подставьте его вместо `/dev/ttyACM0`.

Windows PowerShell:

```powershell
.venv\Scripts\python.exe -m esptool --chip esp32c3 --port COM5 --baud 460800 write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB 0x0 electronic_load_esp32c3_full.bin
```

Либо запустите `flash_windows.bat COM5`.

Замените `COM5` на номер своего порта. Если на скорости 460800 появляются
ошибки передачи, повторите команду с `--baud 115200`.

## Альтернатива: записать три файла отдельно

Если программа прошивки не принимает объединённый образ, укажите три файла и
их адреса. Выполняйте команду из каталога комплекта.

```bash
python -m esptool --chip esp32c3 --port PORT --baud 460800 write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 electronic_load.bin
```

В Windows замените `python` на `py`, если так запускается Python. В Linux при
необходимости используйте `python3`.

## После записи

Дождитесь сообщения об успешной записи и автоматического перезапуска либо
нажмите RESET. Нагрузка при запуске выключена. Первичную проверку выполняйте
без подключённого аккумулятора и силовой нагрузки; затем проверьте индикацию,
кнопки, датчик и параметры Wi-Fi.

Объединённый образ записывает bootloader, таблицу разделов и приложение, но не
стирает область NVS отдельной командой. Сохранённые ранее настройки могут
остаться во flash. Полное стирание flash (`erase_flash`) удалит их; без
необходимости его выполнять не следует.

Если esptool сообщает, что не видит чип, проверьте выбранный порт, USB-кабель и
режим BOOT. Для Linux также проверьте права текущего пользователя на устройство
последовательного порта.
