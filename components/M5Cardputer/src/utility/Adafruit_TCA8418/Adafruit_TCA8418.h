/*!
 *  @file Adafruit_TCA8418.h
 *
 * 	I2C Driver for the Adafruit TCA8418 Keypad Matrix / GPIO Expander
 *Breakout
 *
 * 	This is a library for the Adafruit TCA8418 breakout:
 * 	https://www.adafruit.com/products/4918
 *
 * 	Adafruit invests time and resources providing this open source code,
 *  please support Adafruit and open-source hardware by purchasing products from
 * 	Adafruit!
 *
 *
 *	BSD license (see license.txt)
 */

#ifndef _ADAFRUIT_TCA8418_H
#define _ADAFRUIT_TCA8418_H

// #include <Arduino.h>
// #include <Adafruit_I2CDevice.h>
// #include <Adafruit_I2CRegister.h>
#include "Adafruit_TCA8418_registers.h"
#include <M5Unified.hpp>
#include <functional>
#include "driver/i2c_master.h"

#define TCA8418_DEFAULT_ADDR 0x34  ///< The default I2C address for our breakout

/** Pin IDs for matrix rows/columns */
enum {
    TCA8418_ROW0,  // Pin ID for row 0
    TCA8418_ROW1,  // Pin ID for row 1
    TCA8418_ROW2,  // Pin ID for row 2
    TCA8418_ROW3,  // Pin ID for row 3
    TCA8418_ROW4,  // Pin ID for row 4
    TCA8418_ROW5,  // Pin ID for row 5
    TCA8418_ROW6,  // Pin ID for row 6
    TCA8418_ROW7,  // Pin ID for row 7
    TCA8418_COL0,  // Pin ID for column 0
    TCA8418_COL1,  // Pin ID for column 1
    TCA8418_COL2,  // Pin ID for column 2
    TCA8418_COL3,  // Pin ID for column 3
    TCA8418_COL4,  // Pin ID for column 4
    TCA8418_COL5,  // Pin ID for column 5
    TCA8418_COL6,  // Pin ID for column 6
    TCA8418_COL7,  // Pin ID for column 7
    TCA8418_COL8,  // Pin ID for column 8
    TCA8418_COL9   // Pin ID for column 9
};

enum {
    TCA8418_LOW  = 0,
    TCA8418_HIGH = 1,
};

enum {
    TCA8418_INPUT = 0,
    TCA8418_OUTPUT,
    TCA8418_INPUT_PULLUP,
};

enum {
    TCA8418_RISING = 0,
    TCA8418_FALLING,
};

/*!
 *    @brief  Class that stores state and functions for interacting with
 *            the TCA8418 I2C GPIO expander
 */
class Adafruit_TCA8418 : public m5::I2C_Device {
public:
    Adafruit_TCA8418(std::uint8_t i2c_addr = TCA8418_DEFAULT_ADDR,
                     std::uint32_t freq = 400000,
                     m5::I2C_Class* i2c = &m5::In_I2C);

    ~Adafruit_TCA8418();

    bool begin();

    bool matrix(uint8_t rows, uint8_t columns);

    uint8_t available();
    uint8_t getEvent();
    uint8_t flush();

    uint8_t digitalRead(uint8_t pinnum);
    bool digitalWrite(uint8_t pinnum, uint8_t level);
    bool pinMode(uint8_t pinnum, uint8_t mode);
    bool pinIRQMode(uint8_t pinnum, uint8_t mode);

    void enableInterrupts();
    void disableInterrupts();

    void enableMatrixOverflow();
    void disableMatrixOverflow();

    void enableDebounce();
    void disableDebounce();

    // New: use shared ESP-IDF board_i2c instead of M5.In_I2C.
    uint8_t readRegister8(uint8_t reg);
    bool writeRegister8(uint8_t reg, uint8_t value);

private:
    bool ensureDevice();

    uint8_t _addr = TCA8418_DEFAULT_ADDR;
    uint32_t _freq = 400000;
    i2c_master_dev_handle_t _dev_handle = nullptr;
};

#endif