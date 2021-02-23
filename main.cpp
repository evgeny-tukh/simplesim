#include <stdlib.h>
#include <stdio.h>
#include <windows.h>
#include <Shlwapi.h>
#include <vector>
#include <time.h>
#include <cstdint>

struct target {
    bool relative;
    double lat, lon;
    float brg, rng;

    target (double _lat, double _lon): relative (false), lat (_lat), lon (_lon), brg (0.0f), rng (0.0f) {}
    target (float _brg, float _rng): relative (true), lat (0.0), lon (0.0), brg (_brg), rng (_rng) {}
};

struct config {
    int port, baud;
    int numOfTargets;
    time_t pause;
    std::vector<target> targets;
    double lat, lon;
    float cog, sog, hdg;
};

void readConfig (config& cfg, char *cfgPath) {
    static const char *OWN_SHIP = "OwnShip";
    static const char *TARGETS = "Targets";
    static const char *COMM = "Comm";

    auto getIntValue = [cfgPath] (const char *section, char *key, int defValue = 0) {
        return GetPrivateProfileIntA (section, key, defValue, cfgPath);
    };
    auto getDoubleValue = [cfgPath] (const char *section, char *key, double defValue = 0.0) {
        char buffer [100];
        
        return (GetPrivateProfileStringA (section, key, "", buffer, sizeof (buffer), cfgPath) == 0) ? defValue : atof (buffer);
    };

    cfg.port = getIntValue (COMM, "port", 1);
    cfg.baud = getIntValue (COMM, "baud", 38400);
    cfg.pause = getIntValue (COMM, "pause", 1);

    cfg.lat = getDoubleValue (OWN_SHIP, "lat", 59.0);
    cfg.lon = getDoubleValue (OWN_SHIP, "lon", 9.5);
    cfg.hdg = (float) getDoubleValue (OWN_SHIP, "hdg");
    cfg.cog = (float) getDoubleValue (OWN_SHIP, "cog");
    cfg.sog = (float) getDoubleValue (OWN_SHIP, "sog");

    cfg.numOfTargets = getIntValue (TARGETS, "numOfTargets");

    cfg.targets.clear ();

    for (int i = 1; i <= cfg.numOfTargets; ++ i) {
        char key [40];
        auto getKey = [i, &key] (const char *postfix) {
            sprintf (key, "target%02d:%s", i, postfix); return key;
        };
        
        if (getIntValue (TARGETS, getKey ("relative"))) {
            cfg.targets.emplace_back ((float) getDoubleValue (TARGETS, getKey ("brg")), (float) getDoubleValue (TARGETS, getKey ("rng")));
        } else {
            cfg.targets.emplace_back (getDoubleValue (TARGETS, getKey ("lat")), getDoubleValue (TARGETS, getKey ("lon")));
        }
    }
}

HANDLE init (config& cfg) {
    char portName [MAX_PATH];

    sprintf (portName, "\\\\.\\COM%d", cfg.port);

    HANDLE port = CreateFile (portName, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);

    if (port != INVALID_HANDLE_VALUE) {
        DCB portCfg;
        COMMTIMEOUTS timeouts;

        memset (& portCfg, 0, sizeof (portCfg));
        portCfg.DCBlength = sizeof (portCfg);

        timeouts.ReadIntervalTimeout = 1000;
        timeouts.ReadTotalTimeoutMultiplier = 1;
        timeouts.ReadTotalTimeoutConstant = 3000;

        SetupComm (port, 4096, 4096); 
        PurgeComm (port, PURGE_TXABORT | PURGE_RXABORT | PURGE_TXCLEAR | PURGE_RXCLEAR); 
        GetCommState (port, & portCfg);

        portCfg.fInX     = 
        portCfg.fOutX    =
        portCfg.fParity  =
        portCfg.fBinary  = 1;
        portCfg.BaudRate = cfg.baud;
        portCfg.ByteSize = 8;
        portCfg.fParity = NOPARITY;
        portCfg.StopBits = ONESTOPBIT;
        portCfg.XonChar = 0x11;
        portCfg.XoffChar = 0x13;

        SetCommTimeouts (port, & timeouts);
        SetCommState (port, & portCfg);
    }

    return port;
}

void formatLat (double lat, char *buffer, char *sign) {
    double absValue = fabs (lat);
    int degree = (int) absValue;
    double minutes = (absValue - (double) degree) * 60.0;

    sprintf (buffer, "%02d%06.3f", degree, minutes);
    *sign = lat >= 0.0 ? 'N' : 'S';
}

void formatLon (double lon, char *buffer, char *sign) {
    double absValue = fabs (lon);
    int degree = (int) absValue;
    double minutes = (absValue - (double) degree) * 60.0;

    sprintf (buffer, "%03d%06.3f", degree, minutes);
    *sign = lon >= 0.0 ? 'E' : 'W';
}

uint8_t calcCrc (char *sentence) {
    uint8_t crc = sentence [1];

    for (int i = 2; sentence [i] != '*'; crc ^= sentence [i++]);

    return crc;
}

void finalizeSendSentence (HANDLE port, char *sentence) {
    char tail [10];

    sprintf (tail, "%02X\r\n", calcCrc (sentence));
    strcat (sentence, tail);

    unsigned long bytesSent;

    WriteFile (port, sentence, strlen (sentence), & bytesSent, 0);
}

char *getTimestampString (char *buffer) {
    time_t timestamp = time (0);
    tm *now = gmtime (& timestamp);
    
    sprintf (buffer, "%02d%02d%02d.00", now->tm_hour, now->tm_min, now->tm_sec);

    return buffer;
}

void sendGLL (HANDLE port, config& cfg) {
    char sentence [100];
    char lat [10], lon [10];
    char latSign, lonSign;
    char utc [20];

    formatLat (cfg.lat, lat, & latSign);
    formatLon (cfg.lon, lon, & lonSign);
    getTimestampString (utc);

    sprintf (sentence, "$GPGLL,%s,%c,%s,%c,%s,A,D*", lat, latSign, lon, lonSign, utc);
    finalizeSendSentence (port, sentence);
}

void sendVTG (HANDLE port, config& cfg) {
    char sentence [100];

    sprintf (sentence, "$GPVTG,%05.1f,T,,M,%.1f,N,,K,D*", cfg.cog, cfg.sog);

    finalizeSendSentence (port, sentence);
}

void sendHDT (HANDLE port, config& cfg) {
    char sentence [100];

    sprintf (sentence, "$HCHDT,%05.1f,T*", cfg.hdg);

    finalizeSendSentence (port, sentence);
}

void process (config& cfg, char *cfgPath) {
    HANDLE port = init (cfg);

    if (port == INVALID_HANDLE_VALUE) {
        printf ("Unable to open port.\n");
        exit (0);
    }

    time_t prevWrite = 0;

    while (true) {
        time_t now = time (0);
        
        if ((now - prevWrite) >= cfg.pause) {
            prevWrite = now;

            readConfig (cfg, cfgPath);

            sendGLL (port, cfg);
            sendVTG (port, cfg);
            sendHDT (port, cfg);
        }
    }
}

int main (int argCount, char *args []) {
    char cfgPath [MAX_PATH];
    config cfg;

    printf ("SearchLight Simple Simulator v2\n");

    if (argCount > 0) {
        strcpy (cfgPath, args [1]);
    } else {
        GetModuleFileNameA (0, cfgPath, sizeof (cfgPath));
        PathRenameExtensionA (cfgPath, ".cfg");
    }

    readConfig (cfg, cfgPath);
    process (cfg, cfgPath);
}