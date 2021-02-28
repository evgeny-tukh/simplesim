#include <stdlib.h>
#include <stdio.h>
#include <cstdint>
#include <windows.h>
#include <Shlwapi.h>
#include <thread>
#include <chrono>
#include "defs.h"

void readConfig (config& cfg, char *cfgPath) {
    auto getIntValue = [cfgPath] (const char *section, char *key, int defValue = 0) {
        return GetPrivateProfileIntA (section, key, defValue, cfgPath);
    };
    auto getDoubleValue = [cfgPath] (const char *section, char *key, double defValue = 0.0) {
        char buffer [100];
        
        return (GetPrivateProfileStringA (section, key, "", buffer, sizeof (buffer), cfgPath) == 0) ? defValue : atof (buffer);
    };
    auto getStringValue = [cfgPath] (const char *section, char *key, char *defValue = "") {
        char buffer [500];
        
        GetPrivateProfileStringA (section, key, defValue, buffer, sizeof (buffer), cfgPath);

        return std::string (buffer);
    };
    auto getFlag = [cfgPath, getIntValue] (const char *section, char *key, bool defValue = false) {
        return getIntValue (section, key, defValue ? 1 : 0) != 0;
    };
    auto readPortCfg = [cfgPath, getIntValue, getStringValue, getFlag] (portCfg& cfg, const char *section) {
        cfg.port = getIntValue (section, "port", 1);
        cfg.baud = getIntValue (section, "baud", 38400);
        cfg.pause = getIntValue (section, "pause", 1);
        cfg.copyToConsole = getIntValue (section, "copyToConsole", 0) != 0;
        cfg.readFromLog = getFlag (section, "readFromLog");
        cfg.logPath = cfg.readFromLog ? getStringValue (section, "logPath") : "";
    };

    readPortCfg (cfg.nmea, NMEA);
    readPortCfg (cfg.lamp, LAMP);

    cfg.fakeMode = getFlag (COMMON, "fakeMode");
    cfg.readArpaDataFromLog = getFlag (TARGETS, "readFromLog");
    cfg.copyArpaToConsole = getFlag (TARGETS, "copyToConsole");
    cfg.arpaLogPath = getStringValue (TARGETS, "logPath");
    cfg.lat = getDoubleValue (OWN_SHIP, "lat", 59.0);
    cfg.lon = getDoubleValue (OWN_SHIP, "lon", 9.5);
    cfg.hdg = (float) getDoubleValue (OWN_SHIP, "hdg");
    cfg.cog = (float) getDoubleValue (OWN_SHIP, "cog");
    cfg.sog = (float) getDoubleValue (OWN_SHIP, "sog");

    cfg.lampBrg = (float) getDoubleValue (LAMP, "brg");
    cfg.lampHeight = (float) getDoubleValue (LAMP, "height", 10.0);
    cfg.lampRng = (float) getDoubleValue (LAMP, "rng", 0.2);

    cfg.numOfTargets = getIntValue (TARGETS, "numOfTargets");

    cfg.targets.clear ();

    for (int i = 1; i <= cfg.numOfTargets; ++ i) {
        char key [40];
        auto getKey = [i, &key] (const char *postfix) {
            sprintf (key, "target%02d:%s", i, postfix); return key;
        };
        
        cfg.targets.emplace_back ((float) getDoubleValue (TARGETS, getKey ("brg")), (float) getDoubleValue (TARGETS, getKey ("rng")), getIntValue (TARGETS, getKey ("relative")) != 0);
        cfg.targets.back ().cog = (float) getDoubleValue (TARGETS, getKey ("cog"));
    }
}

void updateLampConfig (config& cfg, char *cfgPath) {
    auto putDoubleValue = [cfgPath] (const char *section, char *key, double value) {
        char buffer [100];
        sprintf (buffer, "%f", value);
        WritePrivateProfileStringA (section, key, buffer, cfgPath);
    };

    putDoubleValue (LAMP, "brg", cfg.lampBrg);
    putDoubleValue (LAMP, "rng", cfg.lampRng);
}

