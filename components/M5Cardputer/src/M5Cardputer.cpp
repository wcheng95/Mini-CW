/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "driver/i2c_types.h"
#include "driver/gpio.h"
#include "M5Cardputer.h"

using namespace m5;

M5_CARDPUTER M5Cardputer;

void M5_CARDPUTER::beginDisplayOnly(bool enableKeyboard)
{
    _displayOnly = true;
    _enableKeyboard = enableKeyboard;

    Display.begin();
    Display.setRotation(1);

    if (enableKeyboard) {
        Keyboard.beginCardputerADV();
    }
}

void M5_CARDPUTER::begin(bool enableKeyboard)
{
    _displayOnly = false;
    M5.begin();
    _enableKeyboard = enableKeyboard;
    if (enableKeyboard) {
        Keyboard.begin();
    }
}

void M5_CARDPUTER::begin(m5::M5Unified::config_t cfg, bool enableKeyboard)
{
    _displayOnly = false;
    M5.begin(cfg);
    _enableKeyboard = enableKeyboard;
    if (enableKeyboard) {
        Keyboard.begin();
    }
}

void M5_CARDPUTER::update(void)
{
    if (!_displayOnly) {
        M5.update();
    }

    if (_enableKeyboard) {
        Keyboard.updateKeyList();
        Keyboard.updateKeysState();
    }
}