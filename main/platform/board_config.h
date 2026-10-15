#pragma once

/* ESP32-C3 board wiring defaults. Override here to match the PCB. */
#define BOARD_I2C_SDA_GPIO       8
#define BOARD_I2C_SCL_GPIO       9
#define BOARD_LOAD_PWM_GPIO      10
#define BOARD_BUTTON_DOWN_GPIO   1
#define BOARD_BUTTON_UP_GPIO     2
#define BOARD_BUTTON_SELECT_GPIO 3
#define BOARD_BUTTON_BACK_GPIO   4
#define BOARD_OLED_I2C_ADDRESS   0x3C
#define BOARD_INA219_I2C_ADDRESS 0x40

/* Read-only monitoring access point. Change these before deployment. */
#define BOARD_WIFI_AP_SSID       "ElectronicLoad"
#define BOARD_WIFI_AP_PASSWORD   "load8264"
#define BOARD_WIFI_AP_CHANNEL    1
#define BOARD_WIFI_AP_MAX_CLIENTS 4

/* INA219 module shunt. Verify the resistor marking before applying power. */
#define INA219_SHUNT_OHMS        0.100f
