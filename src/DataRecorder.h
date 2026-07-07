#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <glm/glm.hpp>
#include "CelestialBody.h"

// -----------------------------------------------------------------------
//  OutputParam — the quantity to be measured for a channel
// -----------------------------------------------------------------------
enum class OutputParam {
    DistanceToSun_km,      // |position relative to Sun|
    DistanceToParent_km,   // |position relative to parent body|
    Position_X_km,         // J2000 X position relative to Sun
    Position_Y_km,         // J2000 Y position relative to Sun
    Position_Z_km,         // J2000 Z position relative to Sun
    Speed_kms,             // scalar speed relative to parent
    RelSpeed_kms,          // scalar speed relative to Sun
};

inline const char* OutputParamName(OutputParam p) {
    switch (p) {
        case OutputParam::DistanceToSun_km:    return "Distance to Sun (km)";
        case OutputParam::DistanceToParent_km: return "Distance to Parent (km)";
        case OutputParam::Position_X_km:       return "X Position (km)";
        case OutputParam::Position_Y_km:       return "Y Position (km)";
        case OutputParam::Position_Z_km:       return "Z Position (km)";
        case OutputParam::Speed_kms:           return "Speed rel. to Parent (km/s)";
        case OutputParam::RelSpeed_kms:        return "Speed rel. to Sun (km/s)";
        default:                               return "Unknown";
    }
}

inline const char* OutputParamUnit(OutputParam p) {
    switch (p) {
        case OutputParam::DistanceToSun_km:
        case OutputParam::DistanceToParent_km:
        case OutputParam::Position_X_km:
        case OutputParam::Position_Y_km:
        case OutputParam::Position_Z_km:       return "km";
        case OutputParam::Speed_kms:
        case OutputParam::RelSpeed_kms:        return "km/s";
        default:                               return "";
    }
}

// -----------------------------------------------------------------------
//  OutputChannel — one time-series recorded for a specific body+param
// -----------------------------------------------------------------------
struct OutputChannel {
    std::string  bodyName;   // e.g. "EARTH"
    OutputParam  param  = OutputParam::DistanceToSun_km;
    bool         enabled = true;

    // Storage — parallel arrays kept in sync
    std::vector<double> times;   // ET seconds (ephemeris time)
    std::vector<double> values;  // computed measurement

    // For ImPlot we cache float copies (ImPlot works with float/double arrays)
    std::vector<double> timesDay; // time in days from sim start (for X axis display)

    // Maximum number of recorded samples before we stop recording (memory guard)
    static constexpr int MAX_SAMPLES = 100000;

    // Auto-generated legend label
    std::string label() const {
        return bodyName + " — " + OutputParamName(param);
    }

    void clear() {
        times.clear();
        values.clear();
        timesDay.clear();
    }

    bool isFull() const { return (int)times.size() >= MAX_SAMPLES; }
};

// -----------------------------------------------------------------------
//  DataRecorder — collects data for all channels at each sim step
// -----------------------------------------------------------------------
struct DataRecorder {
    std::vector<OutputChannel> channels;
    double simStartET = 0.0; // set when recording begins (for relative day axis)

    // Call this once at simulation start to anchor the time axis
    void setSimStart(double startET) {
        simStartET = startET;
    }

    // Call this inside the globalSimEt accumulator loop after planet positions are updated
    void record(double et, const std::vector<CelestialBody>& planets) {
        for (auto& ch : channels) {
            if (!ch.enabled || ch.isFull()) continue;

            // Find the body
            const CelestialBody* body = nullptr;
            for (const auto& p : planets) {
                if (p.Name == ch.bodyName) { body = &p; break; }
            }
            if (!body) continue;

            double value = 0.0;
            switch (ch.param) {
                case OutputParam::DistanceToSun_km:
                    // PositionKM is relative to Sun (body != Sun query returns pos w.r.t. Sun)
                    value = glm::length(body->PositionKM);
                    break;
                case OutputParam::DistanceToParent_km:
                    value = glm::length(body->RelativePositionKM);
                    break;
                case OutputParam::Position_X_km:
                    value = body->PositionKM.x;
                    break;
                case OutputParam::Position_Y_km:
                    value = body->PositionKM.y;
                    break;
                case OutputParam::Position_Z_km:
                    value = body->PositionKM.z;
                    break;
                case OutputParam::Speed_kms:
                    value = glm::length(body->RelativeVelocityKMS);
                    break;
                case OutputParam::RelSpeed_kms:
                    value = glm::length(body->VelocityKMS);
                    break;
            }

            double dayFromStart = (et - simStartET) / 86400.0;
            ch.times.push_back(et);
            ch.timesDay.push_back(dayFromStart);
            ch.values.push_back(value);
        }
    }

    // Clear all recorded data (call on simulation reset / rewind)
    void clear() {
        for (auto& ch : channels) ch.clear();
    }

    // Export all channels to a CSV file
    bool exportCSV(const std::string& path) const {
        std::ofstream f(path);
        if (!f.is_open()) return false;

        // Header
        f << "time_et_sec,day_from_start";
        for (const auto& ch : channels) {
            if (!ch.enabled) continue;
            f << "," << ch.bodyName << "_" << OutputParamName(ch.param);
        }
        f << "\n";

        // Find max row count
        size_t rows = 0;
        for (const auto& ch : channels)
            if (ch.times.size() > rows) rows = ch.times.size();

        for (size_t i = 0; i < rows; ++i) {
            // Use first enabled channel's time as the row time
            double t = 0.0, d = 0.0;
            for (const auto& ch : channels) {
                if (!ch.enabled) continue;
                if (i < ch.times.size()) { t = ch.times[i]; d = ch.timesDay[i]; }
                break;
            }
            f << t << "," << d;
            for (const auto& ch : channels) {
                if (!ch.enabled) continue;
                f << ",";
                if (i < ch.values.size()) f << ch.values[i];
            }
            f << "\n";
        }
        return true;
    }
};
