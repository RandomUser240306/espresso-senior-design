# ESP-IDF HX711 scale reader

This project uses a native ESP-IDF C driver for the HX711. It does not depend on
Arduino or the Arduino `HX711_ADC` library.

## Wiring

Connect the HX711 module's `VCC`, `GND`, `DOUT` and `SCK` pins to the ESP.
The example uses GPIO 4 for `DOUT` and GPIO 5 for `SCK`; change
`HX711_DOUT_GPIO` and `HX711_SCK_GPIO` in `main/main.c` to match your board.
Power the HX711 module at a voltage compatible with the ESP's GPIO levels
(typically 3.3 V) and share ground.

## Calibration

On startup the example warms up the converter and tares it using 10 readings.
`HX711_SCALE` in `main/main.c` is set to `704.44` raw counts per gram, based on
the observed change of about 50,720 counts with a 72 g load
(`50,720 / 72`). The reported measurement is
`(filtered_raw - tare_offset) / HX711_SCALE`. Adjust the constant if your
measurements differ; use a negative factor if the load cell direction is
reversed. The tare offset is recalculated at each restart.

The rolling filter holds 16 samples by default and discards the highest and
lowest samples when calculating the average. The driver accepts windows from
1 to 32 samples and HX711 gains of 32, 64, or 128.

## Desktop UI

The Tkinter UI presents a large kitchen-scale-style weight display and a
**TARE / ZERO** button. Choose the ESP32's serial port and connect to show live
readings. Close the ESP-IDF serial monitor first because only one program can
use a serial port at a time.

From the project directory, install the Python dependency and launch the app:

```powershell
py -m pip install -r requirements.txt
py host\scale_ui.py
```

Select the ESP32 COM port, click **Connect**, and wait for the UI to report that
the device is ready. Remove everything from the scale before pressing
**TARE / ZERO**. The firmware recalculates its zero offset from 10 HX711
readings. The UI and firmware communicate at 115200 baud using `@DATA`,
`@READY`, and `TARE` serial messages.

If the UI reports that the imported `serial` module is not PySerial, remove the
unrelated `serial` package and reinstall PySerial using the same Python
interpreter:

```powershell
py -3.13 -m pip uninstall serial
py -3.13 -m pip install --force-reinstall pyserial
```

## Build and flash

Build and flash with the ESP-IDF extension or the standard ESP-IDF CMake
workflow. Ensure the project target is set for your ESP chip.
