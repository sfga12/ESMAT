#pragma once

#include <string>
#include <vector>
#include <memory>
#include <glm/glm.hpp>
#include "CelestialBody.h"

class Spacecraft; // Forward declaration

// 0 = GET (Time), 1 = Orbital Event (Apo/Peri), 2 = Altitude Target
enum class TriggerType { GET = 0, APSIS = 1, ALTITUDE = 2 };

struct BurnEntry {
    TriggerType trigger;
    
    // Parameters
    double get_h, get_m, get_s; // For GET
    int apsisType;              // For APSIS: 0 = Apoapsis, 1 = Periapsis
    double targetAltKM;         // For ALTITUDE
    int altCondition;           // For ALTITUDE: 0=< 1=<= 2=>= 3=>
    int altRefBodyID;           // For ALTITUDE: 399=Earth, 301=Moon, etc.
    
    // Burn Vector
    double dvx, dvy, dvz;       // km/s
    int refBodyID;              // 0=current center, 301=Moon, 399=Earth
    bool isVNB;                 // True: VNB vector, False: absolute J2000 vector
    bool enabled;
};

extern std::vector<BurnEntry> g_BurnTable;

// Base class for all mission commands
class MissionCommand {
public:
    virtual ~MissionCommand() = default;

    virtual std::string GetName() const = 0;
    
    // Called once when the command starts
    virtual void Initialize(Spacecraft* sc, double currentTime) {}

    // Called every physics step. 
    // Returns > 0.0 if the command has finished executing (returning the UNUSED dt fraction so the next command can use it)
    // Returns <= 0.0 if the command is still legitimately running and consumed the whole dt.
    virtual double Execute(Spacecraft* sc, double currentTime, double dt, const std::vector<CelestialBody>& planets) = 0;

    virtual bool IsInstantaneous() const { return false; }

    // Resets the command state so it can be re-run from the beginning
    virtual void Reset() = 0;
};

// ==========================================
// PROPAGATE COMMAND
// Propagates the spacecraft until a condition is met
// For now, only Time/Duration based propagation is implemented
// ==========================================
class PropagateCommand : public MissionCommand {
public:
    double durationSeconds;
    double startTime = 0.0;
    bool isInitialized = false;

    PropagateCommand(double durationSec) : durationSeconds(durationSec) {}

    std::string GetName() const override {
        return "Propagate (" + std::to_string(durationSeconds / 86400.0) + " days)";
    }

    void Initialize(Spacecraft* sc, double currentTime) override {
        startTime = currentTime;
        isInitialized = true;
    }

    double Execute(Spacecraft* sc, double currentTime, double dt, const std::vector<CelestialBody>& planets) override {
        if (!isInitialized) Initialize(sc, currentTime);

        double elapsed = std::abs(currentTime - startTime);
        double futureElapsed = elapsed + dt;
        
        if (futureElapsed >= durationSeconds) {
            double consumed_dt = durationSeconds - elapsed;
            if (consumed_dt < 0.0) consumed_dt = 0.0;
            return dt - consumed_dt; // Return remaining unused dt
        }
        
        return 0.0; // Still propagating, consumed full dt
    }

    void Reset() override {
        isInitialized = false;
        startTime = 0.0;
    }
};

// ==========================================
// PROPAGATE TO GET (Ground Elapsed Time) COMMAND
// Propagates the spacecraft until a specific GET from mission start is reached
// ==========================================
extern double start_et_time; // Global simulation start time reference

class PropagateToGETCommand : public MissionCommand {
public:
    double targetGET_seconds; // GET from mission start
    double missionEpochET;    // Spacecraft's specific epoch
    bool isInitialized = false;

    PropagateToGETCommand(double get_sec, double epoch_et) 
        : targetGET_seconds(get_sec), missionEpochET(epoch_et) {}


    std::string GetName() const override {
        int h = (int)(targetGET_seconds / 3600);
        int m = (int)((targetGET_seconds - h*3600) / 60);
        double s = targetGET_seconds - h*3600 - m*60;
        char buf[64];
        snprintf(buf, sizeof(buf), "Propagate to GET %02d:%02d:%05.2f", h, m, s);
        return std::string(buf);
    }

    void Initialize(Spacecraft* sc, double currentTime) override {
        isInitialized = true;
    }

    double Execute(Spacecraft* sc, double currentTime, double dt, 
                 const std::vector<CelestialBody>& planets) override {
        double currentGET = currentTime - missionEpochET;
        double futureGET = currentGET + dt;
        
        // If we will reach or pass the target GET within this time step
        if (futureGET >= targetGET_seconds) {
            double consumed_dt = targetGET_seconds - currentGET;
            if (consumed_dt < 0.0) consumed_dt = 0.0;
            return dt - consumed_dt; // Return the fraction of dt not used
        }
        return 0.0; // Consumed full dt, keep waiting
    }

    void Reset() override { isInitialized = false; }
};

