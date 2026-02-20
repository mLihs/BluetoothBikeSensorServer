#pragma once
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class CpuLoadMonitorClass {
public:
    void begin(uint8_t core = 0) {
        idleCount = 0;
        idleBaseline = 0;
        lastCount = 0;

        xTaskCreatePinnedToCore(
            idleTaskWrapper,
            "idleTask",
            1024,
            this,
            0,
            NULL,
            core
        );

        delay(1000); // Task stabilisieren
        calibrate();
    }

    void print() {
        float load = getCpuLoad();
        Serial.printf("CPU Load: %.1f%%\n", load);
    }

    float getCpuLoad() {
        uint32_t current = idleCount;
        uint32_t diff = current - lastCount;
        lastCount = current;

        if (idleBaseline == 0) return 0;

        float idleRatio = (float)diff / (float)idleBaseline;
        float cpuLoad = 100.0f * (1.0f - idleRatio);

        if (cpuLoad < 0) cpuLoad = 0;
        if (cpuLoad > 100) cpuLoad = 100;

        return cpuLoad;
    }

private:
    volatile uint32_t idleCount = 0;
    uint32_t idleBaseline = 0;
    uint32_t lastCount = 0;

    static void idleTaskWrapper(void *param) {
        reinterpret_cast<CpuLoadMonitorClass*>(param)->idleTask();
    }

    void idleTask() {
        while (true) idleCount++;
    }

    void calibrate() {
        uint32_t start = millis();
        uint32_t startCount = idleCount;

        // 2 Sekunden Baseline
        while (millis() - start < 2000) { }

        uint32_t diff = idleCount - startCount;
        idleBaseline = diff / 2;  // loops pro Sekunde
    }
};

static CpuLoadMonitorClass CpuLoadMonitor;
