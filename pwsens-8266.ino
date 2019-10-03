#include "heltec.h"          // Heltec_ESP8266_Dev-Boards
#include "Adafruit_INA260.h" // Adafruit_INA260
#include "EasyButton.h"      // button de-bouncer, sequencer

// both the SSD1306 OLED and the INA260 are wired up to the same I2C bus on the
// ESP8266, pins 16 (SDA) and 24 (SCL)
OLEDDisplayUi ui = OLEDDisplayUi(Heltec.display);
Adafruit_INA260 ina260 = Adafruit_INA260();

// button library for de-bouncing the signal, recognizing press sequences,
// and other features. on pin 0 of the ESP8266.
EasyButton modeButton(0);

// the default screen orientation is actually upside down with respect to the
// PCB silkscreen. use this macro to flip it the other way.
//#define DISP_UPSIDE_DOWN

// inverting the colors kinda looks cool, but leave it disabled until the user
// long-presses the mode button.
//#define DISP_INVERT_COLORS

// the direction of current in my implementation is guaranteed. any negative
// feedback is not something we want to display.
//#define DISP_VOLTAGE_POS_ONLY
#define DISP_CURRENT_POS_ONLY

static const size_t FORMAT_BUF_SIZE  =   32;
static const int    FORMAT_VAL_WIDTH =    6;
static const size_t LONGPRESS_DUR_MS = 1500;

static int  currentFrame   = -1; // currently displayed OLED screen
static bool flippedFrame   = false; // screen is upside down (true) or not
static bool invertedColors = false; // invert the monochrome colors or not

class RangedUnit {

private:
  char  _str[FORMAT_BUF_SIZE];
  float _fmin, _fmax;
  float _fmult;
  int   _dig;

  static inline float _pow10(int exp) { // lookup table for powers of 10
#define POW10_MIN -5
#define POW10_MAX  5
    static const float P[11] = { // powers in range [-5, 5]
      1.e-5, 1.e-4, 1.e-3, 1.e-2, 1.e-1, 1.e0, 1.e+1, 1.e+2, 1.e+3, 1.e+4, 1.e+5
    };
    if (exp >= POW10_MIN && exp <= POW10_MAX) {
      return P[exp + POW10_MAX];
    }
    return 0.0; // error, 0 is not a power of 10
#undef POW10_MIN
#undef POW10_MAX
  }

public:
  // common constructor
  RangedUnit(char *str, float fmin, float fmax, float fmult, int dig):
    _fmin(fmin), _fmax(fmax), _fmult(fmult), _dig(dig) {
    strncpy(_str, str, FORMAT_BUF_SIZE);
  }

  // check if a given input value exists within the object's valid range, and
  // formats the value accordingly with units suffix and a dynamic precision.
  bool inRange(float val, char *out) {

    // heap storage for the intermediate formatting strings
    static char buf[FORMAT_BUF_SIZE] = { 0 };

    // check if the input value is actually within our range (max-excluded)
    if (val >= _fmin && val < _fmax) {

      // scale the input value to the units of our object
      float scaled = val * _fmult;

      // count the number of digits to the left of the decimal point, and
      // subtract that from our displayed number of significant digits (_dig).
      // the result is the number of digits to display to the right of the
      // decimal point.
      int prec = _dig - 1; // trailing digits to show when value is 0
      if (fabs(scaled) > _pow10(-_dig)) {
        prec = _dig - (int)log10(fabs(scaled)) - 1;
      }
      if (prec < 0) { prec = 0; }

      // dtostrf() is from AVR stdlib's libm.a -- it provides the printf-style
      // formatting for floats missing from some BSPs sprintf implementation.
      // args are similar, e.g. "%-6.4f" <=> dtostrf(val, -6, 4, buf)

      // first, format the floating point value, converting it to a string.
      dtostrf(scaled, FORMAT_VAL_WIDTH, prec, buf);

      // next, format the output string including the formatted float and units.
      snprintf(out, FORMAT_BUF_SIZE, "%*s %s", FORMAT_VAL_WIDTH, buf, _str);

      return true;
    }

    return false;
  } // inRange()

};

void overlaySystemTime(OLEDDisplay *display, OLEDDisplayUiState* state) {

  display->setTextAlignment(TEXT_ALIGN_RIGHT);
  display->setFont(ArialMT_Plain_10);
  display->drawString(128, 0, String(millis()));

}

