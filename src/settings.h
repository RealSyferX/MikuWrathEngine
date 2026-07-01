#pragma once
#include <string>
#include <cstdio>
#include <cstring>
#include <cstdlib>

struct Settings {
    char autoAttach[256] = {};
    int fontSize = 9;
    int debuggerType = 2; // 0=None, 1=VEH, 2=Windows

    void Save(const char* path = "miku_settings.ini") const {
        FILE* f = fopen(path, "w");
        if (!f) return;
        fprintf(f, "auto_attach=%s\n", autoAttach);
        fprintf(f, "font_size=%d\n", fontSize);
        fprintf(f, "debugger_type=%d\n", debuggerType);
        fclose(f);
    }

    void Load(const char* path = "miku_settings.ini") {
        FILE* f = fopen(path, "r");
        if (!f) return;
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "auto_attach=", 12) == 0) {
                strncpy(autoAttach, line + 12, sizeof(autoAttach) - 1);
                autoAttach[sizeof(autoAttach) - 1] = '\0';
                // strip newline
                char* nl = strchr(autoAttach, '\n');
                if (nl) *nl = '\0';
                char* cr = strchr(autoAttach, '\r');
                if (cr) *cr = '\0';
            } else if (strncmp(line, "font_size=", 10) == 0) {
                fontSize = atoi(line + 10);
                if (fontSize < 6) fontSize = 6;
                if (fontSize > 20) fontSize = 20;
            } else if (strncmp(line, "debugger_type=", 14) == 0) {
                debuggerType = atoi(line + 14);
                if (debuggerType < 0) debuggerType = 0;
                if (debuggerType > 2) debuggerType = 2;
            }
        }
        fclose(f);
    }
};
