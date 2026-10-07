// =====================================================================
//  cli.h — Serial console (ผ่านสาย USB) สำหรับดีบักและสาธิต
//  เปิด Serial Monitor ของ PlatformIO แล้วพิมพ์  help  + Enter
// =====================================================================
#pragma once
#include <Arduino.h>

namespace cli {

void poll();              // เรียกใน loop() (Arduino loopTask)

}  // namespace cli
