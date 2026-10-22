// helpers/ui/telemetry_text.cpp: the joystick UI's rendering of a repeater's telemetry.
#include "test.h"
#include "CayenneLPP.h"
#include "ui/telemetry_text.h"

#include <string>

namespace {
std::string text(CayenneLPP &l) {
    char out[256];
    telemetry_text(l.getBuffer(), l.getSize(), out, sizeof(out));
    return out;
}
}

TEST(ttext_server_order, "UNIT-TTEXT-001", "A repeater's telemetry in slice-10 order renders every field, GPS second") {
    CayenneLPP l(180);
    l.addVoltage(1, 4.22f);
    l.addGPS(1, 47.1234f, -19.5f, 91.7f);
    l.addLuminosity(1, 3);
    l.addTemperature(2, -2.5f);
    l.addRelativeHumidity(2, 45.5f);
    l.addBarometricPressure(2, 1013.2f);
    l.addAltitude(2, 120.0f);
    l.addTemperature(1, 23.3f);
    CHECK(text(l) == "batt 4.22V\ngps 47.1234,-19.5000\nlight 3\nc2 temp -2.5C\n"
                     "c2 hum 45.5%\nc2 pres 1013.2hPa\nc2 alt 120m\ntemp 23.3C");
}

TEST(ttext_power, "UNIT-TTEXT-002", "Power monitor: channel voltage is not 'batt', current is signed") {
    CayenneLPP l(180);
    l.addVoltage(3, 12.10f);
    l.addCurrent(3, -0.0125f);
    l.addPower(3, 2.0f);
    CHECK(text(l) == "c3 12.10V\nc3 cur -13mA\nc3 pwr 2W");
}

TEST(ttext_unknown, "UNIT-TTEXT-003", "A type the screen does not show is skipped, not the end; empty is 'no telemetry'") {
    CayenneLPP l(180);
    l.addGenericSensor(4, 123456.0f);
    l.addVoltage(1, 3.70f);
    CHECK(text(l) == "batt 3.70V");
    CayenneLPP e(10);
    CHECK(text(e) == "no telemetry");
}

TEST(ttext_fields, "UNIT-TTEXT-004", "Fields: labels, units and channels for the touch UI; GPS to 5 decimals") {
    CayenneLPP l(180);
    l.addVoltage(1, 4.22f);
    l.addGPS(1, 47.1234f, -19.5f, 91.7f);
    l.addTemperature(2, -2.5f);
    l.addRelativeHumidity(2, 45.5f);
    l.addVoltage(3, 12.10f);
    l.addGenericSensor(4, 1.0f);
    l.addCurrent(3, -0.0125f);
    telemetry_field f[8];
    int n = telemetry_fields(l.getBuffer(), l.getSize(), f, 8);
    CHECK(n == 6);
    CHECK(f[0].channel == 1 && std::string(f[0].label) == "Battery" && std::string(f[0].value) == "4.22 V");
    CHECK(std::string(f[1].label) == "Position" && std::string(f[1].value) == "47.12340, -19.50000");
    CHECK(f[2].channel == 2 && std::string(f[2].label) == "Temperature" && std::string(f[2].value) == "-2.5 C");
    CHECK(std::string(f[3].label) == "Humidity" && std::string(f[3].value) == "45.5 %");
    CHECK(f[4].channel == 3 && std::string(f[4].label) == "Voltage" && std::string(f[4].value) == "12.10 V");
    CHECK(std::string(f[5].label) == "Current" && std::string(f[5].value) == "-13 mA");
}

TEST(ttext_fields_max, "UNIT-TTEXT-005", "Fields stop at max and at the end-of-data marker") {
    CayenneLPP l(180);
    l.addVoltage(1, 3.70f);
    l.addTemperature(1, 20.0f);
    l.addLuminosity(2, 7);
    telemetry_field f[2];
    CHECK(telemetry_fields(l.getBuffer(), l.getSize(), f, 2) == 2);
    uint8_t buf[16] = {1, 116 /* LPP_VOLTAGE */, 0x01, 0x72, 0, 0, 0, 0};
    telemetry_field g[4];
    CHECK(telemetry_fields(buf, sizeof(buf), g, 4) == 1);
    CHECK(std::string(g[0].value) == "3.70 V");
}