void frameVoltage(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {

  // ranges for deciding which units to display for each sampled data item at
  // each draw cycle. the base units input would correspond to the defined range
  // containing 0.0 below (millivolts and milliamps for the INA260).
  //
  // the ranges are inclusive with the minimum, and exclusive with the maximum.
  // the scaling factor (applied to the input for producing units in range) and
  // number of significant digits to display are also defined.
  //
  // NOTE:
  //   if you change the number of significant digits to display, make sure the
  //   value is supported by the RangedUnit::_pow10() lookup table, otherwise
  //   expand the table to include it.

  enum { _vunMillivolts, _vunVolts, _vunCOUNT };
  static RangedUnit * const VOLTAGE_UNITS[_vunCOUNT] = {
    new RangedUnit((char *)"mV",   0.0F,    1000.0F, 1.0F,   4),
    new RangedUnit((char *)"V", 1000.0F, 1000000.0F, 0.001F, 5),
  };

  enum { _cunMilliamps,  _cunAmps,  _cunCOUNT };
  static RangedUnit * const CURRENT_UNITS[_cunCOUNT] = {
    new RangedUnit((char *)"mA",   0.0F,    1000.0F, 1.0F,   4),
    new RangedUnit((char *)"A", 1000.0F, 1000000.0F, 0.001F, 5),
  };

  // heap storage for our output string buffers
  static char voltageStr[FORMAT_BUF_SIZE];
  static char currentStr[FORMAT_BUF_SIZE];

  // query the INA260 for voltage and current, keeping the value in its original
  // units; readBusVoltage() is millivolts, readCurrent() is milliamps
  float voltage = ina260.readBusVoltage();
  float current = ina260.readCurrent();

  // cap the values if desired
#if defined(DISP_VOLTAGE_POS_ONLY)
  voltage = max(0.0F, voltage);
#endif
#if defined(DISP_CURRENT_POS_ONLY)
  current = max(0.0F, current);
#endif

  // locate the range in which the reported bus voltage (in millivolts) exists
  for (int i = 0;
    // store the result in the voltage string buffer; then break out
    i < _vunCOUNT && !VOLTAGE_UNITS[i]->inRange(voltage, voltageStr);
    ++i); // don't touch the output string if no valid range was found

  // locate the range in which the reported current (in milliamps) exists
  for (int i = 0;
    // store the result in the current string buffer; then break out
    i < _cunCOUNT && !CURRENT_UNITS[i]->inRange(current, currentStr);
    ++i); // don't touch the output string if no valid range was found

  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->setFont(ArialMT_Plain_16);

  display->drawString(x, y +  0, voltageStr);
  display->drawString(x, y + 16, currentStr);

  delay(100);

}

void frameErrorINA260(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {

  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->setFont(ArialMT_Plain_16);
  display->drawString(x, y +  0, "failed to init");
  display->drawString(x, y + 16, "INA260");

}

enum { oidSystemTime, oidCOUNT };
OverlayCallback overlays[oidCOUNT] =
  { overlaySystemTime };

enum { fidVoltage, fidErrorINA260, fidCOUNT };
FrameCallback frames[fidCOUNT] =
  { frameVoltage, frameErrorINA260 };

void modeButtonPressed() {

  switch (currentFrame) {
    case fidVoltage:
      Heltec.display->resetDisplay();
      if (flippedFrame) { Heltec.display->resetOrientation(); }
                   else { Heltec.display->flipScreenVertically(); }
      flippedFrame = !flippedFrame;
      break;

    case fidErrorINA260:
    default:
      break;
  }

}

void modeButtonLongPressed() {

  switch (currentFrame) {
    case fidVoltage:
      Heltec.display->resetDisplay();
      if (invertedColors) { Heltec.display->normalDisplay(); }
                     else { Heltec.display->invertDisplay(); }
      invertedColors = !invertedColors;
      break;

    case fidErrorINA260:
    default:
      break;
  }

}

void setup() {

  Heltec.begin(true/*OLED*/, true/*Serial(115200)*/);
  ui.setTargetFPS(30);
  ui.disableAutoTransition();
  ui.disableAllIndicators();

  ui.setOverlays(overlays, oidCOUNT);
  ui.setFrames(frames, fidCOUNT);

  if (ina260.begin()) {
    currentFrame = fidVoltage;
    ina260.setMode(INA260_MODE_CONTINUOUS);
  }
  else {
    currentFrame = fidErrorINA260;
    ui.switchToFrame(fidErrorINA260);
  }

  ui.init();

#if !defined(DISP_UPSIDE_DOWN)
  flippedFrame = true;
  Heltec.display->flipScreenVertically();
#endif

#if defined(DISP_INVERT_COLORS)
  invertedColors = true;
  Heltec.display->invertDisplay();
#endif

  modeButton.onPressed(modeButtonPressed);
  modeButton.onPressedFor(LONGPRESS_DUR_MS, modeButtonLongPressed);
  modeButton.begin();
}

void loop() {

  modeButton.read();

  int remainingTimeBudget = ui.update();

  if (remainingTimeBudget > 0) {
    delay(remainingTimeBudget);
  }

}

