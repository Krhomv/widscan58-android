// FT6336 capacitive touch (I2C), mapped to the landscape screen. GPL-3.0-or-later.
#pragma once
#include <stdint.h>

void touchInit();
/** True while touched, with the point in screen coordinates. */
bool touchRead(int *x, int *y);
