# Portfolio / CV Notes

## Recommended Project Title
PID-Controlled Thermoelectric Cooling System for Cold Brew Coffee Extraction

## Short Portfolio Description
Designed and implemented an ESP32-based closed-loop thermoelectric cooling system using Peltier modules, DS18B20 temperature feedback, BTS7960 PWM power control, RTC timing, and a PID controller. The system was experimentally identified using open-loop testing and evaluated at multiple temperature setpoints. A later firmware version adds MQTT telemetry and a real-time web dashboard.

## CV Bullet Points
- Designed and implemented an ESP32-based thermoelectric cooling system with closed-loop PID temperature control.
- Performed open-loop system identification and Ziegler–Nichols PID tuning using experimentally measured thermal response data.
- Evaluated closed-loop performance at 5 °C, 8 °C, and 15 °C, with documented steady-state errors of approximately 0.026–0.037 °C.
- Integrated DS18B20 temperature sensing, BTS7960 PWM power drivers, RTC DS3231 timing, LCD interface, push-button controls, and buzzer alerts.
- Extended the embedded system with Wi-Fi/MQTT telemetry and a browser-based real-time monitoring dashboard.

## Skills Demonstrated
Embedded Systems • ESP32 • C/C++ • PID Control • System Identification • Ziegler–Nichols • PWM • Instrumentation • Sensor Calibration • Power Electronics • Thermoelectric Cooling • MQTT • IoT Dashboard • Experimental Data Analysis

## Interview Note
When explaining the cooling redesign, do not attribute the entire performance improvement only to the increase from two to four Peltier modules. The insulation and environmental test conditions also changed.

When explaining the repository, distinguish the thesis-baseline PID test parameters from the later MQTT firmware version because the current firmware contains updated PID/calibration constants.