// ==========================================
// PROPAGATE TO ALTITUDE COMMAND
// Propagates the spacecraft until a specific altitude condition is met
// condition: 0 = LT (<), 1 = LE (<=), 2 = GE (>=), 3 = GT (>)
// ==========================================
class PropagateToAltitudeCommand : public MissionCommand {
public:
    int targetSpiceID;
    double targetAltitudeKM;
    int condition; // 0 = < (LT), 1 = <= (LE), 2 = >= (GE), 3 = > (GT)
    double minAltitudeReached;
    bool hasLoggedMiss = false;
    int logStepCount = 0; // For periodic altitude diagnostics

    // Legacy ctor for descending bool compatibility
    PropagateToAltitudeCommand(int spiceID, double altitudeKM, bool isDescending = true) 
        : targetSpiceID(spiceID), targetAltitudeKM(altitudeKM), condition(isDescending ? 1 : 2), minAltitudeReached(1e12) {}
    
    PropagateToAltitudeCommand(int spiceID, double altitudeKM, int conditionEnum) 
        : targetSpiceID(spiceID), targetAltitudeKM(altitudeKM), condition(conditionEnum), minAltitudeReached(1e12) {}

    std::string GetName() const override {
        const char* ops[] = {"<", "<=", ">=", ">"};
        return "Propagate to Altitude (" + std::to_string(targetSpiceID) + " " + ops[condition < 4 ? condition : 1] + " " + std::to_string((long long)targetAltitudeKM) + " km)";
    }

    double Execute(Spacecraft* sc, double currentTime, double dt, const std::vector<CelestialBody>& planets) override;

    void Reset() override {
        minAltitudeReached = 1e12;
        hasLoggedMiss = false;
        logStepCount = 0;
    }
};

// ==========================================
// PROPAGATE TO APSIS COMMAND
// Propagates the spacecraft until Apoapsis or Periapsis is reached
// ==========================================
class PropagateToApsisCommand : public MissionCommand {
public:
    bool targetApoapsis;
    int refBodyID;
    bool hasInitialized = false;

    PropagateToApsisCommand(bool apoapsis = true, int refBody = 0) : targetApoapsis(apoapsis), refBodyID(refBody) {}

    std::string GetName() const override {
        return targetApoapsis ? "Propagate to Apoapsis" : "Propagate to Periapsis";
    }

    double Execute(Spacecraft* sc, double currentTime, double dt, const std::vector<CelestialBody>& planets) override;

    void Reset() override {
        hasInitialized = false;
    }
};

// ==========================================
// IMPULSIVE BURN COMMAND
// Applies an instantaneous delta-V to the spacecraft
// ==========================================
class ImpulsiveBurnCommand : public MissionCommand {
public:
    glm::dvec3 deltaV_KMS; // Delta V vector
    bool isVNBFrame;       // True: VNB frame, False: J2000 Inertial frame
    int refBodyID;         // If non-zero, VNB frame is calculated relative to this body's state
    bool hasExecuted = false;

    ImpulsiveBurnCommand(glm::dvec3 dV, bool vnb, int refBody = 0) 
        : deltaV_KMS(dV), isVNBFrame(vnb), refBodyID(refBody) {}

    std::string GetName() const override {
        return "Impulsive Burn (" + (isVNBFrame ? std::string("VNB") : std::string("J2000")) + ")";
    }

    double Execute(Spacecraft* sc, double currentTime, double dt, const std::vector<CelestialBody>& planets) override;

    bool IsInstantaneous() const override { return true; }

    void Reset() override {
        hasExecuted = false;
    }
};

// ==========================================
// MISSION SEQUENCE
// Manages strictly ordered execution of commands
// ==========================================
class MissionSequence {
public:
    std::vector<std::shared_ptr<MissionCommand>> commands;
    int currentCommandIndex = 0;
    bool isFinished = false;

    void AddCommand(std::shared_ptr<MissionCommand> cmd) {
        commands.push_back(cmd);
    }

    // Called every physics step to progress the sequence. 
    // Returns the fraction of dt that was NOT consumed (if it finished a command early).
    double ExecuteStep(Spacecraft* sc, double currentTime, double dt, const std::vector<CelestialBody>& planets) {
        if (commands.empty() || isFinished) return dt;

        double remaining_dt = dt;
        
        while (currentCommandIndex < commands.size() && !isFinished && remaining_dt >= 0.0) {
            double current_dt_to_use = remaining_dt;
            double leftover = commands[currentCommandIndex]->Execute(sc, currentTime, remaining_dt, planets);
            
            if (leftover > 0.0 || commands[currentCommandIndex]->IsInstantaneous()) {
                // Command finished.
                remaining_dt = leftover; 
                currentCommandIndex++;
                
                if (currentCommandIndex >= commands.size()) {
                    isFinished = true;
                    return remaining_dt; // Finished whole sequence, return unused dt
                }
                
                if (leftover > 0.0 && leftover < current_dt_to_use) {
                    // Fractional stop, break so main.cpp updates physics exactly
                    return remaining_dt;
                }
            } else {
                // The current command is blocking and consumed all the remaining dt.
                return 0.0;
            }
        }
        return remaining_dt;
    }

    void Reset() {
        currentCommandIndex = 0;
        isFinished = false;
        for (auto& cmd : commands) {
            cmd->Reset();
        }
    }
};