HANDLE initPort (portCfg& cfg) {
    char portName [MAX_PATH];

    sprintf (portName, "\\\\.\\COM%d", cfg.port);

    HANDLE port = CreateFile (portName, GENERIC_READ | GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);

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

void finalizeSendSentence (HANDLE port, char *sentence, bool fakeMode, bool copyToConsole) {
    char tail [10];

    sprintf (tail, "%02X\r\n", calcCrc (sentence));
    strcat (sentence, tail);

    unsigned long bytesSent;

    if (copyToConsole) printf (sentence);
    if (!fakeMode) WriteFile (port, sentence, strlen (sentence), & bytesSent, 0);
}

char *getTimestampString (char *buffer) {
    time_t timestamp = time (0);
    tm *now = gmtime (& timestamp);
    
    sprintf (buffer, "%02d%02d%02d.00", now->tm_hour, now->tm_min, now->tm_sec);

    return buffer;
}

void sendLampSentence (HANDLE port, config& cfg) {
    char sentence [100];
    auto elevation = fabs (cfg.lampRng) < 1.0 ? 45 : (atan (cfg.lampHeight / cfg.lampRng) / RAD_IN_DEG);

    sprintf (sentence, "$PSMACK,01,%.1f,%.2f,100,00*", cfg.lampBrg, elevation);
    finalizeSendSentence (port, sentence, cfg.fakeMode, cfg.lamp.copyToConsole);
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
    finalizeSendSentence (port, sentence, cfg.fakeMode, cfg.nmea.copyToConsole);
}

void sendVTG (HANDLE port, config& cfg) {
    char sentence [100];

    sprintf (sentence, "$GPVTG,%05.1f,T,,M,%.1f,N,,K,D*", cfg.cog, cfg.sog);

    finalizeSendSentence (port, sentence, cfg.fakeMode, cfg.nmea.copyToConsole);
}

void sendHDT (HANDLE port, config& cfg) {
    char sentence [100];

    sprintf (sentence, "$HCHDT,%05.1f,T*", cfg.hdg);

    finalizeSendSentence (port, sentence, cfg.fakeMode, cfg.nmea.copyToConsole);
}

void sendTTM (HANDLE port, config& cfg, int trgNum) {
    char sentence [100];

    auto& target = cfg.targets.at (trgNum);

    //double brg = target.relative ? target.brg : target.brg - cfg.hdg;
    //double cog = target.relative ? target.cog : target.cog - cfg.hdg;

    sprintf (
        sentence,
        "$RATTM,%02d,%.3f,%05.1f,%c,%05.1f,%c,,N,#%02d,T,*",
        trgNum + 1, target.rng, target.brg, target.relative ? 'R' : 'T', target.cog, target.relative ? 'R' : 'T', trgNum + 1
    );

    finalizeSendSentence (port, sentence, cfg.fakeMode, cfg.nmea.copyToConsole);
}

uint8_t htodec (char chr) {
    if (chr >= '0' && chr <= '9') return chr - '0';
    if (chr >= 'A' && chr <= 'F') return chr - 'A' + 10;
    if (chr >= 'a' && chr <= 'f') return chr - 'a' + 10;
    return 0;
}

int splitFields (char *source, char *fields []) {
    int count = 1;
    fields [0] = source;
    uint8_t actualCrc = calcCrc (source);

    for (char *chr = source + 1; *chr; ++ chr) {
        if (*chr == ',') {
            *chr = '\0';
            fields [count++] = chr + 1;
        } else if (*chr == '*') {
            uint8_t crc = htodec (chr [1]) * 16 + htodec (chr [2]);

            if (crc != actualCrc) return 0;

            *chr = '\0'; break;
        }
    }

    return count;
}

double elevation2range (config& cfg, const double elevation) {
    double rangeValue;

    if (elevation > 1.0E-8 && elevation < 90.0) {
        double tangent = tan (elevation * RAD_IN_DEG);
        
        if (tangent > 1.0e-3)
            rangeValue = cfg.lampHeight / tangent;
        else
            rangeValue = 0.0;

        if (rangeValue < MIN_DISTANCE)
            rangeValue = MIN_DISTANCE;
        else if (rangeValue > MAX_RANGE)
            rangeValue = MAX_RANGE;
    } else {
        rangeValue = MAX_RANGE;
    }

    return rangeValue;
}

void parseLampFeedback (char *source, config& cfg, char *cfgPath) {
    char *fields [20];
    int numOfFields = splitFields (source, fields);

    if (numOfFields > 4) {
        int lampID = atoi (fields [0] + 1);
        double elevation = atof (fields [2]);

        if (lampID != 1) {
            printf ("Invalid lamp %d\n", lampID); return;
        }

        cfg.lampRng = elevation2range (cfg, elevation);
        cfg.lampBrg = atof (fields [1]);

        updateLampConfig (cfg, cfgPath);
    }
}

void readAvailableData (HANDLE port, config& cfg, char *cfgPath) {
    unsigned long errorFlags, bytesRead;
    COMSTAT commState;
    char buffer [5000];

    do {
        ClearCommError (port, & errorFlags, & commState);

        bool overflow = (errorFlags & (CE_RXOVER | CE_OVERRUN)) != 0L;

        if (commState.cbInQue > 0 && ReadFile (port, buffer, commState.cbInQue, & bytesRead, NULL) && bytesRead > 0) {
            buffer [bytesRead] = '\0';

            if (cfg.lamp.copyToConsole) printf (buffer);
            if (*buffer) parseLampFeedback (buffer, cfg, cfgPath);

            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
    } while (commState.cbInQue > 0);
}

HANDLE openLog (portCfg& cfg) {
    return cfg.readFromLog ? CreateFile (cfg.logPath.c_str (), GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0) : INVALID_HANDLE_VALUE;
}

HANDLE openLog (std::string& path) {
    return path.empty () ? INVALID_HANDLE_VALUE : CreateFile (path.c_str (), GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0);
}

void transferLogLine (HANDLE dest, HANDLE log, bool copyToConsole) {
    if (log != INVALID_HANDLE_VALUE) {
        std::string line;
        char chr;
        unsigned long bytesWritten, bytesRead;
        do {
            if (ReadFile (log, & chr, 1, & bytesRead, 0) && bytesRead > 0) {
                if (chr == '\r') continue;
                line += chr;
            }
        } while (chr != '\n');

        WriteFile (dest, line.c_str (), line.length (), & bytesWritten, 0);

        if (copyToConsole) printf (line.c_str ());
    }
}

void lampProcessor (config& cfg, char *cfgPath) {
    HANDLE port = initPort (cfg.lamp);
    HANDLE log = openLog (cfg.lamp);

    if (!cfg.fakeMode && port == INVALID_HANDLE_VALUE) {
        printf ("Unable to open lamp port.\n");
        exit (0);
    }

    while (true) {
        readConfig (cfg, cfgPath);
        if (cfg.lamp.readFromLog) {
            transferLogLine (port, log, cfg.lamp.copyToConsole);
        } else {
            sendLampSentence (port, cfg);
        }
        readAvailableData (port, cfg, cfgPath);
        std::this_thread::sleep_for (std::chrono::milliseconds (200));
    }

    CloseHandle (port);
}

void nmeaProcessor (config& cfg, char *cfgPath) {
    HANDLE port = initPort (cfg.nmea);
    HANDLE log = openLog (cfg.nmea);
    HANDLE arpaLog = cfg.readArpaDataFromLog ? openLog (cfg.arpaLogPath) : INVALID_HANDLE_VALUE;

    if (!cfg.fakeMode && port == INVALID_HANDLE_VALUE) {
        printf ("Unable to open nmea port.\n");
        exit (0);
    }

    time_t prevWrite = 0;

    while (true) {
        time_t now = time (0);
        
        if (true || (now - prevWrite) >= cfg.nmea.pause) {
            prevWrite = now;

            readConfig (cfg, cfgPath);

            if (cfg.nmea.readFromLog) {
                transferLogLine (port, log, cfg.nmea.copyToConsole);
            } else {
                sendGLL (port, cfg);
                sendVTG (port, cfg);
                sendHDT (port, cfg);

                if (cfg.readArpaDataFromLog) {
                    transferLogLine (port, arpaLog, cfg.copyArpaToConsole);
                } else {
                    for (int i = 0; i < cfg.numOfTargets; ++ i) {
                        sendTTM (port, cfg, i);
                    }
                }
            }
        }

        std::this_thread::sleep_for (std::chrono::milliseconds (500));
    }

    CloseHandle (port);
}

int main (int argCount, char *args []) {
    char cfgPath [MAX_PATH];
    config cfg;

    printf ("SearchLight Simple Simulator v2\n");

    if (argCount > 1) {
        strcpy (cfgPath, args [1]);
    } else {
        GetModuleFileNameA (0, cfgPath, sizeof (cfgPath));
        PathRenameExtensionA (cfgPath, ".cfg");
    }

    readConfig (cfg, cfgPath);

    auto nmea = new std::thread (nmeaProcessor, cfg, cfgPath);
    auto lamp = new std::thread (lampProcessor, cfg, cfgPath);

    nmea->join ();
    lamp->join ();
}