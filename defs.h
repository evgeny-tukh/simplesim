#pragma once

#include <time.h>
#include <vector>
#include <string>

#define RAD_IN_DEG      0.01745329251994329576923690768489

#define MAX_RANGE       3704
#define MIN_RANGE       463
#define MIN_DISTANCE    10

#define NMEA            "Nmea"
#define LAMP            "Lamp"
#define TARGETS         "Targets"
#define OWN_SHIP        "OwnShip"
#define COMMON          "Common"

struct target {
    bool relative;
    double lat, lon;
    float brg, rng, cog;

    target (float _brg, float _rng, bool _relative): relative (_relative), lat (0.0), lon (0.0), brg (_brg), rng (_rng) {}
};

struct portCfg {
    int port, baud;
    time_t pause = 1;
    bool copyToConsole = false;
    bool readFromLog = false;
    std::string logPath;
};

struct config {
    bool fakeMode, readArpaDataFromLog, copyArpaToConsole;
    std::string arpaLogPath;
    portCfg nmea, lamp;
    int numOfTargets;
    std::vector<target> targets;
    double lat, lon;
    float cog, sog, hdg;
    float lampBrg, lampRng, lampHeight;
};

