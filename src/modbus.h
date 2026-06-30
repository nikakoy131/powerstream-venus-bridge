#pragma once

/* Minimal Modbus-TCP server (port 502) serving the SunSpec register map.
   Only FC3 (read holding registers) is implemented — that's all Venus OS
   dbus-fronius needs. SunSpec unit id is 126. */
void modbus_start(void);
