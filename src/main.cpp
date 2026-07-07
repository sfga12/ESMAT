#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"

// NASA SPICE toolkit
#include "SpiceUsr.h"

#include "imgui_internal.h"
#include "Shader.h"
#include "Sphere.h"
#include "Camera.h"
#include "CelestialBody.h"
#include "LineRenderer.h"
#include "OutputScript.h"
#include "Model.h"
#include "Spacecraft.h"
#include "ElectricSail.h"
#include "SolarWind.h"
#include "NavigationSystem.h"

#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <set>
#include <limits>
#include <cmath>
#include <chrono>

// APIENTRY macro is defined in glad.h, so including windows.h causes a conflict and warning (C4005).
// Therefore, we undefine it before including windows.h.
#ifdef APIENTRY
#undef APIENTRY
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // Native File Dialogs
#include <shlobj.h>  // Windows Shell for Folder Browsing
#include <direct.h>  // _mkdir

#include "stb_image.h"
#include <algorithm>

// Check if file exists
inline bool fileExists(const std::string& name) {
    std::ifstream f(name.c_str());
    return f.good();
}

// Get Executable Path helper
std::string GetExePath() {
    char result[MAX_PATH];
    return std::string(result, GetModuleFileNameA(NULL, result, MAX_PATH));
}
std::string GetExeDir() {
    std::string path = GetExePath();
    size_t pos = path.find_last_of("\\/");
    return (std::string::npos == pos) ? "" : path.substr(0, pos);
}

std::string ResolvePath(const std::string& relativePath) {
    if (relativePath.length() > 2 && relativePath[1] == ':') return relativePath; // already absolute
    
    std::string exeDir = GetExeDir();
    
    // Normalize slashes for consistency
    std::string rel = relativePath;
    std::replace(rel.begin(), rel.end(), '/', '\\');

    // Remove leading slash if any
    if (!rel.empty() && rel[0] == '\\') rel = rel.substr(1);

    // 1. Portable: Check directly relative to EXE (e.g. app/data)
    std::string path1 = exeDir + "\\" + rel;
    if (fileExists(path1)) return path1;
    
    // 2. Installer: Check parent directory (e.g. bin/ESMAT.exe -> app/data)
    std::string path2 = exeDir + "\\..\\" + rel;
    if (fileExists(path2)) return path2;
    
    // 3. Development: Check two levels up (e.g. build/Release/ESMAT.exe -> project/data)
    std::string path3 = exeDir + "\\..\\..\\" + rel;
    if (fileExists(path3)) return path3;

    return path3; // Fallback to dev path but don't promise it exists
}

void framebuffer_size_callback(GLFWwindow* window, int width, int height);
void mouse_callback(GLFWwindow* window, double xpos, double ypos);
void scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
void processInput(GLFWwindow *window);

// settings
unsigned int SCR_WIDTH = 1280;
unsigned int SCR_HEIGHT = 720;

// camera (default viewing J2000 XY plane from Z-axis)
Camera camera(glm::vec3(0.0f, 0.0f, 400.0f), glm::vec3(0.0f, 1.0f, 0.0f), -90.0f, 0.0f);
float lastX = SCR_WIDTH / 2.0f;
float lastY = SCR_HEIGHT / 2.0f;
bool firstMouse = true;
bool isMousePressed = false;

// timing
float deltaTime = 0.0f;	// time between current frame and last frame
float lastFrame = 0.0f;

// IDE Viewport & FBO tracking
float viewportWidth = 1280.0f;
float viewportHeight = 720.0f;
bool isSolarViewHovered = false;


// SPICE Simulation time
char startDate[32] = "2026-02-20T12:00:00";
char endDate[32] = "2027-02-20T12:00:00";
char utctime[32] = "2026-02-20T12:00:00";
SpiceDouble et_time = 0.0;
SpiceDouble start_et_time = 0.0;
SpiceDouble end_et_time = 0.0;
SpiceDouble globalSimEt = 0.0; // Global data accumulator
float timeSpeedMultiplier = 86400.0f; // 1 day per real-time second
float simulationStepSize = 86400.0f; // Data sample step size in seconds
bool isSimulating = false;
bool isSimulationInitialized = false;
bool isSimulationFinished = false;
bool isProjectLoaded = false;
bool showEsailParametersWindow = false;

// Project Definition
struct SimulationProject {
    std::string currentProjectPath = "";
    char configText[4096] = "START_DATE=2026-02-20T12:00:00\nEND_DATE=2027-02-20T12:00:00\nSTEP_SIZE_SEC=86400.0\n";
    float stepSizeSec    = 86400.0f;
};
SimulationProject g_Project;

std::vector<SolarWindField> projectSolarWinds;

std::vector<BurnEntry> g_BurnTable = {
    {TriggerType::GET,   0, 0, 0.0, 0, 0.0,   1, 399,  2.39947, 0.0, 0.0, 399, true, true}, 
    {TriggerType::APSIS, 0, 0, 0.0, 0, 0.0,   1, 399,  1.45722, 0.0, 0.0, 399, true, true}, 
};

bool isValidSpiceDate(const std::string& dateStr) {
    // Basic check for format: YYYY-MM-DDTHH:MM:SS (Length: 19)
    if (dateStr.length() != 19) return false;
    if (dateStr[4] != '-' || dateStr[7] != '-' || dateStr[10] != 'T' || dateStr[13] != ':' || dateStr[16] != ':') return false;
    for (int i = 0; i < 19; ++i) {
        if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16) continue;
        if (!isdigit(dateStr[i])) return false;
    }
    return true;
}


void ParseConfigText() {
    std::istringstream iss(g_Project.configText);
    std::string line;
    auto trim = [](std::string& s) {
        s.erase(0, s.find_first_not_of(" \n\r\t"));
        s.erase(s.find_last_not_of(" \n\r\t") + 1);
    };
    while (std::getline(iss, line)) {
        if (line.find("START_DATE=") == 0) {
            std::string val = line.substr(11);
            trim(val);
            strncpy(startDate, val.c_str(), sizeof(startDate)-1);
            startDate[sizeof(startDate)-1] = '\0';
        } else if (line.find("END_DATE=") == 0) {
            std::string val = line.substr(9);
            trim(val);
            strncpy(endDate, val.c_str(), sizeof(endDate)-1);
            endDate[sizeof(endDate)-1] = '\0';
        } else if (line.find("STEP_SIZE_SEC=") == 0) {
            try {
                std::string val = line.substr(14);
                trim(val);
                float step = std::stof(val);
                simulationStepSize = step;
                g_Project.stepSizeSec = step;
                // Fix: Sync visual speed with new step size so it always starts at 1x
                timeSpeedMultiplier = step;
            } catch (...) { }
        }
    }
}

// ==========================================
// IMPULSIVE BURN COMMAND IMPLEMENTATION
// ==========================================
void AddLog(const std::string& msg); // Forward declaration for Mission Commands

double ImpulsiveBurnCommand::Execute(Spacecraft* sc, double currentTime, double dt, const std::vector<CelestialBody>& planets) {
    if (hasExecuted) return dt; // Already executed, didn't use any of this dt.

    if (isVNBFrame) {
        // Calculate VNB Frame relative to central body (GMAT Convention)
        // Use the spacecraft's current position and velocity relative to its center body.
        // These are already the correct relative-state vectors for constructing the orbit plane.
        glm::dvec3 r_vec = sc->PositionKM;   // Position relative to center body
        glm::dvec3 v_vec = sc->VelocityKMS;  // Velocity relative to center body
        
        if (refBodyID != 0 && refBodyID != sc->centerBodySpiceID) {
            // Calculate state relative to the requested reference body
            const CelestialBody* refBody = nullptr;
            const CelestialBody* currentCenter = nullptr;
            for (const auto& p : planets) {
                if (p.SpiceID == refBodyID) refBody = &p;
                if (p.SpiceID == sc->centerBodySpiceID) currentCenter = &p;
            }
            if (refBody && currentCenter) {
                // Get absolute states (relative to Sun)
                glm::dvec3 scAbsolutePos = sc->PositionKM + currentCenter->PositionKM;
                glm::dvec3 scAbsoluteVel = sc->VelocityKMS + currentCenter->VelocityKMS;
                
                r_vec = scAbsolutePos - refBody->PositionKM;
                v_vec = scAbsoluteVel - refBody->VelocityKMS;
                AddLog("[Mission] Calculating VNB relative to body " + refBody->Name);
            } else if (currentCenter && refBodyID == 10) {
                 // Special case if reference body is Sun (10) but not found in planets list directly
                 r_vec = sc->PositionKM + currentCenter->PositionKM;
                 v_vec = sc->VelocityKMS + currentCenter->VelocityKMS;
            }
        }
        
        // V-axis: along velocity direction
        glm::dvec3 V = glm::normalize(v_vec);
        
        // N-axis (Orbit Normal): perpendicular to the orbital plane
        // For prograde orbits: N = normalize(r × v)
        glm::dvec3 N = glm::normalize(glm::cross(r_vec, v_vec)); // Out-of-plane
        
        // B-axis (Binormal): completes the right-hand coordinate system
        // B = V × N (co-plane with orbit but perpendicular to V)
        glm::dvec3 B = glm::cross(V, N); // Radial in-plane

        // Transform deltaV from local VNB to J2000 (Inertial)
        // deltaV_KMS.x = V (Velocity direction - prograde positive)
        // deltaV_KMS.y = B (Radial - in-plane normal)
        // deltaV_KMS.z = N (Out-of-plane normal)
        glm::dvec3 deltaV_Inertial = (deltaV_KMS.x * V) + 
                                     (deltaV_KMS.y * B) + 
                                     (deltaV_KMS.z * N);

        sc->VelocityKMS += deltaV_Inertial;
        AddLog("[Mission] Executed VNB Burn: [" + std::to_string(deltaV_KMS.x) + ", " + std::to_string(deltaV_KMS.y) + ", " + std::to_string(deltaV_KMS.z) + "] km/s");
        AddLog("[Mission] V=" + std::to_string(V.x) + "," + std::to_string(V.y) + "," + std::to_string(V.z));
        AddLog("[Mission] N=" + std::to_string(N.x) + "," + std::to_string(N.y) + "," + std::to_string(N.z));
        AddLog("[Mission] B=" + std::to_string(B.x) + "," + std::to_string(B.y) + "," + std::to_string(B.z));
        AddLog("[Mission] dV_inertial=" + std::to_string(deltaV_Inertial.x) + "," + std::to_string(deltaV_Inertial.y) + "," + std::to_string(deltaV_Inertial.z));
    } else {
        // J2000 Inertial Frame is just added directly
        sc->VelocityKMS += deltaV_KMS;
        AddLog("[Mission] Executed J2000 Burn: [" + std::to_string(deltaV_KMS.x) + ", " + std::to_string(deltaV_KMS.y) + ", " + std::to_string(deltaV_KMS.z) + "] km/s");
    }

    if (sc->trajectoryRenderer) {
        sc->trajectoryRenderer->markLastPointAsManeuver();
    }
    if (!sc->missionPath2D.empty()) {
        sc->missionPath2D.back().isManeuver = true;
    }

    hasExecuted = true;
    return dt; // Instantaneous, so always finishes immediately and uses 0 seconds of the dt.
}

// ==========================================
// PROPAGATE TO ALTITUDE COMMAND IMPLEMENTATION
// ==========================================
double PropagateToAltitudeCommand::Execute(Spacecraft* sc, double currentTime, double dt, const std::vector<CelestialBody>& planets) {
    const CelestialBody* target = nullptr;
    int actualTargetID = (targetSpiceID == 0) ? sc->centerBodySpiceID : targetSpiceID;
    for (const auto& p : planets) {
        if (p.SpiceID == actualTargetID) {
            target = &p;
            break;
        }
    }
    
    if (!target) {
        // Target not found, command finishes effectively with error but we just return dt to not block the simulation forever.
        return dt; 
    }

    // Spacecraft position is relative to its current CenterBody.
    // Target position is relative to Sun.
    // Let's get both into absolute pos (Sun relative)
    const CelestialBody* scCenter = nullptr;
    for (const auto& p : planets) {
        if (p.SpiceID == sc->centerBodySpiceID) {
            scCenter = &p;
            break;
        }
    }
    
    glm::dvec3 scAbsoluteKM = sc->PositionKM + (scCenter ? scCenter->PositionKM : glm::dvec3(0.0));
    glm::dvec3 targetAbsoluteKM = target->PositionKM;
    
    double distanceKM = glm::length(scAbsoluteKM - targetAbsoluteKM);
    double altitudeKM = distanceKM - target->RadiusKM;
    
    // Store lowest altitude for logging
    if (altitudeKM < minAltitudeReached) {
        minAltitudeReached = altitudeKM;
    } else if (altitudeKM > (minAltitudeReached + 50000.0) && minAltitudeReached < 300000.0 && !hasLoggedMiss) {
        // We are moving away significantly! Print closest approach once.
        AddLog("[Mission] Missed target. Closest approach altitude: " + std::to_string((long long)minAltitudeReached) + " km to " + target->Name);
        hasLoggedMiss = true; // Prevent spam
    }
    
    // Periodic diagnostic: log altitude every 1000 sub-steps so the user can see progress
    ++logStepCount;
    if (logStepCount % 1000 == 0) {
        AddLog("[AltWait] Target=" + target->Name + "  currentAlt=" + std::to_string((long long)altitudeKM) + 
               " km  triggerAlt<=" + std::to_string((long long)targetAltitudeKM) + " km  closest=" + std::to_string((long long)minAltitudeReached) + " km");
        printf("[AltWait] Target=%s  currentAlt=%lld km  triggerAlt<=%lld km  closest=%lld km\n",
               target->Name.c_str(), (long long)altitudeKM, (long long)targetAltitudeKM, (long long)minAltitudeReached);
        fflush(stdout);
    }

    const char* opNames[] = { "<", "<=", ">=", ">" };
    bool triggered = false;
    switch (condition) {
        case 0: triggered = altitudeKM < targetAltitudeKM; break;
        case 1: triggered = altitudeKM <= targetAltitudeKM; break;
        case 2: triggered = altitudeKM >= targetAltitudeKM; break;
        case 3: triggered = altitudeKM > targetAltitudeKM; break;
        default: break;
    }

    if (triggered) {
        // --- SCIENTIFIC STATE UPDATE (MICRO-STEPPING) ---
        // Estimate tau (fraction of dt) when crossing happened
        // Use the relative state to the TARGET body, not the center body!
        glm::dvec3 scAbsPos = sc->PositionKM + (scCenter ? scCenter->PositionKM : glm::dvec3(0.0));
        glm::dvec3 targetAbsPos = target->PositionKM;
        glm::dvec3 scAbsVel = sc->VelocityKMS + (scCenter ? scCenter->VelocityKMS : glm::dvec3(0.0));
        glm::dvec3 targetAbsVel = target->VelocityKMS;

        glm::dvec3 relPos = scAbsPos - targetAbsPos;
        glm::dvec3 relVel = scAbsVel - targetAbsVel;

        double currentAlt = glm::length(relPos) - target->RadiusKM;
        double radialVel = glm::dot(glm::normalize(relPos), relVel);
        
        double tau = 0.0;
        if (std::abs(radialVel) > 1e-6) {
            tau = (targetAltitudeKM - currentAlt) / (radialVel * dt);
            tau = std::max(0.0, std::min(1.0, tau)); // BUG FIX: Clamp tau to [0, 1] range
        }
        
        double consumed_dt = dt * tau;
        glm::dvec3 a = sc->computeAcceleration(sc->epochET, planets);
        sc->PositionKM += sc->VelocityKMS * consumed_dt + 0.5 * a * consumed_dt * consumed_dt;
        sc->VelocityKMS += a * consumed_dt;
        sc->epochET += consumed_dt;
        // sc->missionEpochET += consumed_dt; // BUG FIX: missionEpochET should stay as GET=0 reference

        std::string op = (condition < 4) ? opNames[condition] : "<=";
        AddLog("[Mission] Altitude condition met: " + target->Name + " altitude " + op + " " + std::to_string((long long)targetAltitudeKM) + " km at micro-step");
        return dt - consumed_dt;
    }

    return 0.0; // Condition not met, keep propagating and consume full dt
}

// ==========================================
// PROPAGATE TO APSIS COMMAND IMPLEMENTATION
// ==========================================
double PropagateToApsisCommand::Execute(Spacecraft* sc, double currentTime, double dt, const std::vector<CelestialBody>& planets) {
    if (!hasInitialized) hasInitialized = true;

    glm::dvec3 r = sc->PositionKM;
    glm::dvec3 v = sc->VelocityKMS;

    // SC state relative to refBodyID
    if (refBodyID != 0 && refBodyID != sc->centerBodySpiceID) {
        double state_c[6]; double lt;
        spkgeo_c(sc->centerBodySpiceID, sc->epochET, "J2000", refBodyID, state_c, &lt);
        r = sc->PositionKM + glm::dvec3(state_c[0], state_c[1], state_c[2]);
        v = sc->VelocityKMS + glm::dvec3(state_c[3], state_c[4], state_c[5]);
    }

    double r_mag = glm::length(r);
    if (r_mag < 1.0) return 0.0;
    double currentRadialVel = glm::dot(r, v) / r_mag;

    // Scientific Relative Acceleration Calculation (N-Body)
    // a_rel = a_sc - a_tgt = sum[ G*Mi * (r_i_sc/d_i_sc^3 - r_i_tgt/d_i_tgt^3) ]
    glm::dvec3 a_rel(0.0);
    
    // Position of Target Body relative to current Center Body
    glm::dvec3 r_tgt_rel_c;
    if (refBodyID != 0 && refBodyID != sc->centerBodySpiceID) {
        double st[6]; double lt;
        spkgeo_c(refBodyID, sc->epochET, "J2000", sc->centerBodySpiceID, st, &lt);
        r_tgt_rel_c = glm::dvec3(st[0], st[1], st[2]);
    } else {
        r_tgt_rel_c = glm::dvec3(0.0); // Target is center
    }

    // Absolute positions (Inertial J2000 as used in planets[i].PositionKM)
    const CelestialBody* cb = nullptr;
    for (auto& b : planets) if (b.SpiceID == sc->centerBodySpiceID) cb = &b;
    if (!cb) return 0.0;

    glm::dvec3 sc_abs = cb->PositionKM + sc->PositionKM;
    glm::dvec3 tgt_abs = cb->PositionKM + r_tgt_rel_c;

    for (const auto& body : planets) {
        double gm = body.GM;
        if (gm <= 0.0) continue;

        // Vector from body to SC
        glm::dvec3 r_i_sc = body.PositionKM - sc_abs;
        double d_i_sc = glm::length(r_i_sc);
        
        // Vector from body to TGT
        glm::dvec3 r_i_tgt = body.PositionKM - tgt_abs;
        double d_i_tgt = glm::length(r_i_tgt);

        if (d_i_sc > 1.0) a_rel += gm * r_i_sc / (d_i_sc * d_i_sc * d_i_sc);
        if (d_i_tgt > 1.0) a_rel -= gm * r_i_tgt / (d_i_tgt * d_i_tgt * d_i_tgt);
    }

    glm::dvec3 future_v = v + a_rel * dt;
    glm::dvec3 future_r = r + v * dt + 0.5 * a_rel * dt * dt;
    double future_r_mag = glm::length(future_r);
    double futureRadialVel = glm::dot(future_r, future_v) / future_r_mag;

    bool willCross = false;
    if (targetApoapsis && currentRadialVel > 1e-9 && futureRadialVel <= 0.0) willCross = true;
    if (!targetApoapsis && currentRadialVel < -1e-9 && futureRadialVel >= 0.0) willCross = true;

    if (willCross) {
        double changeDelta = std::abs(futureRadialVel - currentRadialVel);
        if (changeDelta < 1e-18) return dt; 
        double fractionToZero = std::abs(currentRadialVel) / changeDelta;
        double consumed_dt = dt * fractionToZero;

        // --- SCIENTIFIC STATE UPDATE (MICRO-STEPPING) ---
        // Move spacecraft to the exact crossing point before returning
        sc->PositionKM += sc->VelocityKMS * consumed_dt + 0.5 * sc->computeAcceleration(sc->epochET, planets) * consumed_dt * consumed_dt;
        sc->VelocityKMS += sc->computeAcceleration(sc->epochET, planets) * consumed_dt;
        sc->epochET += consumed_dt;
        // sc->missionEpochET += consumed_dt; // BUG FIX: missionEpochET should stay as GET=0 reference

        AddLog("[Mission] Reached " + std::string(targetApoapsis ? "Apoapsis" : "Periapsis") + " relative to " + std::to_string(refBodyID));
        return dt - consumed_dt; 
    }

    return 0.0; 
}

// Console Logs
std::vector<std::string> consoleLogs;
void AddLog(const std::string& msg) {
    consoleLogs.push_back(msg);
    if (consoleLogs.size() > 100) consoleLogs.erase(consoleLogs.begin());
}

// Global Output Script for simulation output
OutputScript g_Script;
std::string  g_ScriptText = OutputScript::defaultTemplate();

std::vector<std::shared_ptr<Spacecraft>> spacecrafts;
Spacecraft* selectedSpacecraft = nullptr;
std::string selectedSpacecraftName = "";

void checkSpiceErrors() {
    if (failed_c()) {
        char errMsg[1024];
        getmsg_c("LONG", 1024, errMsg);
        std::cerr << "SPICE ERROR: " << errMsg << std::endl;
        reset_c();
    }
}

// --- Native Windows Folder Dialogs ---

// Helper function for SHBrowseForFolder callback to set initial path
int CALLBACK BrowseCallbackProc(HWND hwnd, UINT uMsg, LPARAM lParam, LPARAM lpData) {
    if (uMsg == BFFM_INITIALIZED && lpData != 0) {
        SendMessage(hwnd, BFFM_SETSELECTION, TRUE, lpData);
    }
    return 0;
}

std::string OpenFolderDialog(const char* title) {
    std::string path = "";
    BROWSEINFOA bi = { 0 };
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (pidl != 0) {
        char szPath[MAX_PATH];
        if (SHGetPathFromIDListA(pidl, szPath)) {
            path = szPath;
        }
        IMalloc* imalloc = 0;
        if (SUCCEEDED(SHGetMalloc(&imalloc))) {
            imalloc->Free(pidl);
            imalloc->Release();
        }
    }
    return path;
}

std::string OpenFileDialog(const char* filter, const char* title) {
    OPENFILENAMEA ofn;
    char szFile[MAX_PATH] = { 0 };
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn) == TRUE) {
        return std::string(ofn.lpstrFile);
    }
    return "";
}

unsigned int LoadTexture(const char* path) {
    unsigned int textureID;
    glGenTextures(1, &textureID);
    
    std::string fullPath = ResolvePath(path);

    int width, height, nrComponents;
    unsigned char *data = stbi_load(fullPath.c_str(), &width, &height, &nrComponents, 0);
    if (data) {
        GLenum format;
        if (nrComponents == 1) format = GL_RED;
        else if (nrComponents == 3) format = GL_RGB;
        else if (nrComponents == 4) format = GL_RGBA;

        glBindTexture(GL_TEXTURE_2D, textureID);
        glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
        glGenerateMipmap(GL_TEXTURE_2D);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        // Better quality filtering for zooming in and out
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        stbi_image_free(data);
    } else {
        std::cout << "Texture failed to load at path: " << path << " Reason: " << stbi_failure_reason() << std::endl;
        stbi_image_free(data);
        return 0;
    }
    return textureID;
}

std::string ToUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::toupper(c); });
    return s;
}

// Helper to convert Mean Anomaly to True Anomaly (for Keplerian syncing)
double MeanToTrue(double M, double ecc) {
    if (ecc < 1.0) {
        double E = M;
        for (int i = 0; i < 15; i++) {
            double f = E - ecc * std::sin(E) - M;
            double df = 1.0 - ecc * std::cos(E);
            E -= f / df;
            if (std::abs(f) < 1e-11) break;
        }
        return 2.0 * std::atan2(std::sqrt(1.0 + ecc) * std::sin(E / 2.0), std::sqrt(1.0 - ecc) * std::cos(E / 2.0));
    } else {
        double F = M;
        for (int i = 0; i < 15; i++) {
            double f = ecc * std::sinh(F) - F - M;
            double df = ecc * std::cosh(F) - 1.0;
            F -= f / df;
            if (std::abs(f) < 1e-11) break;
        }
        return 2.0 * std::atan2(std::sqrt(ecc + 1.0) * std::sinh(F / 2.0), std::sqrt(ecc - 1.0) * std::cosh(F / 2.0));
    }
}

// Helper to convert True Anomaly to Mean Anomaly
double TrueToMean(double nu, double ecc) {
    if (ecc < 1.0) {
        double E = 2.0 * std::atan(std::sqrt((1.0 - ecc) / (1.0 + ecc)) * std::tan(nu / 2.0));
        return E - ecc * std::sin(E);
    } else {
        double F = 2.0 * std::atanh(std::sqrt((ecc - 1.0) / (ecc + 1.0)) * std::tan(nu / 2.0));
        return ecc * std::sinh(F) - F;
    }
}

void SyncKeplerianFromCartesian(const glm::dvec3& pos, const glm::dvec3& vel, double mu, double et, 
                                double& sma, double& ecc, double& inc, double& raan, double& argp, double& nu) {
    if (mu <= 0.0) return;
    SpiceDouble state[6] = { pos.x, pos.y, pos.z, vel.x, vel.y, vel.z };
    SpiceDouble elts[8];
    oscelt_c(state, et, mu, elts);
    
    double rp = elts[0];
    ecc = elts[1];
    inc = elts[2] * 180.0 / 3.14159265358979;
    raan = elts[3] * 180.0 / 3.14159265358979;
    argp = elts[4] * 180.0 / 3.14159265358979;
    double m0 = elts[5];
    
    if (ecc < 1.0) {
        sma = rp / (1.0 - ecc);
    } else {
        sma = -rp / (ecc - 1.0); 
    }
    
    nu = MeanToTrue(m0, ecc) * 180.0 / 3.14159265358979;
}

void SyncCartesianFromKeplerian(double sma, double ecc, double inc, double raan, double argp, double nu, double mu, double et,
                                glm::dvec3& pos, glm::dvec3& vel) {
    if (mu <= 0.0) return;
    SpiceDouble elts[8];
    double rp = (ecc < 1.0) ? (sma * (1.0 - ecc)) : (std::abs(sma) * (ecc - 1.0));
    elts[0] = rp;
    elts[1] = ecc;
    elts[2] = inc * 3.14159265358979 / 180.0;
    elts[3] = raan * 3.14159265358979 / 180.0;
    elts[4] = argp * 3.14159265358979 / 180.0;
    elts[5] = TrueToMean(nu * 3.14159265358979 / 180.0, ecc);
    elts[6] = et;
    elts[7] = mu;
    
    SpiceDouble state[6];
    conics_c(elts, et, state);
    pos = glm::dvec3(state[0], state[1], state[2]);
    vel = glm::dvec3(state[3], state[4], state[5]);
}

// --- Project Save & Load Logic ---
bool DirectoryExists(const std::string& path) {
    DWORD ftyp = GetFileAttributesA(path.c_str());
    if (ftyp == INVALID_FILE_ATTRIBUTES) return false;
    return (ftyp & FILE_ATTRIBUTE_DIRECTORY);
}

struct ParsedSpiceBody {
    int id;
    std::string name;
    double radius;
    double j2;
    bool selected;
    int parentId;
};

bool autoFocusDone = false;
int g_pendingFocusID = -1; // SpiceID of body to auto-focus; -1 = none pending

void LoadMissionPlanFromCSV(const std::string& filepath, const std::vector<CelestialBody>& planets) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cout << "[Mission] Failed to open CSV: " << filepath << std::endl;
        return;
    }
    
    g_BurnTable.clear();
    std::string line;
    // Skip header
    std::getline(file, line);
    
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        
        std::string stepStr, triggerStr, paramsStr, dvxStr, dvyStr, dvzStr, totalDvStr, refBodyStr, frameStr;
        
        std::getline(ss, stepStr, ',');
        std::getline(ss, triggerStr, ',');
        std::getline(ss, paramsStr, ',');
        std::getline(ss, dvxStr, ',');
        std::getline(ss, dvyStr, ',');
        std::getline(ss, dvzStr, ',');
        std::getline(ss, totalDvStr, ',');
        std::getline(ss, refBodyStr, ',');
        std::getline(ss, frameStr, ',');
        
        BurnEntry entry = {};
        entry.enabled = true;
        
        // Trigger
        if (triggerStr == "Time (GET)") {
            entry.trigger = TriggerType::GET;
            std::replace(paramsStr.begin(), paramsStr.end(), ':', ' ');
            std::stringstream pss(paramsStr);
            pss >> entry.get_h >> entry.get_m >> entry.get_s;
        } else if (triggerStr == "Orbital Event") {
            entry.trigger = TriggerType::APSIS;
            if (paramsStr == "Apoapsis") entry.apsisType = 0;
            else entry.apsisType = 1; // Periapsis
        } else if (triggerStr == "Altitude") {
            entry.trigger = TriggerType::ALTITUDE;
            size_t lePos = paramsStr.find("<=");
            size_t gePos = paramsStr.find(">=");
            size_t lPos = paramsStr.find("<");
            size_t gPos = paramsStr.find(">");
            std::string bodyName = "";
            std::string valStr = "";
            if (lePos != std::string::npos) {
                entry.altCondition = 1;
                bodyName = paramsStr.substr(0, lePos);
                valStr = paramsStr.substr(lePos + 2);
            } else if (gePos != std::string::npos) {
                entry.altCondition = 2;
                bodyName = paramsStr.substr(0, gePos);
                valStr = paramsStr.substr(gePos + 2);
            } else if (lPos != std::string::npos) {
                entry.altCondition = 0;
                bodyName = paramsStr.substr(0, lPos);
                valStr = paramsStr.substr(lPos + 1);
            } else if (gPos != std::string::npos) {
                entry.altCondition = 3;
                bodyName = paramsStr.substr(0, gPos);
                valStr = paramsStr.substr(gPos + 1);
            }
            
            if (!bodyName.empty()) {
                bodyName.erase(bodyName.find_last_not_of(" \n\r\t")+1);
                bodyName.erase(0, bodyName.find_first_not_of(" \n\r\t"));
            }
            if (!valStr.empty()) {
                size_t kmPos = valStr.find("km");
                if (kmPos != std::string::npos) valStr.erase(kmPos);
                valStr.erase(valStr.find_last_not_of(" \n\r\t")+1);
                valStr.erase(0, valStr.find_first_not_of(" \n\r\t"));
                try { entry.targetAltKM = std::stod(valStr); } catch(...) { entry.targetAltKM = 0; }
            }
            
            bodyName = ToUpper(bodyName);
            if (bodyName == "SSB" || bodyName == "CENTER") {
                entry.altRefBodyID = 0;
            } else {
                entry.altRefBodyID = 0;
                for (const auto& p : planets) {
                    if (ToUpper(p.Name) == bodyName) { entry.altRefBodyID = p.SpiceID; break; }
                }
            }
        }
        
        try { entry.dvx = std::stod(dvxStr); } catch(...) { entry.dvx = 0; }
        try { entry.dvy = std::stod(dvyStr); } catch(...) { entry.dvy = 0; }
        try { entry.dvz = std::stod(dvzStr); } catch(...) { entry.dvz = 0; }
        
        refBodyStr.erase(refBodyStr.find_last_not_of(" \n\r\t")+1);
        refBodyStr.erase(0, refBodyStr.find_first_not_of(" \n\r\t"));
        refBodyStr = ToUpper(refBodyStr);
        if (refBodyStr == "SSB" || refBodyStr == "CENTER") {
            entry.refBodyID = 0;
        } else {
            entry.refBodyID = 0;
            for (const auto& p : planets) {
                if (ToUpper(p.Name) == refBodyStr) { entry.refBodyID = p.SpiceID; break; }
            }
        }
        
        frameStr.erase(frameStr.find_last_not_of(" \n\r\t")+1);
        frameStr.erase(0, frameStr.find_first_not_of(" \n\r\t"));
        if (frameStr == "VNB") entry.isVNB = true;
        else entry.isVNB = false;
        
        g_BurnTable.push_back(entry);
    }
    std::cout << "[Mission] Loaded " << g_BurnTable.size() << " steps from CSV." << std::endl;
}

void SaveProjectToFolder(const std::string& folderPath, const std::vector<CelestialBody>& currentPlanets = std::vector<CelestialBody>(), 
                         const std::string& spkSource = "", const std::string& pckSource = "", const std::string& lskSource = "", 
                         const std::string& gmPckSource = "", 
                         const std::vector<ParsedSpiceBody>& bodies = std::vector<ParsedSpiceBody>()) {
    if (!DirectoryExists(folderPath)) {
        _mkdir(folderPath.c_str());
    }
    
    // Extract base folder name from path
    std::string baseName = folderPath;
    size_t lastSlash = baseName.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        baseName = baseName.substr(lastSlash + 1);
    }

    // 1. Save Project Definition File (.esmatproject)
    std::string projectFile = folderPath + "\\" + baseName + ".esmatproject";
    std::ofstream pf(projectFile);
    if (pf.is_open()) {
        pf << "ESMAT_PROJECT_V1\n";
        pf.close();
    }

    // 2. Save SimulationSettings.cfg
    std::string cfgFile = folderPath + "\\SimulationSettings.cfg";
    std::ofstream file(cfgFile);
    if (!file.is_open()) {
        AddLog("[ERROR] Could not save config to: " + cfgFile);
        return;
    }
    file << g_Project.configText;
    file.close();

    // 2b. Save OutputScript.esmat
    std::ofstream scriptF(folderPath + "\\OutputScript.esmat");
    if (scriptF.is_open()) { scriptF << g_ScriptText; scriptF.close(); }

    // 3. Create SolarSystem Directory
    std::string solarDir = folderPath + "\\SolarSystem";
    if (!DirectoryExists(solarDir)) {
        _mkdir(solarDir.c_str());
    }
    
    // 4. Copy SPICE Kernels if provided
    if (!spkSource.empty() && !pckSource.empty() && !lskSource.empty()) {
        std::string datasDir = folderPath + "\\datas";
        if (!DirectoryExists(datasDir)) {
            _mkdir(datasDir.c_str());
        }
        
        std::string spkName = spkSource.substr(spkSource.find_last_of("\\/") + 1);
        std::string pckName = pckSource.substr(pckSource.find_last_of("\\/") + 1);
        std::string lskName = lskSource.substr(lskSource.find_last_of("\\/") + 1);
        
        std::string destSpk = datasDir + "\\" + spkName;
        std::string destPck = datasDir + "\\" + pckName;
        std::string destLsk = datasDir + "\\" + lskName;
        
        CopyFileA(spkSource.c_str(), destSpk.c_str(), FALSE);
        CopyFileA(pckSource.c_str(), destPck.c_str(), FALSE);
        CopyFileA(lskSource.c_str(), destLsk.c_str(), FALSE);
        
        // Save kernels.txt: spk, pck, lsk, and optionally gmPck
        std::ofstream kmd(datasDir + "\\kernels.txt");
        if (kmd.is_open()) {
            kmd << spkName << "\n" << pckName << "\n" << lskName << "\n";
            if (!gmPckSource.empty()) {
                std::string gmPckName = gmPckSource.substr(gmPckSource.find_last_of("\\/") + 1);
                std::string destGmPck = datasDir + "\\" + gmPckName;
                CopyFileA(gmPckSource.c_str(), destGmPck.c_str(), FALSE);
                kmd << gmPckName << "\n";
            } else {
                kmd << "\n"; // Placeholder for GM PCK
            }

            // Always copy and include built-in geophysical kernel for J2, etc.
            std::string sourceGeo = ResolvePath("data\\kernels\\solar_system_geophysics.ker");
            if (fileExists(sourceGeo)) {
                std::string destGeo = datasDir + "\\solar_system_geophysics.ker";
                CopyFileA(sourceGeo.c_str(), destGeo.c_str(), FALSE);
                kmd << "solar_system_geophysics.ker\n";
            }
            kmd.close();
        }
    }

    // 5. Generate .esmatobj for newly selected bodies or save existing bodies
    if (!bodies.empty()) {
        for (const auto& b : bodies) {
            if (b.selected) {
                // Generate random default color for new body
                srand(b.id * 12345);
                float r = ((float)(rand() % 100)) / 100.0f * 0.5f + 0.5f;
                float g = ((float)(rand() % 100)) / 100.0f * 0.5f + 0.5f;
                float b_col = ((float)(rand() % 100)) / 100.0f * 0.5f + 0.5f;

                std::string objPath = solarDir + "\\" + b.name + ".esmatobj";
                std::ofstream objF(objPath);
                if (objF.is_open()) {
                    objF << "BODY_NAME=" << b.name << "\n";
                    objF << "SPICE_ID=" << b.id << "\n";
                    objF << "PARENT_ID=" << b.parentId << "\n";
                    objF << "RADIUS_KM=" << b.radius << "\n";
                    objF << "COLOR=" << r << "," << g << "," << b_col << "\n";
                    objF << "SHOW_BODY=TRUE\n";
                    objF << "SHOW_ORBIT=TRUE\n";

                    // Default textures for newly generated bodies
                    std::string texFile = "";
                    if (b.id == 1 || b.id == 199) texFile = "MercuryTexture.jpg";
                    else if (b.id == 2 || b.id == 299) texFile = "VenusTexture.jpg";
                    else if (b.id == 3 || b.id == 399) texFile = "EarthTexture.jpg";
                    else if (b.id == 301) texFile = "MoonTexture.jpg";
                    else if (b.id == 4 || b.id == 499) texFile = "MarsTexture.jpg";
                    else if (b.id == 5 || b.id == 599) texFile = "JupiterTexture.jpg";
                    else if (b.id == 6 || b.id == 699) texFile = "SaturnTexture.jpg";
                    else if (b.id == 7 || b.id == 799) texFile = "UranusTexture.jpg";
                    else if (b.id == 8 || b.id == 899) texFile = "NeptuneTexture.jpg";

                    if (!texFile.empty()) {
                        objF << "TEXTURE_PATH=" << ResolvePath("data\\textures\\" + texFile) << "\n";
                    }

                    objF << "J2=" << b.j2 << "\n"; // ParsedSpiceBody should also have j2 or we query it here

                    objF << "READ_ONLY=TRUE\n";
                    objF << "SPICE_DATA_LINKED=TRUE\n";
                    objF.close();
                }
            }
        }
    } else if (!currentPlanets.empty()) {
        for (const auto& p : currentPlanets) {
            std::string objPath = solarDir + "\\" + p.Name + ".esmatobj";
            std::ofstream objF(objPath);
            if (objF.is_open()) {
                objF << "BODY_NAME=" << p.Name << "\n";
                objF << "SPICE_ID=" << p.SpiceID << "\n";
                objF << "PARENT_ID=" << p.ParentID << "\n";
                objF << "RADIUS_KM=" << p.RadiusKM << "\n";
                objF << "COLOR=" << p.Color.r << "," << p.Color.g << "," << p.Color.b << "\n";
                objF << "SHOW_BODY=" << (p.showBody ? "TRUE" : "FALSE") << "\n";
                objF << "SHOW_ORBIT=" << (p.showOrbit ? "TRUE" : "FALSE") << "\n";
                if (p.HasTexture && !p.TexturePath.empty()) {
                    objF << "TEXTURE_PATH=" << p.TexturePath << "\n";
                }
                objF << "J2=" << p.J2 << "\n";
                objF << "READ_ONLY=TRUE\n";
                objF << "SPICE_DATA_LINKED=TRUE\n";
                objF.close();
            }
        }
    }

    // 6. Save Solar Wind Fields (.esmatwind)
    {
        std::string windDir = folderPath + "\\SolarWinds";
        if (!DirectoryExists(windDir)) _mkdir(windDir.c_str());
        for (const auto& sw : projectSolarWinds) {
            std::ofstream wf(windDir + "\\" + sw.Name + ".esmatwind");
            if (wf.is_open()) {
                wf << "name=" << sw.Name << "\n";
                wf << "vx=" << sw.velocityKMS.x << "\n";
                wf << "vy=" << sw.velocityKMS.y << "\n";
                wf << "vz=" << sw.velocityKMS.z << "\n";
                wf << "density=" << sw.density << "\n";
                wf << "temp_ev=" << sw.temperatureEV << "\n";
                // 4D grid referansi: tam yolu kaydet
                if (sw.isDataLoaded && sw.grid.nt > 0) {
                    // Grid dosya yolu sw.Name + ".swgrid4d" olarak ayni klasore kaydedilir
                    wf << "grid4d_path=" << sw.Name << ".swgrid4d" << "\n";
                }
                wf.close();
            }
        }
    }

    g_Project.currentProjectPath = folderPath;
    AddLog("Project saved to folder: " + folderPath);
}

bool LoadProjectFromFolder(const std::string& folderPath) {
    // Check if it's a valid project
    std::string baseName = folderPath;
    size_t lastSlash = baseName.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        baseName = baseName.substr(lastSlash + 1);
    }
    
    std::string projectFile = folderPath + "\\" + baseName + ".esmatproject";
    // Check for specifically named config, or fallback to an older 'project.esmatproject'
    if (!fileExists(projectFile)) {
        std::string fallbackFile = folderPath + "\\project.esmatproject";
        if (!fileExists(fallbackFile)) {
            AddLog("[ERROR] Not a valid ESMAT Project folder: " + folderPath);
            return false;
        }
    }

    std::string cfgPath = folderPath + "\\SimulationSettings.cfg";
    std::ifstream file(cfgPath);
    if (!file.is_open()) {
        AddLog("[ERROR] Could not load config from: " + cfgPath);
        return false;
    }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    strncpy(g_Project.configText, content.c_str(), sizeof(g_Project.configText) - 1);
    g_Project.configText[sizeof(g_Project.configText) - 1] = '\0';
    file.close();
    
    g_Project.currentProjectPath = folderPath;
    isSimulationInitialized = false;
    isSimulating = false;
    isSimulationFinished = false;
    ParseConfigText();

    // Clear and Reload SPICE Kernels specifically for this project
    kclear_c();
    
    std::string kernelsFile = folderPath + "\\datas\\kernels.txt";
    if (fileExists(kernelsFile)) {
        std::ifstream kf(kernelsFile);
        std::string spkName, pckName, lskName;
        
        // Trim whitespace and \r from names (crucial for Windows line endings)
        auto trim = [](std::string& s) {
            s.erase(0, s.find_first_not_of(" \n\r\t"));
            s.erase(s.find_last_not_of(" \n\r\t") + 1);
        };

        // First 3 lines are always SPK, PCK, LSK from initial creation
        if (std::getline(kf, spkName) && std::getline(kf, pckName) && std::getline(kf, lskName)) {
            trim(spkName); trim(pckName); trim(lskName);

            std::string spkPath = folderPath + "\\datas\\" + spkName;
            std::string pckPath = folderPath + "\\datas\\" + pckName;
            std::string lskPath = folderPath + "\\datas\\" + lskName;

            if (fileExists(lskPath)) furnsh_c(lskPath.c_str());
            else AddLog("[WARN] LSK kernel missing: " + lskName);

            if (fileExists(spkPath)) furnsh_c(spkPath.c_str());
            else AddLog("[WARN] SPK kernel missing: " + spkName);

            if (fileExists(pckPath)) furnsh_c(pckPath.c_str());
            else AddLog("[WARN] PCK kernel missing: " + pckName);

            std::string logMsg = "Loaded Kernels: " + spkName + " | " + pckName + " | " + lskName;
            
            // Read remaining lines dynamically (could be GM PCK, Built-in Geophysics, or User Uploaded SPKs)
            std::string extraKernelName;
            while (std::getline(kf, extraKernelName)) {
                trim(extraKernelName);
                if (!extraKernelName.empty()) {
                    std::string extraPath = folderPath + "\\datas\\" + extraKernelName;
                    if (fileExists(extraPath)) {
                        furnsh_c(extraPath.c_str());
                        logMsg += " | " + extraKernelName;
                    } else {
                        AddLog("[WARN] Kernel missing from datas directory: " + extraKernelName);
                    }
                }
            }
            AddLog(logMsg);
        }
        
        // NOW initialize et_time — MUST be after LSK is loaded so str2et_c works
        SpiceInt nKernels = 0;
        ktotal_c("ALL", &nKernels);

        if (nKernels > 0 && isValidSpiceDate(startDate)) {
            str2et_c(startDate, &start_et_time);
            if (!failed_c()) {
                et_time = start_et_time;
                globalSimEt = start_et_time;
            } else {
                reset_c();
                AddLog("[WARN] Could not convert startDate to ET (LSK missing?), et_time = 0.");
            }
        } else {
            AddLog("[WARN] No kernels loaded or invalid date. et_time and sim start at 0.");
            et_time = 0;
            globalSimEt = 0;
        }
    } else {
        // Fallback to default
        furnsh_c(ResolvePath("data\\kernels\\naif0012.tls").c_str());
        furnsh_c(ResolvePath("data\\kernels\\de432s.bsp").c_str());
        
        // Auto-load any PCK (.tpc) found in the default folder
        std::string kernelDir = ResolvePath("data\\kernels") + "\\*.tpc";
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(kernelDir.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    std::string pckPath = ResolvePath("data\\kernels\\" + std::string(fd.cFileName));
                    furnsh_c(pckPath.c_str());
                    AddLog("Loaded Fallback PCK: " + std::string(fd.cFileName));
                }
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }

        AddLog("Loaded Default SPICE Kernels.");
    }
    
    AddLog("Project loaded from folder: " + folderPath);
    autoFocusDone = false; // Allow auto-focus to run once for this project

    // Load OutputScript.esmat if present, else use default template
    std::string scriptPath = folderPath + "\\OutputScript.esmat";
    if (fileExists(scriptPath)) {
        std::ifstream sf(scriptPath);
        if (sf.is_open()) {
            g_ScriptText = std::string((std::istreambuf_iterator<char>(sf)),
                                        std::istreambuf_iterator<char>());
            sf.close();
        }
    } else {
        g_ScriptText = OutputScript::defaultTemplate();
        // Write the default template
        std::ofstream sf(scriptPath);
        if (sf.is_open()) { sf << g_ScriptText; }
    }
    // Reset script state for new project
    g_Script.plots.clear();
    g_Script.reports.clear();

    // Load Spacecrafts
    spacecrafts.clear();
    std::string scSearchPath = folderPath + "\\Spacecrafts\\*.esmatspacecraft";
    WIN32_FIND_DATAA fdSC;
    HANDLE hFindSC = FindFirstFileA(scSearchPath.c_str(), &fdSC);
    if (hFindSC != INVALID_HANDLE_VALUE) {
        do {
            if (!(fdSC.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                std::string scFile = folderPath + "\\Spacecrafts\\" + std::string(fdSC.cFileName);
                std::ifstream scf(scFile);
                if (scf.is_open()) {
                    std::string line;
                    std::string scName = "Unknown";
                    int scID = -1;
                    int scCenter = 399; // Default Earth
                    glm::dvec3 pos(0.0), vel(0.0);
                    std::string scEpoch = "";
                    std::string scMissionEpoch = "";
                    bool isLinked = false;
                    int linkedId = -1;
                    
                    bool p_hasEsail = false;
                    double p_esailLen = 20.0, p_esailVolt = 25.0, p_esailMass = 1000.0, p_esailAccel = 0.0, p_esailRA = 0.0, p_esailDEC = 0.0, p_esailDeflection = 10.0;
                    int p_esailCnt = 50;
                    std::string p_esailWindName = "";
                    while (std::getline(scf, line)) {
                        size_t eq = line.find("=");
                        if (eq != std::string::npos) {
                            std::string key = line.substr(0, eq);
                            std::string val = line.substr(eq + 1);
                            if (key == "name") scName = val;
                            else if (key == "id") scID = std::stoi(val);
                            else if (key == "center") scCenter = std::stoi(val);
                            else if (key == "pos") {
                                sscanf(val.c_str(), "%lf,%lf,%lf", &pos.x, &pos.y, &pos.z);
                            }
                            else if (key == "vel") {
                                sscanf(val.c_str(), "%lf,%lf,%lf", &vel.x, &vel.y, &vel.z);
                            }
                            else if (key == "epoch") scEpoch = val;
                            else if (key == "mission_epoch") scMissionEpoch = val;
                            else if (key == "is_spice_linked") isLinked = (std::stoi(val) == 1);
                            else if (key == "linked_spice_id") linkedId = std::stoi(val);
                            else if (key == "has_esail") p_hasEsail = (std::stoi(val) == 1);
                            else if (key == "esail_len") p_esailLen = std::stod(val);
                            else if (key == "esail_cnt") p_esailCnt = std::stoi(val);
                            else if (key == "esail_vol") p_esailVolt = std::stod(val);
                            else if (key == "esail_mass") p_esailMass = std::stod(val);
                            else if (key == "esail_ra") p_esailRA = std::stod(val);
                            else if (key == "esail_dec") p_esailDEC = std::stod(val);
                            else if (key == "esail_deflection") p_esailDeflection = std::stod(val);
                            else if (key == "esail_wind_name") p_esailWindName = val;
                            
                            // Group A: Spin
                            else if (key == "esail_spin_rate") {
                                if (p_hasEsail) {
                                    // We'll update the object after creation
                                }
                            }
                        }
                    }
                    scf.close();
                    
                    std::shared_ptr<Spacecraft> newSc;
                    if (p_hasEsail) {
                        auto es = std::make_shared<ElectricSail>(scName, glm::vec3(0.0f, 1.0f, 1.0f));
                        
                        es->esailLengthKM = p_esailLen;
                        es->esailTetherCount = p_esailCnt;
                        es->esailVoltageKV = p_esailVolt;
                        es->esailMassKG = p_esailMass;
                        es->esailSpinRA = glm::radians(p_esailRA);
                        es->esailSpinDEC = glm::radians(p_esailDEC);
                        es->esailDeflectionAngleDeg = p_esailDeflection;
                        es->attachedWindName = p_esailWindName;
                        
                        // Re-parse file to get all advanced parameters for the instantiated object
                        scf.clear();
                        scf.seekg(0, std::ios::beg);
                        while (std::getline(scf, line)) {
                            size_t eq = line.find("=");
                            if (eq != std::string::npos) {
                                std::string key = line.substr(0, eq);
                                std::string val = line.substr(eq + 1);
                                if (key == "esail_spin_rate") es->spinRateRPM = std::stod(val);




                            }
                        }
                        
                        es->updateCharAccel();
                        newSc = es;
                    } else {
                        newSc = std::make_shared<Spacecraft>(scName, glm::vec3(0.0f, 1.0f, 1.0f));
                    }

                    newSc->ID = scID;
                    newSc->centerBodySpiceID = scCenter;
                    newSc->initialCenterBodySpiceID = scCenter;
                    newSc->InitialPositionKM = pos;
                    newSc->InitialVelocityKMS = vel;
                    newSc->PositionKM = pos;
                    newSc->VelocityKMS = vel;
                    newSc->isSpiceLinked = isLinked;
                    newSc->linkedSpiceID = linkedId;
                    
                    // Parse Epoch
                    if (!scEpoch.empty()) {
                        SpiceDouble parsed_et = 0.0;
                        size_t modPos = scEpoch.find(" ::");
                        if (modPos != std::string::npos) scEpoch = scEpoch.substr(0, modPos);
                        if (scEpoch.find("UTC") == std::string::npos && scEpoch.find("TDB") == std::string::npos) {
                            scEpoch += " UTC";
                        }
                        str2et_c(scEpoch.c_str(), &parsed_et);
                        if (!failed_c()) {
                            newSc->epochET = parsed_et;
                        } else {
                            reset_c();
                            newSc->epochET = 0.0;
                        }
                    } else {
                        newSc->epochET = 0.0;
                    }
                    
                    // Parse Mission Epoch
                    if (!scMissionEpoch.empty()) {
                        SpiceDouble m_et = 0.0;
                        size_t modPos2 = scMissionEpoch.find(" ::");
                        if (modPos2 != std::string::npos) scMissionEpoch = scMissionEpoch.substr(0, modPos2);
                        if (scMissionEpoch.find("UTC") == std::string::npos && scMissionEpoch.find("TDB") == std::string::npos) {
                            scMissionEpoch += " UTC";
                        }
                        str2et_c(scMissionEpoch.c_str(), &m_et);
                        if (!failed_c()) {
                            newSc->missionEpochET = m_et;
                        } else {
                            reset_c();
                            newSc->missionEpochET = newSc->epochET;
                        }
                    } else {
                        newSc->missionEpochET = newSc->epochET;
                    }
                    
                    std::string modelPath = ResolvePath("data\\models\\testsatmodel.glb");
                    if (fileExists(modelPath)) {
                        newSc->model = std::make_shared<ModelGLTF>(modelPath);
                        if (!newSc->model->loaded) AddLog("[WARN] Spacecraft model failed to load.");
                    }
                    spacecrafts.push_back(newSc);
                    AddLog("Loaded Spacecraft: " + scName);
                }
            }
        } while (FindNextFileA(hFindSC, &fdSC));
        FindClose(hFindSC);
    }

    // Load Solar Wind Fields
    projectSolarWinds.clear();
    {
        std::string windSearch = folderPath + "\\SolarWinds\\*.esmatwind";
        WIN32_FIND_DATAA fdW;
        HANDLE hFindW = FindFirstFileA(windSearch.c_str(), &fdW);
        if (hFindW != INVALID_HANDLE_VALUE) {
            do {
                if (!(fdW.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    std::string wPath = folderPath + "\\SolarWinds\\" + std::string(fdW.cFileName);
                    std::ifstream wf(wPath);
                    if (wf.is_open()) {
                        SolarWindField sw;
                        std::string wline;
                        std::string gridPath4D;
                        while (std::getline(wf, wline)) {
                            size_t eq = wline.find('=');
                            if (eq == std::string::npos) continue;
                            std::string k = wline.substr(0, eq);
                            std::string v = wline.substr(eq + 1);
                            if (k == "name")         sw.Name             = v;
                            else if (k == "vx")      sw.velocityKMS.x   = std::stod(v);
                            else if (k == "vy")      sw.velocityKMS.y   = std::stod(v);
                            else if (k == "vz")      sw.velocityKMS.z   = std::stod(v);
                            else if (k == "density") sw.density          = std::stod(v);
                            else if (k == "temp_ev") sw.temperatureEV    = std::stod(v);
                            else if (k == "grid4d_path") gridPath4D      = v;
                        }
                        // 4D grid varsa yukle
                        if (!gridPath4D.empty()) {
                            std::string absGrid = gridPath4D;
                            // Goreceli yol ise proje klasorune gore coz
                            if (absGrid.find(':') == std::string::npos)
                                absGrid = folderPath + "\\" + absGrid;
                            if (sw.load4DGrid(absGrid))
                                AddLog("[OK] ENLIL 4D grid loaded: " + sw.Name
                                       + "  (" + sw.infoString() + ")");
                            else
                                AddLog("[WARNING] 4D grid could not be loaded: " + absGrid);
                        }
                        projectSolarWinds.push_back(sw);
                        AddLog("Loaded Solar Wind: " + sw.Name);
                    }
                }
            } while (FindNextFileA(hFindW, &fdW));
            FindClose(hFindW);
        }
    }

    return true;
}

bool resetLayoutRequested = true;
// autoFocusDone is declared above SaveProjectToFolder


// Modal State variables
static bool openNewProjectModal = false;
static char newProjectName[128] = "MySimulation";
static char newProjectLocation[MAX_PATH] = "";
static char newProjectSpkPath[MAX_PATH] = "";
static char newProjectPckPath[MAX_PATH] = "";
static char newProjectLskPath[MAX_PATH] = "";
static char newProjectGmPckPath[MAX_PATH] = ""; // Optional: GM PCK (e.g. gm_de431.tpc)
static std::vector<ParsedSpiceBody> parsedBodies;
static std::string parsedCoverageStart = "";
static std::string parsedCoverageEnd = "";

// Spacecraft Modal Variables
static char newSpacecraftName[128] = "MySpacecraft";
static char newSpacecraftEpoch[128] = "2000-Jan-01 12:00:00 UTC";
static char newSpacecraftMissionEpoch[128] = "2000-Jan-01 12:00:00 UTC"; // GET=0 point
static int selectedCenterBodyIdx = 0;
glm::dvec3 newSpacecraftPos(0.0);
glm::dvec3 newSpacecraftVel(0.0);
static bool useSpkForSpacecraft = false;
static char scSpkPath[MAX_PATH] = "";
static std::vector<ParsedSpiceBody> scParsedBodies;
static int selectedScSpkBodyIdx = 0;
static int selectedSolarWindIdx = 0;


// Upload Kernel Modal State variables
static bool openUploadKernelModal = false;
static char uploadKernelSpkPath[MAX_PATH] = "";
static std::vector<ParsedSpiceBody> uploadParsedBodies;

#define MAXIV  1000
#define WINSIZ ( 2 * MAXIV )

void ParseSpkForUpload() {
    uploadParsedBodies.clear();
    std::string spkPath = uploadKernelSpkPath;
    if (spkPath.empty()) return;

    // Load only the SPK temporarily (LSK and PCK are assumed already loaded by the active project)
    furnsh_c(spkPath.c_str());

    SPICEINT_CELL(ids, MAXIV);
    spkobj_c(spkPath.c_str(), &ids);

    for (int i = 0; i < card_c(&ids); i++) {
        int id = ((SpiceInt*)ids.data)[i];
        
        // 1. Get Name
        char name[32];
        SpiceBoolean found;
        bodc2n_c(id, 32, name, &found);
        std::string bodyName = found ? name : "Unknown_" + std::to_string(id);

        // 2. Get Radius
        SpiceInt dim;
        SpiceDouble radii[3];
        bodvrd_c(bodyName.c_str(), "RADII", 3, &dim, radii);
        double r = 0.0;
        if (!failed_c()) {
            r = radii[0]; // Equatorial radius
        } else {
            reset_c(); // Clear error
            
            // Fallback: If it's a Barycenter (ID < 10), try to get the radius of its primary planet
            if (id > 0 && id < 10) {
                int planetId = id * 100 + 99;
                char planetName[32];
                SpiceBoolean pFound;
                bodc2n_c(planetId, 32, planetName, &pFound);
                if (pFound) {
                    bodvrd_c(planetName, "RADII", 3, &dim, radii);
                    if (!failed_c()) r = radii[0];
                    else reset_c();
                }
            }
        }

        // 3. Get J2
        SpiceInt n_j2;
        SpiceDouble j2_val = 0.0;
        bodvcd_c(id, "J2", 1, &n_j2, &j2_val);
        if (failed_c()) reset_c();

        ParsedSpiceBody b;
        b.id = id;
        b.name = bodyName;
        b.radius = r;
        b.j2 = j2_val;
        b.selected = true; // Default selected
        if (id == 301) b.parentId = 399; // Moon defaults to Earth
        else if (id > 100 && id < 1000 && (id % 100 != 99)) b.parentId = (id / 100) * 100 + 99; // Moons default to planets
        else b.parentId = 10; // Everything else defaults to Sun
        uploadParsedBodies.push_back(b);
    }
    unload_c(spkPath.c_str());
}

void ParseSpkForSpacecraft() {
    scParsedBodies.clear();
    std::string spkPath = scSpkPath;
    if (spkPath.empty()) return;

    // Load only the SPK temporarily (LSK and PCK are assumed already loaded by the active project)
    furnsh_c(spkPath.c_str());

    SPICEINT_CELL(ids, MAXIV);
    spkobj_c(spkPath.c_str(), &ids);

    for (int i = 0; i < card_c(&ids); i++) {
        int id = ((SpiceInt*)ids.data)[i];
        
        // 1. Get Name
        char name[32];
        SpiceBoolean found;
        bodc2n_c(id, 32, name, &found);
        std::string bodyName = found ? name : "Unknown_" + std::to_string(id);

        ParsedSpiceBody b;
        b.id = id;
        b.name = bodyName;
        scParsedBodies.push_back(b);
    }
    unload_c(spkPath.c_str());
    selectedScSpkBodyIdx = 0;
}

void ParseSpiceFilesForNewProject() {
    parsedBodies.clear();
    parsedCoverageStart = "";
    parsedCoverageEnd = "";

    std::string spkPath = newProjectSpkPath;
    std::string pckPath = newProjectPckPath;
    std::string lskPath = newProjectLskPath;
    std::string gmPckPath = newProjectGmPckPath;

    if (spkPath.empty() || pckPath.empty() || lskPath.empty()) return;

    // Load kernels temporarily for parsing
    furnsh_c(lskPath.c_str());
    furnsh_c(spkPath.c_str());
    furnsh_c(pckPath.c_str());
    if (!gmPckPath.empty()) {
        furnsh_c(gmPckPath.c_str());
    }
    
    // Always load the built-in geophysical kernel during parsing to pick up constants
    std::string builtinGeo = ResolvePath("data\\kernels\\solar_system_geophysics.ker");
    if (fileExists(builtinGeo)) {
        furnsh_c(builtinGeo.c_str());
    }

    SPICEINT_CELL(ids, MAXIV);
    spkobj_c(spkPath.c_str(), &ids);
    
    // Get coverage of the very first object to represent the file coverage roughly
    if (card_c(&ids) > 0) {
        SPICEDOUBLE_CELL(cover, WINSIZ);
        spkcov_c(spkPath.c_str(), ((SpiceInt*)ids.data)[0], &cover);
        if (card_c(&cover) > 0) {
            SpiceDouble b, e;
            wnfetd_c(&cover, 0, &b, &e);
            char bStr[64], eStr[64];
            timout_c(b, "YYYY-MM-DDTHH:MM:SS", 64, bStr);
            timout_c(e, "YYYY-MM-DDTHH:MM:SS", 64, eStr);
            if (!failed_c()) {
                parsedCoverageStart = bStr;
                parsedCoverageEnd = eStr;
            } else {
                reset_c();
            }
        }
    }

    for (int i = 0; i < card_c(&ids); i++) {
        int id = ((SpiceInt*)ids.data)[i];
        
        // 1. Get Name
        char name[32];
        SpiceBoolean found;
        bodc2n_c(id, 32, name, &found);
        std::string bodyName = found ? name : "Unknown_" + std::to_string(id);

        // 2. Get Radius
        SpiceInt dim;
        SpiceDouble radii[3];
        bodvrd_c(bodyName.c_str(), "RADII", 3, &dim, radii);
        double r = 0.0;
        if (!failed_c()) {
            r = radii[0]; // Equatorial radius
        } else {
            reset_c(); // Clear error
            
            // Fallback: If it's a Barycenter (ID < 10), try to get the radius of its primary planet
            // Planet ID = Barycenter ID * 100 + 99  (e.g., 3 -> 399 Earth, 4 -> 499 Mars)
            if (id > 0 && id < 10) {
                int planetId = id * 100 + 99;
                char planetName[32];
                SpiceBoolean pFound;
                bodc2n_c(planetId, 32, planetName, &pFound);
                if (pFound) {
                    bodvrd_c(planetName, "RADII", 3, &dim, radii);
                    if (!failed_c()) {
                        r = radii[0];
                    } else {
                        reset_c();
                    }
                }
            }
        }

        // 3. Get J2
        SpiceInt n_j2;
        SpiceDouble j2_val = 0.0;
        bodvcd_c(id, "J2", 1, &n_j2, &j2_val);
        if (failed_c()) {
            reset_c();
        }

        ParsedSpiceBody b;
        b.id = id;
        b.name = bodyName;
        b.radius = r;
        b.j2 = j2_val;
        b.selected = true; // Default selected
        if (id == 301) b.parentId = 399; // Moon defaults to Earth
        else if (id > 100 && id < 1000 && (id % 100 != 99)) b.parentId = (id / 100) * 100 + 99; // Moons default to planets
        else b.parentId = 10; // Everything else defaults to Sun
        parsedBodies.push_back(b);
    }
    unload_c(lskPath.c_str());
    unload_c(spkPath.c_str());
    unload_c(pckPath.c_str());
}

// Helper to compute orbital period using SPICE conics
double GetOrbitalPeriod(int bodyID, int parentID, double et) {
    if (bodyID == 10 || parentID == 0) return 0.0; // Sun or invalid parent
    
    SpiceDouble state[6], lt;
    SpiceDouble gmArr[1];
    SpiceInt n;
    
    // 1. Get GM of parent by integer ID
    bodvcd_c(parentID, "GM", 1, &n, gmArr);
    if (failed_c()) {
        reset_c();
        AddLog("[DBG] bodvcd_c(GM) failed for parentID=" + std::to_string(parentID) + ", trying bodvrd_c by name...");

        // Try by string name
        char pName[64];
        SpiceBoolean pFound;
        bodc2n_c(parentID, 64, pName, &pFound);
        
        if (pFound) {
            AddLog("[DBG] Parent name resolved to: " + std::string(pName));
            bodvrd_c(pName, "GM", 1, &n, gmArr);
            if (!failed_c()) goto gm_found;
            reset_c();
            AddLog("[DBG] bodvrd_c(GM) also failed for: " + std::string(pName));
        } else {
            AddLog("[DBG] bodc2n_c could not resolve parentID=" + std::to_string(parentID));
        }

        // Try barycenter (e.g. 399 -> 3)
        if (parentID > 100 && parentID < 1000) {
            int baryID = parentID / 100;
            AddLog("[DBG] Trying barycenter GM, ID=" + std::to_string(baryID));
            bodvcd_c(baryID, "GM", 1, &n, gmArr);
            if (!failed_c()) goto gm_found;
            reset_c();
        }
        
        AddLog("[SPICE] GM not found for parent " + std::to_string(parentID) + ". (Is PCK loaded?) Using 1-year fallback.");
        return 31557600.0; 
    }

gm_found:
    double gm = gmArr[0];
    AddLog("[DBG] GM for parent " + std::to_string(parentID) + " = " + std::to_string(gm) + ", et=" + std::to_string(et));

    // 2. Get state relative to parent
    spkezr_c(std::to_string(bodyID).c_str(), et, "J2000", "NONE", std::to_string(parentID).c_str(), state, &lt);
    if (failed_c()) {
        char errMsg[1024];
        getmsg_c("SHORT", 1024, errMsg);
        AddLog("[SPICE] spkezr_c failed for body=" + std::to_string(bodyID) + " parent=" + std::to_string(parentID) + " et=" + std::to_string(et) + " err: " + std::string(errMsg));
        reset_c();
        return 31557600.0;
    }

    // 3. Get orbital elements
    SpiceDouble elts[8];
    oscelt_c(state, et, gm, elts);
    if (failed_c()) {
        reset_c();
        AddLog("[DBG] oscelt_c failed for body=" + std::to_string(bodyID));
        return 31557600.0;
    }

    double e = elts[1];
    double rp = elts[0];
    
    if (e >= 1.0) return -1.0; 

    double a = rp / (1.0 - e);
    const double PI = 3.14159265358979323846;
    double period = 2.0 * PI * sqrt(pow(a, 3.0) / gm);
    
    return period;
}

void GenerateOrbitForPlanet(CelestialBody& planet, SpiceDouble et_time, double scaleFactor) {
    if (planet.SpiceID != 10 && planet.ParentID != 0) {
        if (!planet.orbitRenderer) planet.orbitRenderer = new LineRenderer();
        planet.orbitRenderer->updatePoints(std::vector<TrailPoint>());
    }
}

void SyncPlanetsWithFileSystem(std::vector<CelestialBody>& planets, const std::string& projectFolder, std::string& selectedBodyName, std::string& openedFile, SpiceDouble et_time, double scaleFactor) {
    if (projectFolder.empty()) return;
    
    std::string searchPath = projectFolder + "\\SolarSystem\\*.esmatobj";
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath.c_str(), &fd);
    
    std::set<std::string> foundFiles;
    
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                std::string fname = fd.cFileName;
                size_t extPos = fname.find(".esmatobj");
                if (extPos != std::string::npos) {
                    foundFiles.insert(fname.substr(0, extPos));
                }
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }
    
    // 1. Remove planets not matching any file
    for (auto it = planets.begin(); it != planets.end(); ) {
        if (foundFiles.find(it->Name) == foundFiles.end()) {
            if (selectedBodyName == it->Name) {
                selectedBodyName = "";
                openedFile = "SimulationSettings.cfg";
            }
            if (it->orbitRenderer) {
                delete it->orbitRenderer;
            }
            it = planets.erase(it);
        } else {
            ++it;
        }
    }
    
    // 2. Add planets found in files but missing from memory
    for (const std::string& bName : foundFiles) {
        bool exists = false;
        for (const auto& p : planets) {
            if (p.Name == bName) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            std::string objPath = projectFolder + "\\SolarSystem\\" + bName + ".esmatobj";
            std::ifstream objF(objPath);
            if (objF.is_open()) {
                std::string line;
                int spiceID = -1;
                int parentID = 10;
                float radiusKM = 1000.0f; // fallback
                double j2_val = 0.0;
                glm::vec3 color(-1.0f);
                bool showBody = true;
                bool showOrbit = true;
                
                    std::string texturePathRead = "";
                    auto trim = [](std::string& s) {
                        s.erase(0, s.find_first_not_of(" \n\r\t"));
                        s.erase(s.find_last_not_of(" \n\r\t") + 1);
                    };
                    
                    while (std::getline(objF, line)) {
                        if (line.find("SPICE_ID=") == 0) {
                            std::string val = line.substr(9); trim(val);
                            spiceID = std::stoi(val);
                        }
                        else if (line.find("PARENT_ID=") == 0) {
                            std::string val = line.substr(10); trim(val);
                            parentID = std::stoi(val);
                        }
                        else if (line.find("RADIUS_KM=") == 0) {
                            std::string val = line.substr(10); trim(val);
                            radiusKM = std::stof(val);
                        }
                        else if (line.find("COLOR=") == 0) {
                            float r, g, b;
                            if (sscanf(line.c_str() + 6, "%f,%f,%f", &r, &g, &b) == 3) {
                                color = glm::vec3(r, g, b);
                            }
                        }
                        else if (line.find("SHOW_BODY=") == 0) {
                            std::string val = line.substr(10); trim(val);
                            showBody = (val == "TRUE");
                        }
                        else if (line.find("SHOW_ORBIT=") == 0) {
                            std::string val = line.substr(11); trim(val);
                            showOrbit = (val == "TRUE");
                        }
                        else if (line.find("TEXTURE_PATH=") == 0) {
                            texturePathRead = line.substr(13);
                            trim(texturePathRead);
                        }
                        else if (line.find("J2=") == 0) {
                            std::string val = line.substr(3); trim(val);
                            j2_val = std::stod(val);
                        }
                    }
                    objF.close();
                    
                    if (spiceID != -1) {
                        // Generate a seeded random color based on SPICE ID
                        if (color.x < 0.0f) {
                            srand(spiceID * 12345);
                            float r = ((float)(rand() % 100)) / 100.0f * 0.5f + 0.5f;
                            float g = ((float)(rand() % 100)) / 100.0f * 0.5f + 0.5f;
                            float b = ((float)(rand() % 100)) / 100.0f * 0.5f + 0.5f;
                            color = glm::vec3(r, g, b);
                        }
                        
                        CelestialBody newBody(bName, spiceID, radiusKM, color, parentID);
                        newBody.J2 = j2_val;

                        // Query GM from project kernels
                        {
                            SpiceInt n_gm;
                            SpiceDouble gm_tmp = 0.0;
                            bodvcd_c(spiceID, "GM", 1, &n_gm, &gm_tmp);
                            if (!failed_c()) {
                                newBody.GM = gm_tmp;
                            } else {
                                reset_c();
                                // Fallback for common bodies if GM kernels are missing
                                if (spiceID == 10) newBody.GM = 132712440018.0; // Sun
                                else if (spiceID == 399) newBody.GM = 398600.435436; // Earth
                                else if (spiceID == 301) newBody.GM = 4902.800066; // Moon
                                else newBody.GM = 0.0;
                                
                                AddLog("[SPICE] GM kernel not found for " + bName + ". Using fallback.");
                            }
                        }

                        // Fallback: If J2 is 0, try to query from currently loaded project kernels
                        if (newBody.J2 == 0.0) {
                            SpiceInt n_j2;
                            SpiceDouble j2_tmp = 0.0;
                            int queryID = (spiceID > 0 && spiceID < 10) ? (spiceID * 100 + 99) : spiceID;
                            bodvcd_c(queryID, "J2", 1, &n_j2, &j2_tmp);
                            if (!failed_c()) {
                                newBody.J2 = j2_tmp;
                            } else {
                                reset_c();
                            }
                        }

                        newBody.showBody = showBody;
                        newBody.showOrbit = showOrbit;

                        // Load texture if path was read from file
                        if (!texturePathRead.empty() && fileExists(texturePathRead)) {
                            // Find the data\\textures part to pass as a relative path to LoadTexture
                            // LoadTexture uses ResolvePath which expects something relative to project root (up 2 from build/Release)
                            // But here we have an absolute path. Let's try to load it directly if possible or extract relative part.
                            // Actually LoadTexture calls ResolvePath. If we pass absolute path to ResolvePath, it should return it as is.
                            newBody.TextureID = LoadTexture(texturePathRead.c_str());
                            if (newBody.TextureID > 0) {
                                newBody.HasTexture = true;
                                newBody.TexturePath = texturePathRead;
                                newBody.Color = glm::vec3(1.0f);
                            }
                        }
                    
                    // Fetch dynamic orbital period from SPICE
                    newBody.orbitPeriodSeconds = GetOrbitalPeriod(spiceID, parentID, et_time);
                    
                    // Auto-assign texture if available
                    std::string texFile = "";
                    if (spiceID == 1 || spiceID == 199) texFile = "MercuryTexture.jpg";
                    else if (spiceID == 2 || spiceID == 299) texFile = "VenusTexture.jpg";
                    else if (spiceID == 3 || spiceID == 399) texFile = "EarthTexture.jpg";
                    else if (spiceID == 301) texFile = "MoonTexture.jpg";
                    else if (spiceID == 4 || spiceID == 499) texFile = "MarsTexture.jpg";
                    else if (spiceID == 5 || spiceID == 599) texFile = "JupiterTexture.jpg";
                    else if (spiceID == 6 || spiceID == 699) texFile = "SaturnTexture.jpg";
                    else if (spiceID == 7 || spiceID == 799) texFile = "UranusTexture.jpg";
                    else if (spiceID == 8 || spiceID == 899) texFile = "NeptuneTexture.jpg";

                    if (!texFile.empty()) {
                        std::string relPath = "data\\textures\\" + texFile;
                        std::string absPath = ResolvePath(relPath);
                        if (fileExists(absPath)) {
                            newBody.TextureID = LoadTexture(relPath.c_str());
                            if (newBody.TextureID > 0) {
                                newBody.HasTexture = true;
                                newBody.TexturePath = absPath;
                                newBody.Color = glm::vec3(1.0f);
                            }
                        }
                    }

                    GenerateOrbitForPlanet(newBody, et_time, scaleFactor);
                    planets.push_back(newBody);
                }
            }
        }
    }
}

int main()
{
    // glfw: initialize and configure
    if (!glfwInit()) return -1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(SCR_WIDTH, SCR_HEIGHT, "ESMAT Electric Sail Mission Analysis Tool", NULL, NULL);
    if (!window) { glfwTerminate(); return -1; }
    glfwMakeContextCurrent(window);
    
    // Set Window Icon
    GLFWimage images[1];
    images[0].pixels = stbi_load(ResolvePath("data\\esmat-icon-2.png").c_str(), &images[0].width, &images[0].height, 0, 4); // req_comp = 4 (RGBA)
    if (images[0].pixels) {
        glfwSetWindowIcon(window, 1, images);
        stbi_image_free(images[0].pixels);
    }
    
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetScrollCallback(window, scroll_callback);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return -1;
    glEnable(GL_DEPTH_TEST);
    
    // Enable Alpha Blending for transparent planets
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Initializing ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImPlot::GetStyle().UseISO8601    = true;
    ImPlot::GetStyle().Use24HourClock = true;
    ImPlot::GetStyle().UseLocalTime  = false;
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable; 
    
    if (!fileExists("imgui.ini")) {
        resetLayoutRequested = true;
    } else {
        resetLayoutRequested = false;
    }

    // Load Professional Font for Scientific UI + Emoji Support
    ImFontConfig fontConfig;
    fontConfig.MergeMode = false;
    ImFont* mainFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.0f, &fontConfig, io.Fonts->GetGlyphRangesDefault());
    
    // Merge Emoji Font
    static const ImWchar emojiRanges[] = { 0x1F300, 0x1FAD6, 0 }; // Space/Planet and Folder ranges
    fontConfig.MergeMode = true;
    fontConfig.OversampleH = 1;
    fontConfig.OversampleV = 1;
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguiemj.ttf", 16.0f, &fontConfig, emojiRanges);

    // Modern Light IDE Theme
    ImGui::StyleColorsLight();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    
    // Tweak Light Theme colors to look more like a professional IDE (e.g. VS/Eclipse)
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.95f, 0.95f, 0.95f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.98f, 0.98f, 0.98f, 1.0f);
    style.Colors[ImGuiCol_PopupBg] = ImVec4(1.00f, 1.00f, 1.00f, 0.98f);
    
    style.Colors[ImGuiCol_Text] = ImVec4(0.10f, 0.10f, 0.10f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.50f, 1.0f);

    style.Colors[ImGuiCol_FrameBg] = ImVec4(1.00f, 1.00f, 1.00f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.90f, 0.94f, 0.97f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.85f, 0.90f, 0.95f, 1.0f);

    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.90f, 0.90f, 0.90f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.90f, 0.90f, 0.90f, 1.0f);

    style.Colors[ImGuiCol_Header] = ImVec4(0.85f, 0.90f, 0.95f, 1.0f); 
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.80f, 0.88f, 0.96f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.70f, 0.82f, 0.94f, 1.0f);

    style.Colors[ImGuiCol_Button] = ImVec4(0.88f, 0.88f, 0.88f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.80f, 0.88f, 0.96f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.70f, 0.82f, 0.94f, 1.0f);
    
    // Deep blue text for specific emphasized headers
    ImVec4 accentColor = ImVec4(0.0f, 0.3f, 0.7f, 1.0f);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Shaders
    Shader sphereShader(ResolvePath("data\\shaders\\sphere.vs").c_str(), ResolvePath("data\\shaders\\sphere.fs").c_str());
    Shader lineShader(ResolvePath("data\\shaders\\line.vs").c_str(), ResolvePath("data\\shaders\\line.fs").c_str());

    // Global Sphere Mesh for all planets
    Sphere sphere(1.0f, 36, 18);
    unsigned int sphereVAO, sphereVBO, sphereEBO;
    glGenVertexArrays(1, &sphereVAO);
    glGenBuffers(1, &sphereVBO);
    glGenBuffers(1, &sphereEBO);
    glBindVertexArray(sphereVAO);
    glBindBuffer(GL_ARRAY_BUFFER, sphereVBO);
    glBufferData(GL_ARRAY_BUFFER, sphere.vertices.size() * sizeof(Vertex), &sphere.vertices[0], GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sphereEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sphere.indices.size() * sizeof(unsigned int), &sphere.indices[0], GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Normal));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, TexCoords));

    // --- Earth Reference Lines: Equator & Prime Meridian ---
    // Generate circle points in body-fixed frame (unit radius, scaled at draw time)
    auto generateCirclePoints = [](int segments, glm::vec3 axis1, glm::vec3 axis2) -> std::vector<glm::vec3> {
        std::vector<glm::vec3> pts;
        pts.reserve(segments + 1);
        for (int i = 0; i <= segments; ++i) {
            float angle = (float)i / (float)segments * 2.0f * 3.14159265358979f;
            pts.push_back(cosf(angle) * axis1 + sinf(angle) * axis2);
        }
        return pts;
    };
    const int CIRCLE_SEGMENTS = 128;
    // Equator: lies in XY-plane of body frame (Z=0 ring)
    std::vector<glm::vec3> equatorPts    = generateCirclePoints(CIRCLE_SEGMENTS, glm::vec3(1,0,0), glm::vec3(0,1,0));
    // Prime Meridian: lies in XZ-plane of body frame (Y=0 ring)
    std::vector<glm::vec3> meridianPts   = generateCirclePoints(CIRCLE_SEGMENTS, glm::vec3(1,0,0), glm::vec3(0,0,1));


    unsigned int earthLineVAO[2], earthLineVBO[2];
    glGenVertexArrays(2, earthLineVAO);
    glGenBuffers(2, earthLineVBO);
    // Equator
    glBindVertexArray(earthLineVAO[0]);
    glBindBuffer(GL_ARRAY_BUFFER, earthLineVBO[0]);
    glBufferData(GL_ARRAY_BUFFER, equatorPts.size() * sizeof(glm::vec3), equatorPts.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)0);
    glEnableVertexAttribArray(0);
    // Prime Meridian
    glBindVertexArray(earthLineVAO[1]);
    glBindBuffer(GL_ARRAY_BUFFER, earthLineVBO[1]);
    glBufferData(GL_ARRAY_BUFFER, meridianPts.size() * sizeof(glm::vec3), meridianPts.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    // Framebuffer for IDE Window Rendering
    unsigned int fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    unsigned int textureColorBuffer;
    glGenTextures(1, &textureColorBuffer);
    glBindTexture(GL_TEXTURE_2D, textureColorBuffer);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, SCR_WIDTH, SCR_HEIGHT, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textureColorBuffer, 0);

    unsigned int rbo;
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, SCR_WIDTH, SCR_HEIGHT);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        std::cout << "ERROR::FRAMEBUFFER:: Framebuffer is not complete!" << std::endl;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // SPICE Data Loading initial setup
    errprt_c("SET", 0, "NONE");
    erract_c("SET", 0, "RETURN");

    std::string defaultLsk = ResolvePath("data\\kernels\\naif0012.tls");
    std::string defaultSpk = ResolvePath("data\\kernels\\de432s.bsp");

    bool lskLoaded = false;
    if (fileExists(defaultLsk)) {
        furnsh_c(defaultLsk.c_str());
        lskLoaded = !failed_c();
        if (!lskLoaded) reset_c();
    }
    if (fileExists(defaultSpk)) {
        furnsh_c(defaultSpk.c_str());
        if (failed_c()) reset_c();
    }
    
    checkSpiceErrors();

    // Convert starting time safely
    if (lskLoaded) {
        str2et_c(utctime, &et_time);
        str2et_c(startDate, &start_et_time);
        str2et_c(endDate, &end_et_time);
        if (failed_c()) reset_c();
    } else {
        et_time = 0;
        start_et_time = 0;
        end_et_time = 0;
    }

    // Planets Setup — starts empty, populated from project files via SyncPlanetsWithFileSystem
    std::vector<CelestialBody> planets;

    // Scale factor to map KM to OpenGL coordinates
    double scaleFactor = 1.0 / 1000000.0; // 1 million km = 1 unit

    std::string selectedBodyName = "";
    std::string openedFile = "SimulationSettings.cfg";

    float syncTimer = 0.0f;

    unsigned int startScreenIcon = LoadTexture("data\\esmat-icon-3.png");
    unsigned int githubIcon = LoadTexture("data\\icons\\github.png");
    unsigned int webIcon = LoadTexture("data\\icons\\web.png");
    unsigned int youtubeIcon = LoadTexture("data\\icons\\youtube.png");

    while (!glfwWindowShouldClose(window))
    {
        float currentFrame = glfwGetTime();
        deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;

        // Visual smoothing & Lag protection: Cap deltaTime used for simulation logic
        // This prevents the physics engine from trying to "catch up" with huge leaps if the window is moved, 
        // resumed after a long pause, or when starting for the first time.
        static bool wasSimulating = false;
        if (isSimulating && !wasSimulating) {
            // When simulation starts/unpauses, reset timers to avoid the initial leap
            lastFrame = glfwGetTime();
            deltaTime = 0.0f;
        }
        wasSimulating = isSimulating;

        float simDeltaTime = (deltaTime > 0.1f) ? 0.1f : deltaTime; 

        if (isProjectLoaded) {
            syncTimer += deltaTime;
            if (syncTimer >= 1.0f) {
                SyncPlanetsWithFileSystem(planets, g_Project.currentProjectPath, selectedBodyName, openedFile, et_time, scaleFactor);
                syncTimer = 0.0f;
                
                // Auto-focus: on first sync after project load, set flag to trigger camera focus in render section
                if (!autoFocusDone && !planets.empty()) {
                    autoFocusDone = true; // Mark done; actual camera set below where currentOriginBody is in scope
                    // Find Earth or default to first body and set selectedBodyName
                    CelestialBody* focusTarget = nullptr;
                    for (auto& p : planets) { if (p.SpiceID == 399) { focusTarget = &p; break; } }
                    if (!focusTarget) focusTarget = &planets[0];
                    selectedBodyName = focusTarget->Name;
                    openedFile = focusTarget->Name + ".esmatobj";
                    // Signal the render section to apply focus (pendingFocusID is static in the render section)
                    // We use a global so both sections share the same variable
                    extern int g_pendingFocusID;
                    g_pendingFocusID = focusTarget->SpiceID;
                }
            }
            
            // Period Recalculation: If some bodies have the fallback value, try to recalculate them occasionally
            // (especially useful if kernels were loaded after the body was first synced)
            static float periodRetryTimer = 0.0f;
            periodRetryTimer += deltaTime;
            if (periodRetryTimer >= 2.0f) {
                for (auto& planet : planets) {
                    if (planet.orbitPeriodSeconds == 31557600.0 && planet.SpiceID != 10) {
                        planet.orbitPeriodSeconds = GetOrbitalPeriod(planet.SpiceID, planet.ParentID, et_time);
                    }
                }
                periodRetryTimer = 0.0f;
            }
            }

        checkSpiceErrors(); // Proactively clear and log SPICE errors

        processInput(window);

        // Simulation Update
        if (isSimulating) {
            et_time += timeSpeedMultiplier * simDeltaTime;
        }

        // Compute SPICE positions for Visual Rendering synchronized with Fixed Step Size
        if (isSimulating) {
            // Safety net: limit steps per frame to avoid freezing while prioritizing maximum simulation speed 
            auto frameStartTime = std::chrono::high_resolution_clock::now();
            
            // globalSimEt advances in physical steps until it catches up to visual et_time
            while ((timeSpeedMultiplier > 0 && globalSimEt < et_time) || 
                   (timeSpeedMultiplier < 0 && globalSimEt > et_time)) 
            {
                auto now = std::chrono::high_resolution_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - frameStartTime).count() > 15) {
                    et_time = globalSimEt; // Cap visual speed to maximum CPU physics limit (maintains ~60 FPS)
                    break;
                }
                
                double direction = (timeSpeedMultiplier > 0) ? 1.0 : -1.0;
                globalSimEt += simulationStepSize * direction;
                
                // --- Console Progress Logger ---
                static int maxModeLogCounter = 0;
                maxModeLogCounter++;
                if (maxModeLogCounter % 10000 == 0) {
                    char dateStr[128];
                    timout_c(globalSimEt, "YYYY-MON-DD HR:MN:SC ::UTC", 128, dateStr);
                    std::cout << "[SIM PROGRESS] Date: " << dateStr << " | Steps Computed: " << maxModeLogCounter << " \r" << std::flush;
                }
                // -------------------------------
                
                // E-Sail parameters are now static, no per-step evaluation needed
                
                for (auto& planet : planets) {
                    SpiceDouble state[6];
                    SpiceDouble lt;
                        glm::vec3 stepPos(0.0f);
                        bool posFetched = false;
                        
                        // 1. Get position of planet relative to Sun (ID 10) in J2000 frame
                        if (planet.SpiceID != 10) {
                            spkezr_c(std::to_string(planet.SpiceID).c_str(), globalSimEt, "J2000", "NONE", "10", state, &lt);
                            if (!failed_c()) {
                                planet.PositionKM = glm::dvec3(state[0], state[1], state[2]);
                                planet.VelocityKMS = glm::dvec3(state[3], state[4], state[5]);
                            } else {
                                reset_c(); // Some planets might not be in the kernel
                            }
                            
                            // Query parent relative to Sun to shift the orbit ring correctly
                            if (planet.ParentID != 10 && planet.ParentID != 0) {
                                spkezr_c(std::to_string(planet.ParentID).c_str(), globalSimEt, "J2000", "NONE", "10", state, &lt);
                                if (!failed_c()) {
                                    planet.ParentPositionKM = glm::dvec3(state[0], state[1], state[2]);
                                } else {
                                    reset_c();
                                    planet.ParentPositionKM = glm::dvec3(0.0);
                                }
                                
                                // Fetch the true relative state between the planet and its chosen parent
                                SpiceDouble relState[6];
                                spkezr_c(std::to_string(planet.SpiceID).c_str(), globalSimEt, "J2000", "NONE", std::to_string(planet.ParentID).c_str(), relState, &lt);
                                if (!failed_c()) {
                                    planet.RelativePositionKM = glm::dvec3(relState[0], relState[1], relState[2]);
                                    planet.RelativeVelocityKMS = glm::dvec3(relState[3], relState[4], relState[5]);
                                    stepPos = planet.RelativePositionKM;
                                    posFetched = true;
                                } else {
                                    reset_c();
                                    planet.RelativePositionKM = planet.PositionKM;
                                    planet.RelativeVelocityKMS = planet.VelocityKMS;
                                }
                            } else {
                                planet.ParentPositionKM = glm::dvec3(0.0);
                                planet.RelativePositionKM = planet.PositionKM;
                                planet.RelativeVelocityKMS = planet.VelocityKMS;
                                stepPos = planet.RelativePositionKM;
                                posFetched = true;
                            }
                        } else {
                            planet.PositionKM = glm::dvec3(0.0);
                            planet.ParentPositionKM = glm::dvec3(0.0);
                            planet.RelativePositionKM = glm::dvec3(0.0);
                            planet.RelativeVelocityKMS = glm::dvec3(0.0);
                        }
                        
                        // 2. Trail Accumulator Logic (Bidirectional)
                if (posFetched && planet.orbitRenderer) {
                    if (direction > 0) {
                        if (planet.orbitRenderer->pointsKM.empty()) {
                            planet.orbitRenderer->addPoint(globalSimEt, stepPos, planet.ParentID);
                        } else {
                            double lastEt = planet.orbitRenderer->pointsKM.back().et;
                            double timeJump = std::abs(globalSimEt - lastEt);
                            
                            // SMOOTHNESS GUARANTEE: Draw an orbit with at least 100 points (1% of period).
                            // planet.orbitPeriodSeconds value comes dynamically from SPICE.
                            double maxAllowedJump = planet.orbitPeriodSeconds * 0.01; 
                            if (maxAllowedJump < 3600.0) maxAllowedJump = 3600.0; // Precision limit: Never calculate sub-steps more frequently than 1 hour
                            
                            // If simulation step is too large for planet smoothness, fill the gap with SPICE
                            if (timeJump > maxAllowedJump) {
                                int fillCount = static_cast<int>(timeJump / maxAllowedJump);
                                
                                // Safety limit to prevent freezing on huge jumps
                                if (fillCount > 200) fillCount = 200; 
                                
                                double stepFill = (globalSimEt - lastEt) / (fillCount + 1);
                                
                                for (int i = 1; i <= fillCount; ++i) {
                                    double intermediateEt = lastEt + stepFill * i;
                                    
                                    SpiceDouble tempState[6], tempLt;
                                    // Extracting intermediate positions relative to parent from SPICE
                                    spkezr_c(std::to_string(planet.SpiceID).c_str(), intermediateEt, "J2000", "NONE", std::to_string(planet.ParentID).c_str(), tempState, &tempLt);
                                    
                                    if (!failed_c()) {
                                        glm::vec3 interPos = glm::vec3(tempState[0], tempState[1], tempState[2]);
                                        planet.orbitRenderer->addPoint(intermediateEt, interPos, planet.ParentID);
                                    } else {
                                        reset_c(); // Swallow error if SPICE data is missing for that date
                                    }
                                }
                            }
                            
                            // Add actual stepPos after sub-steps are done
                            planet.orbitRenderer->addPoint(globalSimEt, stepPos, planet.ParentID);
                        }
                    } else {
                        // If time flows backward, delete future points
                        planet.orbitRenderer->erasePointsAfter(globalSimEt);
                    }
                }
                
                } // End planet loop

                // Update Spacecraft Physics (N-body gravity) with sub-stepping for stability
                for (auto& sc : spacecrafts) {
                    if (direction > 0) {
                        // Crucial for numerical stability of LEO/Translunar orbits
                        double maxSubStep = 1.0; 
                        int numSubSteps = (int)ceil(simulationStepSize / maxSubStep);

                        // --- SCIENTIFIC FIX: Dynamic RK4 Step Limit ---
                        // Earth (399) and Sun (10) have very large gravity fields, coarse steps are tolerable.
                        // However, to prevent integration drift in narrow fields like Moon (301) or Mars (499) 
                        // we increase the sub-step limit by 5x.
                        int stepLimit = 2000;
                        if (sc->centerBodySpiceID != 399 && sc->centerBodySpiceID != 10) {
                            stepLimit = 10000; 
                        }

                        if (numSubSteps > stepLimit) numSubSteps = stepLimit; 
                        double dt = simulationStepSize / numSubSteps;
                        // --- FIX END ---
                        
                        double startTime = globalSimEt - simulationStepSize;
                        int oldCenterID = sc->centerBodySpiceID;
                        
                        // Store planet positions and velocities at frame end (globalSimEt)
                        // We will linearly interpolate them backward to each sub-step's time.
                        // This fixes the critical bug where planet positions were 600s AHEAD
                        // of the physics sub-step time, causing ~1.8 km/s Moon velocity error
                        // at SOI transition (Moon acc × 600s = 0.003 × 600 = 1.8 km/s).
                        std::vector<glm::dvec3> framePosKM(planets.size());
                        std::vector<glm::dvec3> frameVelKMS(planets.size());
                        for (int pi = 0; pi < (int)planets.size(); ++pi) {
                            framePosKM[pi]  = planets[pi].PositionKM;
                            frameVelKMS[pi] = planets[pi].VelocityKMS;
                        }

                        
                        for (int i = 0; i < numSubSteps; ++i) {
                            if (sc->isSpiceLinked) {
                                // SPICE Data Linked Mode: Directly set position from kernel, bypass physics entirely.
                                SpiceDouble state[6];
                                SpiceDouble lt;
                                spkezr_c(std::to_string(sc->linkedSpiceID).c_str(), startTime + dt * i, "J2000", "NONE", std::to_string(sc->centerBodySpiceID).c_str(), state, &lt);
                                if (!failed_c()) {
                                    sc->PositionKM = glm::dvec3(state[0], state[1], state[2]);
                                    sc->VelocityKMS = glm::dvec3(state[3], state[4], state[5]);
                                } else {
                                    reset_c();
                                }
                            } else {
                                double current_time = startTime + dt * i;
                                
                                // --- SCIENTIFIC FIX: Exact Ephemeris Evaluation ---
                                // Completely removing Linear Interpolation.
                                // To eliminate N-Body integration drift, in every RK4 sub-step 
                                // we fetch PERFECT J2000 SPICE positions of all planets instantly.
                                for (auto& p : planets) {
                                    if (p.SpiceID != 10) { // Sun (10) is already at (0,0,0) center
                                        SpiceDouble st[6], lt;
                                        // Planet's instantaneous geometric state relative to Sun (10)
                                        spkezr_c(std::to_string(p.SpiceID).c_str(), current_time, "J2000", "NONE", "10", st, &lt);
                                        if (!failed_c()) {
                                            p.PositionKM = glm::dvec3(st[0], st[1], st[2]);
                                            p.VelocityKMS = glm::dvec3(st[3], st[4], st[5]);
                                        } else reset_c();
                                    }
                                }
                                // --- FIX END ---
                                double rem_dt = dt;

                                
                                while (rem_dt > 0.0) {
                                    if (!sc->missionSequence.isFinished) {
                                        double ret_dt = sc->missionSequence.ExecuteStep(sc.get(), current_time, rem_dt, planets);
                                        
                                        if (ret_dt > 0.0 && ret_dt < rem_dt) {
                                            // A command finished mid-step, so we only consume the fractional part it waited for
                                            double consumed = rem_dt - ret_dt;
                                            sc->updatePhysics(consumed, planets);
                                            if (auto* es = dynamic_cast<ElectricSail*>(sc.get())) es->postStep(consumed);
                                            current_time += consumed;
                                            rem_dt = ret_dt;
                                        } else {
                                            // Consumed all remaining time, or returned full rem_dt (finished instantly)
                                            sc->updatePhysics(rem_dt, planets);
                                            if (auto* es = dynamic_cast<ElectricSail*>(sc.get())) es->postStep(rem_dt);
                                            current_time += rem_dt;
                                            rem_dt = 0.0;
                                        }
                                    } else {
                                        sc->updatePhysics(rem_dt, planets); // Free flight propagation
                                        if (auto* es = dynamic_cast<ElectricSail*>(sc.get())) es->postStep(rem_dt);
                                        current_time += rem_dt;
                                        rem_dt = 0.0;
                                    }
                                }
                            }
                            // Restore planets to frame-end positions for visual rendering
                            for (int pi = 0; pi < (int)planets.size(); ++pi) {
                                planets[pi].PositionKM  = framePosKM[pi];
                                planets[pi].VelocityKMS = frameVelKMS[pi];
                            }
                            
                            // Visual smoothing: Add a point roughly every 10% of the steps

                            if (sc->trajectoryRenderer && (numSubSteps < 20 || i % (numSubSteps / 10) == 0 || i == numSubSteps - 1)) {
                                double intermediateTime = startTime + dt * (i + 1);
                                CelestialBody* cBody = nullptr;
                                CelestialBody* earthRef = nullptr;
                                CelestialBody* moonRef = nullptr;
                                for (auto& p : planets) { 
                                    if(p.SpiceID == sc->centerBodySpiceID) cBody = &p; 
                                    if(p.SpiceID == 399) earthRef = &p;
                                    if(p.SpiceID == 301) moonRef = &p;
                                }
                                glm::dvec3 absoluteP = sc->PositionKM + (cBody ? cBody->PositionKM : glm::dvec3(0.0));
                                glm::dvec3 earthCentered = absoluteP - (earthRef ? earthRef->PositionKM : glm::dvec3(0.0));
                                sc->trajectoryRenderer->addPoint(intermediateTime, sc->PositionKM, sc->centerBodySpiceID);

                                // -- Earth-Moon Rotating Frame 2D Hook --
                                if (earthRef && moonRef) {
                                    glm::dvec3 moonEC = moonRef->PositionKM - earthRef->PositionKM;
                                    double theta = atan2(moonEC.y, moonEC.x);
                                    double c = cos(theta);
                                    double s = sin(theta);
                                    double rotX = earthCentered.x * c + earthCentered.y * s;
                                    double rotY = -earthCentered.x * s + earthCentered.y * c;
                                    
                                    PlotPoint2D pt2d;
                                    pt2d.x = rotX; 
                                    pt2d.y = rotY; 
                                    pt2d.et = intermediateTime; 
                                    pt2d.isManeuver = false;
                                    
                                    bool shouldAdd = true;
                                    if (!sc->missionPath2D.empty()) {
                                        const auto& lastPt = sc->missionPath2D.back();
                                        if (!lastPt.isManeuver) {
                                            double dist = sqrt(pow(pt2d.x - lastPt.x, 2) + pow(pt2d.y - lastPt.y, 2));
                                            double r = sqrt(pow(pt2d.x, 2) + pow(pt2d.y, 2));
                                            double threshold = std::max(10.0, r * 0.01);
                                            bool timeThresholdMet = (intermediateTime - lastPt.et) > (86400.0 * 5.0);
                                            if (dist < threshold && !timeThresholdMet) {
                                                shouldAdd = false;
                                            }
                                        }
                                    }
                                    if (shouldAdd) {
                                        sc->missionPath2D.push_back(pt2d);
                                    }
                                }
                            }
                        }

                        if (sc->centerBodySpiceID != oldCenterID) {
                            std::string newName = "Unknown";
                            for (auto& p : planets) { if (p.SpiceID == sc->centerBodySpiceID) { newName = p.Name; break; } }
                            AddLog("[SOI] Spacecraft " + sc->Name + " is now centered on " + newName);
                        }
                    } else if (direction < 0) {
                        // Reverse: Erase points and snap to last recorded point
                        if (sc->trajectoryRenderer) {
                            sc->trajectoryRenderer->erasePointsAfter(globalSimEt);
                            auto it = sc->missionPath2D.begin();
                            while (it != sc->missionPath2D.end()) {
                                if (it->et > globalSimEt) it = sc->missionPath2D.erase(it);
                                else ++it;
                            }
                            if (!sc->trajectoryRenderer->pointsKM.empty()) {
                                CelestialBody* cBody = nullptr;
                                CelestialBody* earthRef = nullptr;
                                for (auto& p : planets) { 
                                    if(p.SpiceID == sc->centerBodySpiceID) cBody = &p; 
                                    if(p.SpiceID == 399) earthRef = &p;
                                }
                                // Reconstruct the absolute Earth-centered position for the 2D plot hook
                                if (sc->isSpiceLinked) {
                                    // If linked to SPICE, just query the state backward directly
                                    SpiceDouble state[6];
                                    SpiceDouble lt;
                                    spkezr_c(std::to_string(sc->linkedSpiceID).c_str(), globalSimEt, "J2000", "NONE", std::to_string(sc->centerBodySpiceID).c_str(), state, &lt);
                                    if (!failed_c()) {
                                        sc->PositionKM = glm::dvec3(state[0], state[1], state[2]);
                                        sc->VelocityKMS = glm::dvec3(state[3], state[4], state[5]);
                                    } else {
                                        reset_c();
                                    }
                                } else {
                                    // Fallback to purely visual track reconstruction for physics-based spacecrafts
                                    glm::dvec3 centerPosAbs(0.0);
                                    for (const auto& planet : planets) {
                                        if (planet.SpiceID == sc->trajectoryRenderer->pointsKM.back().centerBodySpiceID) {
                                            centerPosAbs = planet.PositionKM;
                                            break;
                                        }
                                    }
                                    glm::dvec3 pointAbs = centerPosAbs + sc->trajectoryRenderer->pointsKM.back().posRelative;
                                    CelestialBody* eRef = nullptr;
                                    for (auto& p : planets) { if(p.SpiceID == 399) eRef = &p; }
                                    glm::dvec3 lastEarthCentered = pointAbs - (eRef ? eRef->PositionKM : glm::dvec3(0.0));
                                    glm::dvec3 absoluteP = lastEarthCentered + (earthRef ? earthRef->PositionKM : glm::dvec3(0.0));
                                    sc->PositionKM = absoluteP - (cBody ? cBody->PositionKM : glm::dvec3(0.0));
                                }
                            }
                        }
                    }
                }

                // --- OUTPUT: Record one data point per physical step ---
                g_Script.record(globalSimEt, planets, spacecrafts);

            } // End frame sim loop
        }
            
        // After physics engine catches up, evaluate simulation completion
        if (isSimulating) {
            if (timeSpeedMultiplier > 0 && et_time >= end_et_time && end_et_time != 0.0) {
                et_time = end_et_time;
                isSimulating = false;
                isSimulationFinished = true;
                AddLog("Simulation finished (Reached End Date).");
            } else if (timeSpeedMultiplier < 0 && et_time <= start_et_time && start_et_time != 0.0) {
                et_time = start_et_time;
                isSimulating = false;
                isSimulationFinished = true;
                AddLog("Simulation finished (Reached Start Date).");
            }
        }
            
        // Snap visual positions to the absolute interpolated et_time for smooth animation
        
        if (!isSimulating && !isSimulationFinished) {
            // When paused manually or scrubbing, keep physical time sync'd to visual time 
            // so resuming playback doesn't trigger a massive catch-up jump.
            globalSimEt = et_time;
        }

        for (auto& planet : planets) {
            SpiceDouble state[6];
            SpiceDouble lt;
            
            // Rotation Matrix update (Transform FROM local body frame TO inertial J2000 frame)
            SpiceDouble rotMatrix[3][3];
            std::string frameName = "IAU_" + ToUpper(planet.Name);
            pxform_c(frameName.c_str(), "J2000", et_time, rotMatrix);
            if (!failed_c()) {
                glm::mat4 rMat(1.0f);
                rMat[0][0] = rotMatrix[0][0]; rMat[0][1] = rotMatrix[1][0]; rMat[0][2] = rotMatrix[2][0];
                rMat[1][0] = rotMatrix[0][1]; rMat[1][1] = rotMatrix[1][1]; rMat[1][2] = rotMatrix[2][1];
                rMat[2][0] = rotMatrix[0][2]; rMat[2][1] = rotMatrix[1][2]; rMat[2][2] = rotMatrix[2][2];
                planet.currentRotationMatrix = rMat;
            } else {
                reset_c();
                planet.currentRotationMatrix = glm::mat4(1.0f);
            }

            if (planet.SpiceID != 10) {
                spkezr_c(std::to_string(planet.SpiceID).c_str(), et_time, "J2000", "NONE", "10", state, &lt);
                if (!failed_c()) {
                    planet.PositionKM = glm::dvec3(state[0], state[1], state[2]);
                    planet.VelocityKMS = glm::dvec3(state[3], state[4], state[5]);
                } else { reset_c(); }
                
                if (planet.ParentID != 10 && planet.ParentID != 0) {
                    spkezr_c(std::to_string(planet.ParentID).c_str(), et_time, "J2000", "NONE", "10", state, &lt);
                    if (!failed_c()) {
                        planet.ParentPositionKM = glm::dvec3(state[0], state[1], state[2]);
                    } else { reset_c(); planet.ParentPositionKM = glm::dvec3(0.0); }
                    
                    SpiceDouble relState[6];
                    spkezr_c(std::to_string(planet.SpiceID).c_str(), et_time, "J2000", "NONE", std::to_string(planet.ParentID).c_str(), relState, &lt);
                    if (!failed_c()) {
                        planet.RelativePositionKM = glm::dvec3(relState[0], relState[1], relState[2]);
                        planet.RelativeVelocityKMS = glm::dvec3(relState[3], relState[4], relState[5]);
                    } else {
                        reset_c();
                        planet.RelativePositionKM = planet.PositionKM;
                        planet.RelativeVelocityKMS = planet.VelocityKMS;
                    }
                } else {
                    planet.ParentPositionKM = glm::dvec3(0.0);
                    planet.RelativePositionKM = planet.PositionKM;
                    planet.RelativeVelocityKMS = planet.VelocityKMS;
                }
            } else {
                planet.PositionKM = glm::dvec3(0.0);
                planet.ParentPositionKM = glm::dvec3(0.0);
                planet.RelativePositionKM = glm::dvec3(0.0);
                planet.RelativeVelocityKMS = glm::dvec3(0.0);
            }
        }

        // Output datetime string back from et_time only if simulating or paused (not finished/uninitialized)
        if (isSimulationInitialized && !isSimulationFinished) {
            timout_c(et_time, "YYYY-MM-DDTHR:MN:SC", 32, utctime);
        } else {
            strcpy(utctime, "----/--/--T--:--:--");
        }

        // Handle FBO Resize dynamically if window size changes
        static float lastFboWidth = SCR_WIDTH;
        static float lastFboHeight = SCR_HEIGHT;
        if (viewportWidth > 0 && viewportHeight > 0 && (viewportWidth != lastFboWidth || viewportHeight != lastFboHeight)) {
            glBindTexture(GL_TEXTURE_2D, textureColorBuffer);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, (int)viewportWidth, (int)viewportHeight, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
            glBindRenderbuffer(GL_RENDERBUFFER, rbo);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, (int)viewportWidth, (int)viewportHeight);
            lastFboWidth = viewportWidth;
            lastFboHeight = viewportHeight;
        }



        // --- Render prep: Main Solar System FBO ---
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, (int)viewportWidth, (int)viewportHeight);
        glClearColor(0.02f, 0.02f, 0.05f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // ImGui prep
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        
        // --- Top Main Menu Bar ---
        static bool openNewProjectModal = false;
        static char newProjectName[128] = "MySimulation";
        static char newProjectLocation[MAX_PATH] = "";
        
        static bool openNewSpacecraftModal = false;
        static bool isAddingESail = false;
        static bool isEditingSpacecraft = false;
        static char originalSpacecraftName[128] = "";
        static char newSpacecraftName[128] = "TestSat";
        static int newSpacecraftID = -41;
        static char newSpacecraftEpoch[128] = "2000-JAN-01 12:00:00 ::UTC";
        static char newSpacecraftMissionEpoch[128] = "2000-JAN-01 12:00:00 ::UTC";
        static glm::dvec3 newSpacecraftPos(6771.0, 0.0, 0.0);
        static glm::dvec3 newSpacecraftVel(0.0, 7.6726, 0.0);
        
        static bool useKeplerianInputs = false;
        static double kepSMA = 6771.0;
        static double kepECC = 0.0;
        static double kepINC = 0.0;
        static double kepRAAN = 0.0;
        static double kepArgPe = 0.0;
        static double kepTrueAnom = 0.0;
        
        static float modalEsailLength = 20.0f;
        static int modalEsailCount = 50;
        static float modalEsailVoltage = 25.0f;
        static float modalEsailMass = 150.0f;
        static float modalEsailSpinRA = 0.0f;
        static float modalEsailSpinDEC = 0.0f;
        static float modalEsailDeflectionAngle = 10.0f;
        static double modalEsailThrustMax = 0.0;
        static double modalEsailCharAccel = 0.0;
        
        static float modalEsailSpinRate = 1.0f;




        

        if (openNewProjectModal) {
            ImGui::OpenPopup("Create New Project");
            openNewProjectModal = false;
        }

        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("New Project...")) {
                    openNewProjectModal = true;
                }
                if (ImGui::MenuItem("Open Project...")) {
                    std::string path = OpenFolderDialog("Select ESMAT Project Folder");
                    if (!path.empty()) {
                        if (LoadProjectFromFolder(path)) {
                            isProjectLoaded = true;
                            resetLayoutRequested = true;
                        }
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Upload Kernel...")) {
                    if (isProjectLoaded) {
                        openUploadKernelModal = true;
                    } else {
                        AddLog("[WARN] You must have a project open to upload a kernel.");
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Save Project")) {
                    if (g_Project.currentProjectPath.empty()) {
                        std::string path = OpenFolderDialog("Select an EMPTY Folder to Save Project");
                        if (!path.empty()) {
                            SaveProjectToFolder(path, planets);
                        }
                    } else {
                        SaveProjectToFolder(g_Project.currentProjectPath, planets);
                    }
                }
                if (ImGui::MenuItem("Save Project As...")) {
                    std::string path = OpenFolderDialog("Select an EMPTY Folder to Save Project");
                    if (!path.empty()) {
                        SaveProjectToFolder(path, planets);
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Close Project")) {
                    isProjectLoaded = false;
                    isSimulating = false;
                    isSimulationInitialized = false;
                    isSimulationFinished = false;
                }
                if (ImGui::MenuItem("Exit", "Alt+F4")) {
                    glfwSetWindowShouldClose(window, true);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                if (ImGui::MenuItem("Reset Layout")) {
                    resetLayoutRequested = true;
                }
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        if (!isProjectLoaded) {
            // Render the Welcome Home Screen overlay
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(viewport->WorkSize);
            ImGuiWindowFlags welcomeFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
            
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.95f, 0.95f, 0.95f, 1.0f));
            if (ImGui::Begin("WelcomeScreen", nullptr, welcomeFlags)) {
                
                auto ImageTextButton = [](const char* id, unsigned int textureId, const char* text, ImVec2 size) -> bool {
                    ImVec2 pos = ImGui::GetCursorScreenPos();
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    
                    bool clicked = ImGui::InvisibleButton(id, size);
                    bool hovered = ImGui::IsItemHovered();
                    bool active = ImGui::IsItemActive();
                    
                    ImU32 bgCol = hovered ? (active ? ImGui::GetColorU32(ImVec4(0.85f,0.85f,0.85f,1.0f)) : ImGui::GetColorU32(ImVec4(0.92f,0.92f,0.92f,1.0f))) : ImGui::GetColorU32(ImVec4(0.98f,0.98f,0.98f,1.0f));
                    drawList->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bgCol, 6.0f);
                    drawList->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), ImGui::GetColorU32(ImVec4(0.8f,0.8f,0.8f,1.0f)), 6.0f);
                    
                    float iconSize = 20.0f;
                    ImVec2 iconPos = ImVec2(pos.x + 15.0f, pos.y + (size.y - iconSize) * 0.5f);
                    if (textureId != 0) {
                        drawList->AddImage((void*)(intptr_t)textureId, iconPos, ImVec2(iconPos.x + iconSize, iconPos.y + iconSize));
                    }
                    
                    ImVec2 textSize = ImGui::CalcTextSize(text);
                    ImVec2 textPos = ImVec2(pos.x + 15.0f + iconSize + 10.0f, pos.y + (size.y - textSize.y) * 0.5f);
                    drawList->AddText(textPos, ImGui::GetColorU32(ImVec4(0.2f,0.2f,0.2f,1.0f)), text);
                    
                    return clicked;
                };

                float windowWidth = ImGui::GetWindowWidth();
                float windowHeight = ImGui::GetWindowHeight();
                
                float boxWidth = 400.0f;
                float contentHeight = 550.0f;
                
                float startY = (windowHeight - contentHeight) * 0.5f;
                ImGui::SetCursorPosY(startY > 0 ? startY : 0);
                
                float iconSize = 256.0f;
                ImGui::SetCursorPosX((windowWidth - iconSize) * 0.5f);
                ImGui::Image((void*)(intptr_t)startScreenIcon, ImVec2(iconSize, iconSize));
                
                ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing();
                
                const char* welcomeText = "Welcome to";
                ImGui::SetCursorPosX((windowWidth - ImGui::CalcTextSize(welcomeText).x) * 0.5f);
                ImGui::TextDisabled("%s", welcomeText);
                
                ImGui::Spacing();
                
                ImGui::SetWindowFontScale(2.5f);
                const char* titleText = "Electric Sail Mission Analysis Tool";
                ImGui::SetCursorPosX((windowWidth - ImGui::CalcTextSize(titleText).x) * 0.5f);
                ImGui::TextColored(ImVec4(0.1f, 0.1f, 0.1f, 1.0f), "%s", titleText);
                ImGui::SetWindowFontScale(1.0f);
                
                ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing();
                
                ImGui::SetCursorPosX((windowWidth - boxWidth) * 0.5f);
                if (ImGui::Button("Create New Project", ImVec2(boxWidth, 50.0f))) {
                    openNewProjectModal = true;
                }
                
                ImGui::Spacing(); ImGui::Spacing();
                
                ImGui::SetCursorPosX((windowWidth - boxWidth) * 0.5f);
                if (ImGui::Button("Open Existing Project", ImVec2(boxWidth, 50.0f))) {
                    std::string path = OpenFolderDialog("Select ESMAT Project Folder");
                    if (!path.empty()) {
                        if (LoadProjectFromFolder(path)) {
                            isProjectLoaded = true;
                            resetLayoutRequested = true;
                        }
                    }
                }
                
                ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing(); ImGui::Spacing();
                
                float btnWidth = 160.0f;
                float btnHeight = 40.0f;
                float spacing = 20.0f;
                float totalSocialWidth = (btnWidth * 3) + (spacing * 2);
                
                ImGui::SetCursorPosX((windowWidth - totalSocialWidth) * 0.5f);
                
                if (ImageTextButton("##btn_github", githubIcon, "GitHub", ImVec2(btnWidth, btnHeight))) {
                    ShellExecuteA(0, 0, "https://github.com/sfga12/ESMAT", 0, 0, SW_SHOW);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("View Source Code on GitHub");
                
                ImGui::SameLine(0, spacing);
                
                if (ImageTextButton("##btn_web", webIcon, "Visit Website", ImVec2(btnWidth, btnHeight))) {
                    ShellExecuteA(0, 0, "https://www.esmat.space", 0, 0, SW_SHOW);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Visit Official Website");
                
                ImGui::SameLine(0, spacing);
                
                if (ImageTextButton("##btn_youtube", youtubeIcon, "Tutorial", ImVec2(btnWidth, btnHeight))) {
                    ShellExecuteA(0, 0, "https://www.youtube.com/", 0, 0, SW_SHOW);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Watch Tutorial Videos");
                
            }
            ImGui::End();
            ImGui::PopStyleColor();
        } else {
            // Root Dockspace (Only render IDE if a project is open)
            ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");
            ImGui::DockSpaceOverViewport(dockspace_id, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

            if (resetLayoutRequested) {
                resetLayoutRequested = false;
            
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            
            ImGui::DockBuilderRemoveNode(dockspace_id);
            ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);
            ImGui::DockBuilderSetNodePos(dockspace_id, viewport->WorkPos); // CRITICAL: This was missing and caused it to overlap the menu bar at 0,0!

            auto dock_id_main = dockspace_id;
            
            // Split Top (Controls), Rest (MainArea)
            auto dock_id_controls = ImGui::DockBuilderSplitNode(dock_id_main, ImGuiDir_Up, 0.08f, nullptr, &dock_id_main);
            
            // Split Bottom (Console Output + Output) from MainArea
            auto dock_id_bottom = ImGui::DockBuilderSplitNode(dock_id_main, ImGuiDir_Down, 0.25f, nullptr, &dock_id_main);
            // Subdivide bottom: left = Console, right = Output
            ImGuiID dock_id_console, dock_id_output;
            dock_id_console = ImGui::DockBuilderSplitNode(dock_id_bottom, ImGuiDir_Left, 0.50f, nullptr, &dock_id_output);

            // Split Right (Editor) from MainArea
            auto dock_id_editor = ImGui::DockBuilderSplitNode(dock_id_main, ImGuiDir_Right, 0.30f, nullptr, &dock_id_main);

            // Split Left (Explorer) from MainArea
            auto dock_id_explorer = ImGui::DockBuilderSplitNode(dock_id_main, ImGuiDir_Left, 0.25f, nullptr, &dock_id_main);
            
            // dock_id_main is now the remaining central node (View)

            ImGui::DockBuilderDockWindow("Controls", dock_id_controls);
            ImGui::DockBuilderDockWindow("Resources", dock_id_explorer);
            ImGui::DockBuilderDockWindow("Output", dock_id_explorer);  // tabs alongside Resources
            ImGui::DockBuilderDockWindow("Mission", dock_id_explorer); // tabs alongside Resources
            ImGui::DockBuilderDockWindow("Mission Output", dock_id_explorer); // tabs alongside Resources
            ImGui::DockBuilderDockWindow("View", dock_id_main);
            ImGui::DockBuilderDockWindow("Editor", dock_id_editor);
            ImGui::DockBuilderDockWindow("Console Output", dock_id_console);

            ImGui::DockBuilderFinish(dockspace_id);
        }

        // Global target body tracking for origin shifting (using names for safety)
        static std::string currentOriginBodyName = "";
        static std::string currentOriginSpacecraftName = "";
        
        // Find the currently selected body from the UI
        CelestialBody* selectedBody = nullptr;
        if (!selectedBodyName.empty()) {
            for (auto& p : planets) {
                if (p.Name == selectedBodyName) {
                    selectedBody = &p;
                    break;
                }
            }
        }
        
        CelestialBody* originBody = nullptr;
        if (!currentOriginBodyName.empty()) {
            for (auto& p : planets) { if (p.Name == currentOriginBodyName) { originBody = &p; break; } }
        }
        
        Spacecraft* originSpacecraft = nullptr;
        if (!currentOriginSpacecraftName.empty()) {
            for (auto& s : spacecrafts) { if (s->Name == currentOriginSpacecraftName) { originSpacecraft = s.get(); break; } }
        }

        if (!originBody && !originSpacecraft && !planets.empty()) {
            for (auto& p : planets) {
                if (p.SpiceID == 10) { originBody = &p; currentOriginBodyName = p.Name; break; }
            }
            if (!originBody) { originBody = &planets.front(); currentOriginBodyName = originBody->Name; }
        }

        glm::dvec3 worldOriginKM = glm::dvec3(0.0);
        if (camera.isArcball) {
            if (originBody) {
                worldOriginKM = originBody->PositionKM;
                camera.SetArcballTarget(glm::vec3(0.0f), camera.Distance);
            } else if (originSpacecraft) {
                // Determine center body to get absolute position
                CelestialBody* cb = nullptr;
                for (auto& p : planets) { if (p.SpiceID == originSpacecraft->centerBodySpiceID) { cb = &p; break; } }
                worldOriginKM = (cb ? cb->PositionKM : glm::dvec3(0.0)) + originSpacecraft->PositionKM;
                camera.SetArcballTarget(glm::vec3(0.0f), camera.Distance);
            }
        }
        
        // Execute pending auto-focus (triggered from sync section)
        if (g_pendingFocusID >= 0 && !planets.empty()) {
            for (auto& p : planets) {
                if (p.SpiceID == g_pendingFocusID) {
                    currentOriginBodyName = p.Name;
                    currentOriginSpacecraftName = "";
                    float renderRadius = (p.SpiceID == 10)
                        ? p.RadiusKM * (float)scaleFactor * 0.1f  // Sun shown at 1/10th scale
                        : p.RadiusKM * (float)scaleFactor * 1.0f; // Default 1x scale
                    camera.MinDistance = (std::max)(renderRadius * 1.5f, 1e-7f);
                    float dist = (std::max)(renderRadius * 12.0f, camera.MinDistance);
                    camera.SetArcballTarget(glm::vec3(0.0f), dist);
                    camera.Yaw = -90.0f;
                    camera.Pitch = 30.0f;
                    camera.ProcessMouseMovement(0, 0);
                    g_pendingFocusID = -1;
                    break;
                }
            }
        }

        // Increase far clipping plane significantly so zooming out massively doesn't cull planets
        // Modify near clipping plane dynamically based on camera distance to avoid cutting into closely inspected small planets
        float farPlane = 5000000.0f; // Extremely huge far plane
        float nearPlane = (camera.Distance * 0.001f < 0.0000001f) ? 0.0000001f : (camera.Distance * 0.001f);
        glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom), viewportWidth / viewportHeight, nearPlane, farPlane);
        glm::mat4 view = camera.GetViewMatrix();

        struct AxisLabel { glm::vec3 pos; std::string text; glm::vec3 color; };
        std::vector<AxisLabel> axisLabels;

        sphereShader.use();
        sphereShader.setMat4("projection", projection);
        sphereShader.setMat4("view", view);
        sphereShader.setFloat("farPlane", farPlane);
        sphereShader.setVec3("lightColor",  1.0f, 1.0f, 1.0f);
        
        // Sun is light source. Its SPICE origin is (0,0,0) natively
        glm::vec3 sunRenderPos = glm::vec3((glm::dvec3(0.0) - worldOriginKM) * scaleFactor);
        sphereShader.setVec3("lightPos", sunRenderPos); 
        sphereShader.setVec3("viewPos", camera.Position);

        static float planetSizeExaggeration = 1.0f; // Global to control zooming

        for (auto& planet : planets) {
            if (!planet.showBody) continue; // Skip rendering mesh if toggled off
            
            glm::mat4 model = glm::mat4(1.0f);
            glm::vec3 renderPos = glm::vec3((planet.PositionKM - worldOriginKM) * scaleFactor);
            model = glm::translate(model, renderPos);
            
            // Apply physical rotation
            model = model * planet.currentRotationMatrix;
            
            // Adjust sun radius carefully, but let planets scale based on user UI
            float renderRadius = (planet.SpiceID == 10) ? planet.RadiusKM * scaleFactor * (planetSizeExaggeration / 10.0f) : planet.RadiusKM * scaleFactor * planetSizeExaggeration;
            model = glm::scale(model, glm::vec3(renderRadius));

            sphereShader.setMat4("model", model);
            sphereShader.setVec3("objectColor", planet.Color);
            
            if (planet.SpiceID == 10) {
                sphereShader.setInt("isSun", 1);
            } else {
                sphereShader.setInt("isSun", 0);
            }
            
            if (planet.HasTexture && planet.TextureID > 0) {
                sphereShader.setInt("useTexture", 1);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, planet.TextureID);
                sphereShader.setInt("texture1", 0);
            } else {
                sphereShader.setInt("useTexture", 0);
            }
            
            glBindVertexArray(sphereVAO);
            glDrawElements(GL_TRIANGLES, sphere.indices.size(), GL_UNSIGNED_INT, 0);
        }


        // Spacecraft Rendering
        sphereShader.use();
        for (auto& sc : spacecrafts) {
            if (!sc->showBody) continue;
            
            // Find center body for world position
            CelestialBody* centerBody = nullptr;
            for (auto& p : planets) { if (p.SpiceID == sc->centerBodySpiceID) { centerBody = &p; break; } }
            if (!centerBody) continue;

            // Determine if we have a model; if yes, we shrink the sphere to be just a tiny center point
            bool hasModel = (sc->model && sc->model->loaded);
            
            glm::mat4 model = glm::mat4(1.0f);
            glm::vec3 renderPos = glm::vec3((sc->PositionKM + centerBody->PositionKM - worldOriginKM) * scaleFactor);
            model = glm::translate(model, renderPos);
            
            // Spacecraft sphere base scale
            float scScale = (float)scaleFactor * 100.0f * planetSizeExaggeration; 
            
            // If it has a model, the sphere just becomes a tiny center pin (1% size)
            float renderSphereScale = hasModel ? scScale * 0.01f : scScale;
            model = glm::scale(model, glm::vec3(renderSphereScale)); 
            
            sphereShader.setMat4("model", model);
            sphereShader.setVec3("color", sc->Color);
            
            // Render the sphere marker
            sphereShader.setInt("isSun", 0);
            sphereShader.setInt("useTexture", 0);
            glBindVertexArray(sphereVAO);
            glDrawElements(GL_TRIANGLES, sphere.indices.size(), GL_UNSIGNED_INT, 0);

            // Render the GLB model if loaded
            if (!std::dynamic_pointer_cast<ElectricSail>(sc) && sc->model && sc->model->loaded) {
                // Models inside might be too big or small, apply an additional arbitrary scale multiplier for visibility if needed
                // It sits precisely at center of the sphere
                
                // Let's use the sphereShader but set the "color" uniform or just rely on what we have
                // For a proper GLTF we'd need a PBR shader but we'll adapt to this simple shader for now 
                // Color is applied as `color` in some shaders, or `objectColor` in others.
                sphereShader.setVec3("objectColor", glm::vec3(0.8f, 0.8f, 0.8f)); // Greyish silver default
                
                // Normals are now loaded, so we can use standard lighting
                sphereShader.setInt("isSun", 0);
                
                // The loaded model is scaled so its maximum geometric dimension fits perfectly inside the sphere's radius.
                glm::mat4 glbModel = glm::mat4(1.0f);
                glbModel = glm::translate(glbModel, renderPos);
                
                float normalizedScale = scScale / sc->model->maxAbsCoord;
                glbModel = glm::scale(glbModel, glm::vec3(normalizedScale)); // Fit exactly inside sphere bounds

                sc->model->Draw(sphereShader, glbModel);
            }
            
            // Procedural Electric Sail Rendering
            if (auto es = std::dynamic_pointer_cast<ElectricSail>(sc)) {
                if (!es->esailRenderer) {
                    es->esailRenderer = std::make_shared<ESailMesh>();
                }
                es->esailRenderer->updateTethers(es->esailTetherCount, (float)es->esailDeflectionAngleDeg);
                
                // Calculate E-Sail specific orientation matrix relative to the Sun!
                const CelestialBody* sunBody = nullptr;
                for (auto& p : planets) { if (p.SpiceID == 10) { sunBody = &p; break; } }
                
                glm::mat4 sailModel = glm::mat4(1.0f);
                sailModel = glm::translate(sailModel, renderPos);
                
                // E-sails are typically microscopic visually (20+ km radius is tiny in interplanetary space).
                // Instead of physical metric scaling, use the constant Spacecraft Scale parameter so it remains uniformly 
                // visible alongside other spacecraft without over-inflating relative to the planets.
                float sailScale = scScale * 4.0f; // Roughly 4x the radius of standard spacecraft markers to let tethers shine
                
                // Absolute Frame (J2000) Spacecraft Body Orientation
                glm::dvec3 n_vec;
                n_vec.x = std::cos(es->esailSpinDEC) * std::cos(es->esailSpinRA);
                n_vec.y = std::cos(es->esailSpinDEC) * std::sin(es->esailSpinRA);
                n_vec.z = std::sin(es->esailSpinDEC);
                
                glm::dvec3 z_pole(0.0, 0.0, 1.0);
                glm::dvec3 local_x(1.0, 0.0, 0.0);
                
                // Avoid singularity if pointing straight up or down the Z axis
                if (std::abs(glm::dot(n_vec, z_pole)) > 0.9999) {
                    local_x = glm::normalize(glm::cross(n_vec, glm::dvec3(1.0, 0.0, 0.0)));
                } else {
                    local_x = glm::normalize(glm::cross(z_pole, n_vec)); // Parallel to celestial equator
                }
                
                glm::dvec3 local_y = glm::normalize(glm::cross(n_vec, local_x));

                glm::mat4 R(1.0f);
                R[0] = glm::vec4(local_x.x, local_x.y, local_x.z, 0.0f);
                R[1] = glm::vec4(local_y.x, local_y.y, local_y.z, 0.0f);
                R[2] = glm::vec4(n_vec.x, n_vec.y, n_vec.z, 0.0f);
                
                sailModel = sailModel * R;
                
                sailModel = glm::scale(sailModel, glm::vec3(sailScale));
                
                // 1. Draw solid body component (Grey core + Blue solar arrays)
                sphereShader.setInt("isSun", 0);
                sphereShader.setInt("useTexture", 0);
                sphereShader.setMat4("model", sailModel);
                
                glBindVertexArray(es->esailRenderer->bodyVAO);
                glDrawElements(GL_TRIANGLES, (GLsizei)es->esailRenderer->indexCount, GL_UNSIGNED_INT, 0);
                
                // 2. Draw procedural tethers via GL_LINES with lineShader
                lineShader.use();
                lineShader.setMat4("projection", projection);
                lineShader.setMat4("view", view);
                lineShader.setMat4("model", sailModel);
                lineShader.setVec3("lineColor", glm::vec3(0.8f, 0.62f, 0.1f)); // Metallic copper/golden wire
                
                glBindVertexArray(es->esailRenderer->tethersVAO);
                glLineWidth(1.5f);
                glDrawArrays(GL_LINES, 0, (GLsizei)es->esailRenderer->tetherVertexCount);
                glLineWidth(1.0f);
                
                // 3. Draw local Body Frame coordinate axes (RGB) - Only if focused
                bool isFocused = (sc.get() == originSpacecraft);
                if (isFocused) {
                    // We split into Lines (shafts) and Triangles (vectorial crossed heads)
                    glLineWidth(6.0f); // Even more prominent
                    
                    // +Z Axis (Blue)
                    glm::vec3 colorZ(0.3f, 0.5f, 1.0f);
                    lineShader.setVec3("lineColor", colorZ);
                    glBindVertexArray(es->esailRenderer->axesVAO);
                    glDrawArrays(GL_LINES, 0, 2);
                    // glBindVertexArray(es->esailRenderer->axesTriVAO);
                    // glDrawArrays(GL_TRIANGLES, 0, 6);
                    
                    // +X Axis (Red)
                    glm::vec3 colorX(1.0f, 0.3f, 0.3f);
                    lineShader.setVec3("lineColor", colorX);
                    glBindVertexArray(es->esailRenderer->axesVAO);
                    glDrawArrays(GL_LINES, 2, 2);
                    // glBindVertexArray(es->esailRenderer->axesTriVAO);
                    // glDrawArrays(GL_TRIANGLES, 6, 6);
                    
                    // +Y Axis (Green)
                    glm::vec3 colorY(0.3f, 1.0f, 0.3f);
                    lineShader.setVec3("lineColor", colorY);
                    glBindVertexArray(es->esailRenderer->axesVAO);
                    glDrawArrays(GL_LINES, 4, 2);
                    // glBindVertexArray(sc.esailRenderer->axesTriVAO);
                    // glDrawArrays(GL_TRIANGLES, 12, 6);
                    
                    glLineWidth(1.0f);
                    
                    glm::vec4 tipZ = sailModel * glm::vec4(0, 0, 1.45f, 1.0f);
                    glm::vec4 tipX = sailModel * glm::vec4(1.45f, 0, 0, 1.0f);
                    glm::vec4 tipY = sailModel * glm::vec4(0, 1.45f, 0, 1.0f);
                    axisLabels.push_back({glm::vec3(tipZ), "+n", colorZ});
                    axisLabels.push_back({glm::vec3(tipX), "+x", colorX});
                    axisLabels.push_back({glm::vec3(tipY), "+y", colorY});
                }
                
                // Restore Sphere shader for the subsequent spacecraft instances in loop
                glBindVertexArray(0);
                sphereShader.use();
            }
        }

        // Draw Orbits & Spacecraft Trajectories
        lineShader.use();
        lineShader.setMat4("projection", projection);
        lineShader.setMat4("view", view);
        lineShader.setFloat("farPlane", farPlane);
        for (auto& planet : planets) {
            if (planet.orbitRenderer && !planet.orbitRenderer->pointsKM.empty() && planet.showOrbit) {
                lineShader.setVec3("lineColor", planet.Color); 
                lineShader.setMat4("model", glm::mat4(1.0f));
                planet.orbitRenderer->draw(et_time, worldOriginKM, scaleFactor, planet.PositionKM, planets);
            }
        }
        for (auto& sc : spacecrafts) {
            if (sc->trajectoryRenderer && !sc->trajectoryRenderer->pointsKM.empty() && sc->showTrajectory) {
                lineShader.setVec3("lineColor", sc->Color);
                lineShader.setMat4("model", glm::mat4(1.0f));
                
                // Find center bodies
                CelestialBody* centerBody = nullptr;
                CelestialBody* earthBody = nullptr;
                for (auto& p : planets) { 
                    if (p.SpiceID == sc->centerBodySpiceID) centerBody = &p; 
                    if (p.SpiceID == 399) earthBody = &p; 
                }
                
                glm::dvec3 earthShift = (earthBody ? earthBody->PositionKM : glm::dvec3(0.0)) - worldOriginKM;
                
                glm::dvec3 currentAbsolutePos = sc->PositionKM + (centerBody ? centerBody->PositionKM : glm::dvec3(0.0));
                glm::dvec3 currentEarthCentered = currentAbsolutePos - (earthBody ? earthBody->PositionKM : glm::dvec3(0.0));
                
                sc->trajectoryRenderer->draw(et_time, worldOriginKM, scaleFactor, currentAbsolutePos, planets);
            }
        }

        // --- Draw Reference Lines (Equator & Prime Meridian) for focused body only ---
        if (originBody && originBody->showBody) {
            auto& focusBody = *originBody;
            float renderRadius = (focusBody.SpiceID == 10)
                ? focusBody.RadiusKM * (float)scaleFactor * (planetSizeExaggeration / 10.0f)
                : focusBody.RadiusKM * (float)scaleFactor * planetSizeExaggeration;
            float lineRadius = renderRadius * 1.002f; // Slightly outside sphere to avoid z-fighting

            glm::vec3 renderPos = glm::vec3((focusBody.PositionKM - worldOriginKM) * scaleFactor);

            glm::mat4 lineModel = glm::mat4(1.0f);
            lineModel = glm::translate(lineModel, renderPos);
            lineModel = lineModel * focusBody.currentRotationMatrix;
            lineModel = glm::scale(lineModel, glm::vec3(lineRadius));

            lineShader.use();
            lineShader.setMat4("projection", projection);
            lineShader.setMat4("view", view);
            lineShader.setFloat("farPlane", farPlane);
            lineShader.setMat4("model", lineModel);

            glLineWidth(2.0f);

            // Equator: cyan
            lineShader.setVec3("lineColor", glm::vec3(0.0f, 1.0f, 1.0f));
            glBindVertexArray(earthLineVAO[0]);
            glDrawArrays(GL_LINE_STRIP, 0, (GLsizei)(CIRCLE_SEGMENTS + 1));

            // Prime Meridian: yellow
            lineShader.setVec3("lineColor", glm::vec3(1.0f, 0.9f, 0.0f));
            glBindVertexArray(earthLineVAO[1]);
            glDrawArrays(GL_LINE_STRIP, 0, (GLsizei)(CIRCLE_SEGMENTS + 1));

            glLineWidth(1.0f);
            glBindVertexArray(0);
        }

        // Draw ImGui Windows

        // 1. Top Control Bar
        ImGui::Begin("Controls");
        
        // Reset to Start (Only disable if not initialized yet)
        if (!isSimulationInitialized) ImGui::BeginDisabled();
        if (ImGui::Button("|<")) {
            globalSimEt = start_et_time;
            et_time = start_et_time;
            isSimulating = false;
            isSimulationFinished = false;
            g_Script.clear();
            for (auto& planet : planets) {
                if (planet.orbitRenderer) planet.orbitRenderer->updatePoints(std::vector<TrailPoint>());
            }
            for (auto& sc : spacecrafts) {
                sc->reset();
            }
        }
        if (!isSimulationInitialized) ImGui::EndDisabled();
        ImGui::SameLine();
        
        float baseSpeed = simulationStepSize; // "1x speed" equals 1 simulation step per real-time second
        
        bool isP = !isSimulating;
        if (isP) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.4f, 0.0f, 1.0f));
            if (ImGui::Button("|| PAUSED")) isSimulating = false;
            ImGui::PopStyleColor();
        } else {
            if (ImGui::Button("||")) isSimulating = false;
        }
        ImGui::SameLine();
        
        bool pushedRev = (timeSpeedMultiplier == -baseSpeed);
        if (pushedRev) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.6f, 0.0f, 1.0f));
        if (ImGui::Button("<")) { 
            if (isSimulationInitialized && !isSimulationFinished) isSimulating = true; 
            timeSpeedMultiplier = -baseSpeed; 
        }
        if (pushedRev) ImGui::PopStyleColor();
        ImGui::SameLine();

        bool pushedFwd1 = (timeSpeedMultiplier == baseSpeed);
        if (pushedFwd1) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.6f, 0.0f, 1.0f));
        if (ImGui::Button(">")) { 
            if (isSimulationInitialized && !isSimulationFinished) isSimulating = true; 
            timeSpeedMultiplier = baseSpeed; 
        }
        if (pushedFwd1) ImGui::PopStyleColor();
        ImGui::SameLine();
        
        bool pushedFwd10 = (timeSpeedMultiplier == baseSpeed * 10.0f);
        if (pushedFwd10) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.6f, 0.0f, 1.0f));
        if (ImGui::Button(">>")) { 
            if (isSimulationInitialized && !isSimulationFinished) isSimulating = true; 
            timeSpeedMultiplier = baseSpeed * 10.0f; 
        }
        if (pushedFwd10) ImGui::PopStyleColor();
        ImGui::SameLine();

        bool pushedFwd100 = (timeSpeedMultiplier == baseSpeed * 100.0f);
        if (pushedFwd100) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.6f, 0.0f, 1.0f));
        if (ImGui::Button(">>>")) { 
            if (isSimulationInitialized && !isSimulationFinished) isSimulating = true; 
            timeSpeedMultiplier = baseSpeed * 100.0f; 
        }
        if (pushedFwd100) ImGui::PopStyleColor();
        ImGui::SameLine();
        
        bool pushedMax = (timeSpeedMultiplier == baseSpeed * 50000.0f);
        if (pushedMax) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.6f, 0.0f, 1.0f));
        if (ImGui::Button("MAX")) { 
            if (isSimulationInitialized && !isSimulationFinished) isSimulating = true; 
            timeSpeedMultiplier = baseSpeed * 50000.0f; 
        }
        if (pushedMax) ImGui::PopStyleColor();
        
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        ImGui::SliderFloat("Planet Size", &planetSizeExaggeration, 1.0f, 200.0f, "%.1fx");
        
        ImGui::SameLine();
        ImGui::Text(" | Current UTC: %s", utctime);
        ImGui::End();

        // 2. Hierarchy Explorer
        ImGui::Begin("Resources");
        
        // Root project files
        if (g_Project.currentProjectPath.empty()) {
            ImGui::TextDisabled("Unsaved Project");
        } else {
            std::string baseName = g_Project.currentProjectPath;
            size_t lastSlash = baseName.find_last_of("\\/");
            if (lastSlash != std::string::npos) {
                baseName = baseName.substr(lastSlash + 1);
            }
            ImGui::TextColored(ImVec4(0.0f, 0.3f, 0.7f, 1.0f), "\xEF\x84\x95 Project: %s.esmatproject", baseName.c_str());
        }

        std::string swLabel = "\xF0\x9F\x8C\xAC Solar Wind";
        if (ImGui::Selectable(swLabel.c_str(), openedFile == "Solar Wind Data")) {
            openedFile = "Solar Wind Data";
            selectedBodyName = "";
            selectedSpacecraftName = "";
        }

        std::string cfgLabel = "\xF0\x9F\x93\x84 SimulationSettings.cfg";
        if (ImGui::Selectable(cfgLabel.c_str(), openedFile == "SimulationSettings.cfg")) {
            openedFile = "SimulationSettings.cfg";
            selectedBodyName = "";
        }

        // OutputScript.esmat — appears right after SimulationSettings.cfg with scroll icon
        std::string scriptFileLabel = "\xF0\x9F\x93\x9C OutputScript.esmat";
        if (ImGui::Selectable(scriptFileLabel.c_str(), openedFile == "OutputScript.esmat")) {
            openedFile = "OutputScript.esmat";
            selectedBodyName = "";
        }

        ImGui::Spacing();
        
        // SolarSystem directory
        std::string solarLabel = "\xF0\x9F\x93\x81 SolarSystem";
        if (ImGui::TreeNodeEx(solarLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            for (auto& planet : planets) {
                // Determine icon
                std::string icon = "\xF0\x9F\x8C\x91"; // Default dark moon
                if (planet.SpiceID == 10) icon = "\xE2\x98\x80"; // Sun
                else if (planet.SpiceID == 399) icon = "\xF0\x9F\x8C\x8D"; // Earth
                else if (planet.SpiceID == 499) icon = "\xF0\x9F\xAA\x90"; // Mars (Ringed Planet fallback if Mars emoji U+1F30C isn't there, or use standard dot)
                
                std::string filename = planet.Name + ".esmatobj";
                std::string label = icon + " " + filename;
                if (ImGui::Selectable(label.c_str(), openedFile == filename)) {
                    openedFile = filename;
                    selectedBodyName = planet.Name; // Select the body via name
                }
            }
            ImGui::TreePop();
        }

        // Spacecrafts directory
        std::string scLabel = "\xF0\x9F\x9A\x80 Spacecrafts";
        bool scNodeOpen = ImGui::TreeNodeEx(scLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        
        // Right-Click Context Menu for Spacecrafts
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Add Standard Spacecraft")) {
                if (isProjectLoaded) {
                    openNewSpacecraftModal = true;
                    isAddingESail = false;
                    isEditingSpacecraft = false;
                } else {
                    AddLog("[WARN] You must have a project open to add a spacecraft.");
                }
            }
            if (ImGui::MenuItem("Add Electric Sail")) {
                if (isProjectLoaded) {
                    openNewSpacecraftModal = true;
                    isAddingESail = true;
                    isEditingSpacecraft = false;
                } else {
                    AddLog("[WARN] You must have a project open to add an Electric Sail.");
                }
            }
            ImGui::EndPopup();
        }

        if (scNodeOpen) {
            int scToRemove = -1;
            for (int i = 0; i < spacecrafts.size(); i++) {
                auto& sc = spacecrafts[i];
                std::string filename = sc->Name + ".esmatspacecraft";
                std::string label = (std::dynamic_pointer_cast<ElectricSail>(sc) ? "\xE2\x9A\xA1 " : "\xF0\x9F\x9B\xB0 ") + filename;
                bool isESail = (std::dynamic_pointer_cast<ElectricSail>(sc) != nullptr);
                bool isSelected = (openedFile == filename);
                
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
                if (isSelected) flags |= ImGuiTreeNodeFlags_Selected;
                if (!isESail) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

                bool nodeOpen = ImGui::TreeNodeEx(("SC_NODE_" + sc->Name).c_str(), flags, "%s", label.c_str());
                if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                    openedFile = filename;
                    selectedSpacecraftName = sc->Name;
                    selectedBodyName = ""; // Deselect bodies
                }

                if (isESail && nodeOpen) {
                    ImGui::TreePop();
                }

                // Right-Click Context Menu for INDIVIDUAL Spacecraft (Edit/Delete)
                if (ImGui::BeginPopupContextItem(("SC_CTX_" + sc->Name).c_str())) {
                    if (ImGui::MenuItem("Edit Spacecraft Settings")) {
                        isEditingSpacecraft = true;
                        openNewSpacecraftModal = true;
                        strcpy_s(originalSpacecraftName, sizeof(originalSpacecraftName), sc->Name.c_str());
                        strcpy_s(newSpacecraftName, sizeof(newSpacecraftName), sc->Name.c_str());
                        newSpacecraftID = sc->ID;
                        newSpacecraftPos = sc->InitialPositionKM;
                        newSpacecraftVel = sc->InitialVelocityKMS;
                        
                        char etToStr[128];
                        timout_c(sc->epochET, "YYYY-MON-DD HR:MN:SC ::UTC", 128, etToStr);
                        if (!failed_c()) strcpy_s(newSpacecraftEpoch, sizeof(newSpacecraftEpoch), etToStr);
                        
                        char metToStr[128];
                        timout_c(sc->missionEpochET, "YYYY-MON-DD HR:MN:SC ::UTC", 128, metToStr);
                        if (!failed_c()) strcpy_s(newSpacecraftMissionEpoch, sizeof(newSpacecraftMissionEpoch), metToStr);
                        
                        if (auto es = std::dynamic_pointer_cast<ElectricSail>(sc)) {
                            isAddingESail = true;
                            modalEsailLength = es->esailLengthKM;
                            modalEsailCount = es->esailTetherCount;
                            modalEsailSpinRA = glm::degrees(es->esailSpinRA);
                            modalEsailVoltage = es->esailVoltageKV;
                            modalEsailMass = es->esailMassKG;
                            modalEsailSpinDEC = glm::degrees(es->esailSpinDEC);
                            modalEsailDeflectionAngle = es->esailDeflectionAngleDeg;

                            modalEsailSpinRate = es->spinRateRPM;




                            
                            selectedSolarWindIdx = 0;
                            for (size_t wi = 0; wi < projectSolarWinds.size(); wi++) {
                                if (projectSolarWinds[wi].Name == es->attachedWindName) {
                                    selectedSolarWindIdx = wi;
                                    break;
                                }
                            }
                        } else {
                            isAddingESail = false;
                        }
                    }
                    if (ImGui::MenuItem("Remove Spacecraft")) {
                        std::string filepath = g_Project.currentProjectPath + "\\Spacecrafts\\" + filename;
                        remove(filepath.c_str());
                        scToRemove = i;
                        if (openedFile == filename) openedFile = "";
                        AddLog("[SUCCESS] Removed spacecraft: " + filename);
                    }
                    ImGui::EndPopup();
                }
            }
            if (scToRemove >= 0) spacecrafts.erase(spacecrafts.begin() + scToRemove);
            ImGui::TreePop();
        }
        
        // Navigation/Control directory
        std::string navLabel = "\xF0\x9F\x9B\xA0 Navigation/Control";
        if (ImGui::TreeNodeEx(navLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            std::string navPlanLabel = "\xF0\x9F\x93\x9D Navigation Plan";
            if (ImGui::Selectable(navPlanLabel.c_str(), openedFile == "Navigation Plan")) {
                openedFile = "Navigation Plan";
                selectedBodyName = "";
            }
            ImGui::TreePop();
        }
        
        ImGui::Spacing();
        ImGui::End();

        
        // 3. Editor Window
        ImGui::Begin("Editor");
        if (openedFile == "Navigation Plan") {
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "\xF0\x9F\x9B\xA0 Navigation & Control");
            ImGui::Separator();
            
            static int selScIdx = 0;
            std::vector<const char*> scNames;
            for (auto& s : spacecrafts) scNames.push_back(s->Name.c_str());
            if (!scNames.empty()) {
                if (selScIdx >= scNames.size()) selScIdx = 0;
                ImGui::Combo("Spacecraft", &selScIdx, scNames.data(), (int)scNames.size());
            } else {
                ImGui::TextDisabled("No Spacecrafts Available");
            }
            
            static int centralBodyIdx = 10; // Sun default
            std::vector<int> bIds; 
            std::vector<const char*> bNames;
            for (auto& p : planets) {
                if (p.GM > 0) { bIds.push_back(p.SpiceID); bNames.push_back(p.Name.c_str()); }
            }
            int cIdx = 0;
            for (size_t i=0; i<bIds.size(); i++) if(bIds[i]==centralBodyIdx) cIdx = (int)i;
            if (ImGui::Combo("Central Body", &cIdx, bNames.data(), (int)bNames.size())) {
                centralBodyIdx = bIds[cIdx];
            }
            
            ImGui::Separator();
            ImGui::Text("Target Sequence (Transfers, Flybys & Insertions)");
            
            enum class MissionObjective { Flyby = 0, OrbitInsertion = 1, Impact = 2 };
            struct NavTarget { int spiceID; float tofDays; MissionObjective objective; float targetAltKm; };
            static std::vector<NavTarget> navTargets;
            
            static const char* objNames[] = { "Flyby", "Orbit Insertion", "Impact" };

            for (size_t i = 0; i < navTargets.size(); i++) {
                ImGui::PushID((int)i);
                int tIdx = 0;
                for (size_t j=0; j<bIds.size(); j++) if(bIds[j]==navTargets[i].spiceID) tIdx = (int)j;
                ImGui::SetNextItemWidth(130);
                if (ImGui::Combo("Target", &tIdx, bNames.data(), (int)bNames.size())) {
                    navTargets[i].spiceID = bIds[tIdx];
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80);
                ImGui::InputFloat("TOF (d)", &navTargets[i].tofDays, 0.0f, 0.0f, "%.1f");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(110);
                int objIdx = (int)navTargets[i].objective;
                if (ImGui::Combo("Mode", &objIdx, objNames, 3)) {
                    navTargets[i].objective = (MissionObjective)objIdx;
                }
                if (navTargets[i].objective == MissionObjective::OrbitInsertion ||
                    navTargets[i].objective == MissionObjective::Impact) {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(70);
                    ImGui::InputFloat("Alt(km)", &navTargets[i].targetAltKm, 0.0f, 0.0f, "%.0f");
                }
                ImGui::SameLine();
                if (ImGui::Button("X")) { navTargets.erase(navTargets.begin() + i); }
                ImGui::PopID();
            }
            
            if (ImGui::Button("+ Add Target")) {
                navTargets.push_back({301, 3.5f, MissionObjective::Flyby, 100.0f});
            }
            
            ImGui::Separator();
            static double startETOffsetDays = 0.0;
            ImGui::InputDouble("Initial Departure Delay (Days)", &startETOffsetDays, 1.0, 10.0, "%.3f");

            static bool autoOptimize = true;
            ImGui::Checkbox("Auto-Optimize Trajectory (Fastest Route / Min Delta-V)", &autoOptimize);
            if (autoOptimize) {
                ImGui::TextDisabled("Note: Central Body and optimal transfer dates are purely determined geometrically.");
            }

            if (ImGui::Button("GENERATE MISSION SEGMENTS TO MISSION TABLE", ImVec2(ImGui::GetContentRegionAvail().x, 30))) {
                if (selScIdx >= 0 && selScIdx < (int)spacecrafts.size() && !navTargets.empty()) {
                    auto& sc = spacecrafts[selScIdx];
                    
                    g_BurnTable.clear(); // Clear existing burns to prevent trajectory corruption

                    // --- AUTO DETECT CENTRAL BODY ---
                    if (autoOptimize) {
                        std::vector<int> ances1;
                        int curr1 = sc->centerBodySpiceID;
                        while (curr1 != 0) {
                            ances1.push_back(curr1);
                            if (curr1 == 10) break;
                            bool f = false;
                            for (auto& p : planets) { if (p.SpiceID == curr1) { curr1 = p.ParentID; f = true; break; } }
                            if (!f) break;
                        }
                        int curr2 = navTargets[0].spiceID;
                        int commonParent = 10;
                        while (curr2 != 0) {
                            if (std::find(ances1.begin(), ances1.end(), curr2) != ances1.end()) { commonParent = curr2; break; }
                            if (curr2 == 10) break;
                            bool f = false;
                            for (auto& p : planets) { if (p.SpiceID == curr2) { curr2 = p.ParentID; f = true; break; } }
                            if (!f) break;
                        }
                        // --- BUG FIX #2: Also check remaining targets to find deepest common ancestor ---
                        for (size_t ti = 1; ti < navTargets.size(); ti++) {
                            int curr_t = navTargets[ti].spiceID;
                            int candidateParent = 10;
                            while (curr_t != 0) {
                                if (std::find(ances1.begin(), ances1.end(), curr_t) != ances1.end()) { candidateParent = curr_t; break; }
                                if (curr_t == 10) break;
                                bool f2 = false;
                                for (auto& p : planets) { if (p.SpiceID == curr_t) { curr_t = p.ParentID; f2 = true; break; } }
                                if (!f2) break;
                            }
                            // Keep the common ancestor that is LOWER in the hierarchy (i.e., closer to the bodies)
                            // Lower means smaller distance from Sun; prefer the one that is an ancestor of both
                            if (candidateParent != commonParent) {
                                // Check if commonParent is ancestor of candidateParent's target
                                // Simple: prefer Earth (399) over Sun (10) when possible
                                if (commonParent == 10 && candidateParent != 10) commonParent = candidateParent;
                            }
                        }
                        centralBodyIdx = commonParent;
                    }
                    // --- END AUTO DETECT ---
                    
                    CelestialBody* cBody = nullptr;
                    for (auto& p : planets) if (p.SpiceID == centralBodyIdx) cBody = &p;
                    
                    if (cBody) {
                        double mu = cBody->GM;
                        
                        // --- AUTO OPTIMIZE 2D GRID SEARCH (works for any number of segments) ---
                        // Optimizes the DEPARTURE timing and first-leg TOF for minimum total dV.
                        // For multi-segment missions, subsequent legs use user-specified TOFs.
                        if (autoOptimize) {
                            CelestialBody* tgt = nullptr;
                            for (auto& p : planets) if (p.SpiceID == navTargets[0].spiceID) tgt = &p;
                            
                            CelestialBody* scCenter = nullptr;
                            for (auto& p : planets) if (p.SpiceID == sc->initialCenterBodySpiceID) scCenter = &p;
                            
                            if (tgt) {
                                double base_et = start_et_time;
                                // Baseline calculation to find rough synodic and Hohmann targets
                                double state[6]; double lt;
                                spkgeo_c(tgt->SpiceID, base_et, "J2000", cBody->SpiceID, state, &lt);
                                glm::dvec3 target_pos(state[0], state[1], state[2]);
                                
                                // Always use initial state for planning (not current simulated position)
                                glm::dvec3 sc_pos = sc->InitialPositionKM;
                                if (scCenter && scCenter->SpiceID != cBody->SpiceID) {
                                    double scC[6]; spkgeo_c(scCenter->SpiceID, base_et, "J2000", cBody->SpiceID, scC, &lt);
                                    sc_pos += glm::dvec3(scC[0], scC[1], scC[2]);
                                }
                                
                                double r1_dist = glm::length(sc_pos);
                                double r2_dist = glm::length(target_pos);
                                double a = (r1_dist + r2_dist) / 2.0;
                                double T_h = 3.1415926535 * std::sqrt((a*a*a)/mu); // seconds
                                
                                double T1 = 2.0 * 3.1415926535 * std::sqrt((r1_dist*r1_dist*r1_dist)/mu);
                                double T2 = 2.0 * 3.1415926535 * std::sqrt((r2_dist*r2_dist*r2_dist)/mu);
                                double diff = std::abs(1.0/T1 - 1.0/T2);
                                double S = diff > 1e-12 ? (1.0 / diff) : T1; // Synodic
                                
                                double min_dv = 1e9;
                                double best_dep = startETOffsetDays;
                                double best_tof = T_h / 86400.0;
                                
                                double dep_step = (S / 86400.0) / 40.0; 
                                double tof_step = (T_h / 86400.0) / 40.0;
                                if (dep_step < 0.1) dep_step = 0.1;
                                if (tof_step < 0.1) tof_step = 0.1;
                                
                                double dep_max = (S/86400.0) + dep_step;
                                double sc_mu = mu;
                                if (scCenter) sc_mu = scCenter->GM;

                                // Phase Matching: Constrain departure scan to 1 orbital period if in parking orbit
                                if (r1_dist < 150000.0) {
                                    double v0_mag = glm::length(sc->InitialVelocityKMS);
                                    double eps = (v0_mag * v0_mag) / 2.0 - sc_mu / glm::length(sc->InitialPositionKM);
                                    if (eps < 0.0) {
                                        double sma = -sc_mu / (2.0 * eps);
                                        double T_orbit = 2.0 * 3.1415926535 * std::sqrt((sma*sma*sma)/sc_mu);
                                        // BUG FIX: Limit search to 2 orbits for parking orbits.
                                        // The optimizer uses Keplerian propagation (no J2). If we search up to 5 days, 
                                        // the actual N-body propagation will include J2 precession, causing massive plane mismatch.
                                        dep_max = (T_orbit / 86400.0) * 2.0;
                                        dep_step = (T_orbit / 86400.0) / 100.0;
                                    } else {
                                        dep_max = 0.0; // Hyperbolic bypass
                                    }
                                }
                                
                                for (double dep = 0; dep <= dep_max; dep += dep_step) {
                                    double testET = base_et + dep * 86400.0;
                                    
                                    // Use initial state for porkchop (Keplerian propagation from LEO)
                                    glm::dvec3 test_r = sc->InitialPositionKM;
                                    glm::dvec3 test_v = sc->InitialVelocityKMS;

                                    if (dep > 0.0) {
                                        test_r = PropagateKepler(sc->InitialPositionKM, sc->InitialVelocityKMS, dep * 86400.0, sc_mu, test_v);
                                    }

                                    if (scCenter && scCenter->SpiceID != cBody->SpiceID) {
                                        double stateC[6];
                                        spkgeo_c(scCenter->SpiceID, testET, "J2000", cBody->SpiceID, stateC, &lt);
                                        test_r += glm::dvec3(stateC[0], stateC[1], stateC[2]);
                                        test_v += glm::dvec3(stateC[3], stateC[4], stateC[5]);
                                    }
                                    
                                    // --- GENERALIZED TOF SEARCH WINDOW ---
                                    double hohmann_tof_days = T_h / 86400.0;
                                    double search_tof_min = hohmann_tof_days * 0.4;
                                    double search_tof_max = hohmann_tof_days * 1.5;
                                    
                                    // Adaptive resolution: further targets need more scan points
                                    double current_tof_steps = (hohmann_tof_days < 10.0) ? 40.0 : 120.0;
                                    tof_step = (search_tof_max - search_tof_min) / current_tof_steps;

                                    for (double tof_d = search_tof_min; tof_d <= search_tof_max; tof_d += tof_step) {
                                        double arrET = testET + tof_d * 86400.0;
                                        double stateT[6];
                                        spkgeo_c(tgt->SpiceID, arrET, "J2000", cBody->SpiceID, stateT, &lt);
                                        glm::dvec3 target_pos_center(stateT[0], stateT[1], stateT[2]);
                                        glm::dvec3 target_v(stateT[3], stateT[4], stateT[5]);

                                        // Lambert solver targeting the center of the target body (standard Patched Conics)
                                        glm::dvec3 target_r = target_pos_center;
                                        
                                        LambertResult lam = SolveLambert(test_r, target_r, tof_d * 86400.0, mu, cBody->SpiceID, true);
                                        if (lam.success) {
                                            // --- SCIENTIFIC dV METRIC: Patched Conics Gravity Well Awareness ---
                                            double current_dv = 0.0;
                                            if (scCenter && scCenter->SpiceID != cBody->SpiceID) {
                                                double stC[6], lt;
                                                spkgeo_c(scCenter->SpiceID, testET, "J2000", cBody->SpiceID, stC, &lt);
                                                glm::dvec3 planet_v(stC[3], stC[4], stC[5]);
                                                glm::dvec3 v_inf_vec = lam.v1 - planet_v;
                                                double v_inf = glm::length(v_inf_vec);
                                                glm::dvec3 r_rel = test_r - glm::dvec3(stC[0], stC[1], stC[2]);
                                                glm::dvec3 v_rel = test_v - planet_v;
                                                double r_mag = glm::length(r_rel);
                                                double v_req_at_r = std::sqrt(v_inf * v_inf + 2.0 * scCenter->GM / r_mag);
                                                double speed_diff = std::abs(v_req_at_r - glm::length(v_rel));
                                                double alignment = glm::dot(glm::normalize(v_rel), glm::normalize(v_inf_vec));
                                                current_dv = speed_diff + (1.0 - alignment) * 5.0; 
                                            } else {
                                                current_dv = glm::length(lam.v1 - test_v);
                                            }

                                            // Generalized dv2 metric: Arrival LOI cost based on energy conservation
                                            glm::dvec3 v_inf_vec_opt = lam.v2 - target_v;
                                            double v_inf_sq_opt = glm::dot(v_inf_vec_opt, v_inf_vec_opt);
                                            double r_peri_opt = tgt->RadiusKM + (double)navTargets[0].targetAltKm;
                                            double v_actual_opt = std::sqrt(v_inf_sq_opt + 2.0 * tgt->GM / r_peri_opt);
                                            double v_circ_opt   = std::sqrt(tgt->GM / r_peri_opt);
                                            double dv2 = std::abs(v_actual_opt - v_circ_opt);

                                            double total_score = current_dv;
                                            if (navTargets[0].objective != MissionObjective::Flyby) {
                                                total_score += dv2; // Only add insertion cost if we actually intend to stay in orbit
                                            }
                                            double trial_tof2 = 0.0;
                                            
                                            // --- GLOBAL SEQUENCE PREDICTION ---
                                            if (navTargets.size() > 1) {
                                                // 1. Predict flyby exit velocity at Target 1
                                                glm::dvec3 v_inf_in = lam.v2 - target_v;
                                                double v_inf_mag = glm::length(v_inf_in);
                                                double r_peri = tgt->RadiusKM + (double)navTargets[0].targetAltKm;
                                                double delta = 2.0 * std::asin(1.0 / (1.0 + (r_peri * v_inf_mag * v_inf_mag) / tgt->GM));
                                                glm::dvec3 rot_axis = glm::normalize(glm::cross(target_v, v_inf_in));
                                                if (glm::length(rot_axis) < 1e-6) rot_axis = glm::dvec3(0,0,1);
                                                glm::dmat4 rot_mat = glm::rotate(glm::dmat4(1.0), delta, rot_axis);
                                                glm::dvec3 v_inf_out = glm::dvec3(rot_mat * glm::dvec4(v_inf_in, 0.0));
                                                
                                                // 2. Predict starting state for the next leg (Leg 1)
                                                double r_soi = 66000.0; 
                                                glm::dvec3 leg1_r1 = target_pos_center + glm::normalize(v_inf_out) * r_soi;
                                                glm::dvec3 leg1_v1_start = target_v + v_inf_out;
                                                
                                                // 3. Search for best return TOF (TOF2) to find true free-returns
                                                CelestialBody* nextTarget = nullptr;
                                                for(auto& b : planets) if(b.SpiceID == navTargets[1].spiceID) nextTarget = &b;
                                                
                                                if (nextTarget) {
                                                    double best_leg1_dv = 10.0;
                                                    trial_tof2 = navTargets[1].tofDays;

                                                    for (double t2 = 2.0; t2 <= 15.0; t2 += 0.2) {
                                                        double tof2_sec = t2 * 86400.0;
                                                        double et_final = arrET + tof2_sec;
                                                        double stF[6], ltF;
                                                        spkgeo_c(nextTarget->SpiceID, et_final, "J2000", cBody->SpiceID, stF, &ltF);
                                                        glm::dvec3 leg1_r2(stF[0], stF[1], stF[2]);
                                                        
                                                        LambertResult lam2 = SolveLambert(leg1_r1, leg1_r2, tof2_sec, cBody->GM, cBody->SpiceID, true);
                                                        if (lam2.success) {
                                                            double dv_corr = glm::length(lam2.v1 - leg1_v1_start);
                                                            if (dv_corr < best_leg1_dv) {
                                                                best_leg1_dv = dv_corr;
                                                                trial_tof2 = t2;
                                                            }
                                                        }
                                                    }
                                                    total_score += best_leg1_dv;
                                                }
                                            }

                                            total_score += (tof_d / 10.0) * 0.05;
                                            if (total_score < min_dv) {
                                                min_dv = total_score;
                                                best_dep = dep;
                                                best_tof = tof_d;
                                                if (navTargets.size() > 1) navTargets[1].tofDays = (float)trial_tof2;
                                            }
                                        }
                                    }
                                }
                                if (min_dv < 1e8) {
                                    startETOffsetDays = best_dep;
                                    navTargets[0].tofDays = best_tof;
                                    AddLog("[NAV] Auto-Optimized: Departure Delayed by " + std::to_string(best_dep) + "d (Waiting for cheaper window), TOF=" + std::to_string(best_tof) + "d (Score=" + std::to_string(min_dv) + ")");
                                }
                            }
                        }
                        // --- END AUTO OPTIMIZE ---
                        
                        // Auto-sync spacecraft epoch to simulation start epoch
                        if (sc->epochET != start_et_time) sc->epochET = start_et_time;
                        if (sc->missionEpochET != start_et_time) sc->missionEpochET = start_et_time;

                        double base_et = start_et_time;
                        double currentET = base_et + startETOffsetDays * 86400.0;
                        
                        // CRITICAL FIX: Always use the spacecraft's INITIAL state for navigation planning.
                        // After the simulation runs (e.g. 13 days), sc->PositionKM reflects the current
                        // simulated position (near Moon, Moon-centered), NOT the initial LEO state.
                        // Using the current state causes ~8 km/s TLI instead of the correct ~3.1 km/s.
                        // InitialPositionKM and InitialVelocityKMS are the state at mission creation.
                        glm::dvec3 r_sc = sc->InitialPositionKM;
                        glm::dvec3 v_sc = sc->InitialVelocityKMS;
                        int navCenterBodyID = sc->initialCenterBodySpiceID;

                        // Phase shift the departure point using exact N-body physics (matches real simulation)
                        if (startETOffsetDays > 0.0) {
                            double wait_sec = startETOffsetDays * 86400.0;
                            double t_current = base_et;
                            double h_wait = 10.0; // Precise steps for parking orbit
                            int steps_wait = (int)std::ceil(wait_sec / h_wait);
                            h_wait = wait_sec / steps_wait;
                            
                            glm::dvec3 r_park = sc->InitialPositionKM;
                            glm::dvec3 v_park = sc->InitialVelocityKMS;
                            
                            CelestialBody* sCen = nullptr;
                            for (auto& p : planets) if (p.SpiceID == sc->initialCenterBodySpiceID) sCen = &p;

                            if (sCen) {
                                // Match the real simulation physics (J2 + N-Body) exactly for the wait period
                                for (int s = 0; s < steps_wait; ++s) {
                                    auto get_acc_park = [&](glm::dvec3 p, double et) {
                                        double rm = glm::length(p);
                                        glm::dvec3 a = -sCen->GM * p / (rm*rm*rm);
                                        
                                        // J2 Perturbation
                                        if (sCen->J2 > 1e-9) {
                                            double R_mat[3][3]; std::string iau = "IAU_" + std::string(sCen->Name);
                                            for(auto &c: iau) c=toupper(c); pxform_c(iau.c_str(), "J2000", et, R_mat);
                                            glm::dmat3 rot = glm::dmat3(R_mat[0][0],R_mat[1][0],R_mat[2][0],R_mat[0][1],R_mat[1][1],R_mat[2][1],R_mat[0][2],R_mat[1][2],R_mat[2][2]);
                                            glm::dvec3 pl = glm::transpose(rot) * p; double r2=rm*rm; double r5=r2*r2*rm;
                                            double j2f = -1.5*sCen->J2*sCen->GM*sCen->RadiusKM*sCen->RadiusKM/r5;
                                            a += rot * glm::dvec3(j2f*pl.x*(1.0-5.0*pl.z*pl.z/r2), j2f*pl.y*(1.0-5.0*pl.z*pl.z/r2), j2f*pl.z*(3.0-5.0*pl.z*pl.z/r2));
                                        }
                                        
                                        // N-body perturbations from other bodies
                                        for (auto& b : planets) {
                                            if (b.SpiceID == sCen->SpiceID) continue;
                                            double stB[6], lt; spkgeo_c(b.SpiceID, et, "J2000", sCen->SpiceID, stB, &lt);
                                            glm::dvec3 rb(stB[0], stB[1], stB[2]);
                                            glm::dvec3 r_rel = p - rb;
                                            double d_mag = glm::length(r_rel);
                                            double rb_mag = glm::length(rb);
                                            a += -b.GM * (r_rel / (d_mag * d_mag * d_mag) + rb / (rb_mag * rb_mag * rb_mag));
                                        }
                                        return a;
                                    };
                                    
                                    // Classic RK4 Integration
                                    glm::dvec3 k1v = get_acc_park(r_park, t_current); glm::dvec3 k1r = v_park;
                                    glm::dvec3 k2v = get_acc_park(r_park + k1r*(h_wait/2.0), t_current + h_wait/2.0); glm::dvec3 k2r = v_park + k1v*(h_wait/2.0);
                                    glm::dvec3 k3v = get_acc_park(r_park + k2r*(h_wait/2.0), t_current + h_wait/2.0); glm::dvec3 k3r = v_park + k2v*(h_wait/2.0);
                                    glm::dvec3 k4v = get_acc_park(r_park + k3r*h_wait, t_current + h_wait); glm::dvec3 k4r = v_park + k3v*h_wait;
                                    
                                    v_park += (h_wait/6.0)*(k1v + 2.0*k2v + 2.0*k3v + k4v); 
                                    r_park += (h_wait/6.0)*(k1r + 2.0*k2r + 2.0*k3r + k4r); 
                                    t_current += h_wait;
                                }
                                r_sc = r_park;
                                v_sc = v_park;
                            } else {
                                // Fallback: If center body not found, return to Kepler
                                r_sc = PropagateKepler(sc->PositionKM, sc->VelocityKMS, startETOffsetDays * 86400.0, cBody->GM, v_sc);
                            }
                        }
                        
                        // Convert to absolute relative to selected Central Body
                        // Use initialCenterBodySpiceID (not current centerBodySpiceID) because we started from
                        // the initial state above, which is relative to the initial center body.
                        CelestialBody* scCenter = nullptr;
                        for (auto& p : planets) if (p.SpiceID == navCenterBodyID) scCenter = &p;
                        if (scCenter && scCenter->SpiceID != cBody->SpiceID) {
                             double scC[6]; double lt;
                             spkgeo_c(scCenter->SpiceID, currentET, "J2000", cBody->SpiceID, scC, &lt);
                             r_sc += glm::dvec3(scC[0], scC[1], scC[2]);
                             v_sc += glm::dvec3(scC[3], scC[4], scC[5]);
                        }
                        
                        glm::dvec3 current_r = r_sc;
                        glm::dvec3 current_v = v_sc; // Before burn
                        double current_t = currentET;
                        
                        bool ok = true;
                        for (size_t i = 0; i < navTargets.size(); i++) {
                            CelestialBody* targetBody = nullptr;
                            for (auto& p : planets) if (p.SpiceID == navTargets[i].spiceID) targetBody = &p;
                            
                            if (targetBody) {
                                double tof_sec = navTargets[i].tofDays * 86400.0;
                                
                                // --- AUTO TOF: If user left TOF=0, compute a Hohmann estimate ---
                                if (tof_sec <= 0.0) {
                                    // Get current target body position to estimate distance
                                    double stateEst[6]; double ltEst;
                                    spkgeo_c(targetBody->SpiceID, current_t, "J2000", cBody->SpiceID, stateEst, &ltEst);
                                    glm::dvec3 tgt_est(stateEst[0], stateEst[1], stateEst[2]);
                                    
                                    double r1_est = glm::length(current_r);
                                    double r2_est = glm::length(tgt_est);
                                    
                                    // If target IS the central body (r2≈0), use current_r as r2 estimate
                                    if (r2_est < targetBody->RadiusKM * 2.0) r2_est = r1_est * 0.5;
                                    
                                    double a_h = (r1_est + r2_est) / 2.0;
                                    double mu_est = cBody->GM;
                                    if (mu_est < 1.0) mu_est = 398600.4418; // Earth GM fallback
                                    tof_sec = 3.14159265358979 * std::sqrt((a_h * a_h * a_h) / mu_est);
                                    
                                    // Clamp to reasonable range (1 hour to 60 days)
                                    tof_sec = std::max(3600.0, std::min(tof_sec, 60.0 * 86400.0));
                                    navTargets[i].tofDays = (float)(tof_sec / 86400.0);
                                    AddLog("[NAV] Segment " + std::to_string(i) + ": Auto TOF = " + std::to_string((int)(tof_sec/3600.0)) + " hours (" + std::to_string(navTargets[i].tofDays) + " days)");
                                }
                                
                                double arrival_t = current_t + tof_sec;
                                
                                double state[6]; double lt;
                                spkgeo_c(targetBody->SpiceID, arrival_t, "J2000", cBody->SpiceID, state, &lt);
                                glm::dvec3 target_pos_center(state[0], state[1], state[2]);
                                glm::dvec3 target_v(state[3], state[4], state[5]);

                                // --- BUG FIX #3: Target == Central Body (e.g. Earth impact in Earth-centered frame) ---
                                // spkgeo_c(399,...,"J2000",399,...) returns (0,0,0) which breaks Lambert.
                                // Fix: aim at a point on the target surface in the approach direction.
                                if (glm::length(target_pos_center) < targetBody->RadiusKM * 2.0) {
                                    // Spacecraft approaches from current_r direction; target surface point opposite to it
                                    glm::dvec3 approach_dir = (glm::length(current_r) > 1.0)
                                        ? -glm::normalize(current_r)
                                        : glm::dvec3(1.0, 0.0, 0.0);
                                    double aim_r = targetBody->RadiusKM + (double)navTargets[i].targetAltKm;
                                    if (aim_r < targetBody->RadiusKM * 1.01) aim_r = targetBody->RadiusKM * 1.01;
                                    target_pos_center = approach_dir * aim_r;
                                    AddLog("[NAV] Target is central body - aiming at surface point " + std::to_string((int)aim_r) + " km from center");
                                }

                                AddLog("[NAV-DBG] Target pos (CB frame): X=" + std::to_string((int)target_pos_center.x) + " Y=" + std::to_string((int)target_pos_center.y) + " Z=" + std::to_string((int)target_pos_center.z) + " km");
                                AddLog("[NAV-DBG] Dep pos (CB frame): X=" + std::to_string((int)current_r.x) + " Y=" + std::to_string((int)current_r.y) + " Z=" + std::to_string((int)current_r.z) + " km | |v|=" + std::to_string(glm::length(current_v)));

                                // 1. Get Initial Guess from Lambert
                                // Virtual Pilot periapsis/impact target distance
                                // For Impact: targetAltKm = 0 means surface (radius only)
                                double r_peri_target = targetBody->RadiusKM + (double)navTargets[i].targetAltKm;
                                if (navTargets[i].objective == MissionObjective::Impact && navTargets[i].targetAltKm <= 0.0f) {
                                    r_peri_target = targetBody->RadiusKM; // Surface impact
                                }
                                {


                                LambertResult lambert = SolveLambert(current_r, target_pos_center, tof_sec, cBody->GM, cBody->SpiceID, true);
                                if (!lambert.success) lambert = SolveLambert(current_r, target_pos_center, tof_sec, cBody->GM, cBody->SpiceID, false);
                                
                                if (lambert.success) {
                                    glm::dvec3 dv_inertial = lambert.v1 - current_v;

                                    // --- BUG FIX #5 (CORRECTED): VNB reference body per segment ---
                                    // For segment 0: use spacecraft's center body (e.g. Earth) as VNB reference.
                                    // For segment i>0: use cBody (central body) — NOT previous target!
                                    //   Reason: after flyby, current_r == previous_target_pos,
                                    //   so r_rel_vp = current_r - prev_target = (0,0,0) → NaN in cross product.
                                    CelestialBody* vnbRef = nullptr;
                                    if (i == 0) {
                                        vnbRef = scCenter ? scCenter : cBody;
                                    } else {
                                        // Use central body for all subsequent segments to avoid r=0 issue
                                        vnbRef = cBody;
                                    }
                                    glm::dvec3 v_rel_vp = current_v;
                                    glm::dvec3 r_rel_vp = current_r;
                                    
                                    if (vnbRef->SpiceID != cBody->SpiceID) {
                                        double stC[6], lt;
                                        spkgeo_c(vnbRef->SpiceID, current_t, "J2000", cBody->SpiceID, stC, &lt);
                                        r_rel_vp = current_r - glm::dvec3(stC[0], stC[1], stC[2]);
                                        v_rel_vp = current_v - glm::dvec3(stC[3], stC[4], stC[5]);
                                    }

                                    // NaN guard: if r_rel_vp is near-zero (e.g. SC at body center after flyby),
                                    // fall back to inertial frame relative to cBody
                                    if (glm::length(r_rel_vp) < 1.0) {
                                        r_rel_vp = current_r;
                                        v_rel_vp = current_v;
                                    }

                                    glm::dvec3 V = glm::normalize(v_rel_vp);
                                    glm::dvec3 cross_rv = glm::cross(r_rel_vp, v_rel_vp);
                                    glm::dvec3 N = (glm::length(cross_rv) > 1e-10)
                                        ? glm::normalize(cross_rv)
                                        : glm::dvec3(0, 0, 1);
                                    glm::dvec3 B = glm::cross(V, N);

                                    double dv_v = glm::dot(dv_inertial, V);
                                    double dv_n = glm::dot(dv_inertial, N);
                                    double dv_b = glm::dot(dv_inertial, B);

                                    double actual_peri_v = 0.0;

                                    // Single-objective Virtual Flight: optimize Moon periapsis only.
                                    // Earth return comes naturally from flyby bending geometry.
                                    auto runVirtualFlight = [&](double dvv, double dvn, double dvb, double& out_v) {
                                        glm::dvec3 v_start = current_v + (V * dvv + N * dvn + B * dvb);
                                        glm::dvec3 r = current_r; glm::dvec3 v = v_start;
                                        double t = current_t;
                                        double h_base = 10.0;
                                        double elapsed_t = 0.0;
                                        double min_dist = 1e18;
                                        bool isImpactMode = (navTargets[i].objective == MissionObjective::Impact);

                                        double flight_duration = tof_sec + 86400.0 * 2.0; // 2 days padding for delayed arrivals
                                        while (elapsed_t < flight_duration) {
                                            double stT[6], lt; spkgeo_c(targetBody->SpiceID, t, "J2000", cBody->SpiceID, stT, &lt);
                                            glm::dvec3 r_tgt(stT[0], stT[1], stT[2]);
                                            glm::dvec3 v_tgt(stT[3], stT[4], stT[5]);
                                            double d_to_target = glm::length(r - r_tgt);

                                            // --- IMPACT SURFACE DETECTION ---
                                            // If spacecraft is already inside the body, stop. Surface was already crossed.
                                            if (isImpactMode && d_to_target <= targetBody->RadiusKM) {
                                                min_dist = d_to_target;
                                                out_v = glm::length(v - v_tgt);
                                                break;
                                            }

                                            double actual_h = h_base;
                                            // Finer steps near the surface (Impact: 1s, others: 10s)
                                            if (d_to_target < r_peri_target * 5.0)
                                                actual_h = isImpactMode ? std::min(h_base, 1.0) : std::min(h_base, 10.0);
                                            if (elapsed_t + actual_h > flight_duration) actual_h = flight_duration - elapsed_t;
                                            if (actual_h < 1e-6) break;

                                            auto get_acc = [&](glm::dvec3 p, double et) {
                                                double rm = glm::length(p);
                                                glm::dvec3 a = -cBody->GM * p / (rm*rm*rm);
                                                for (auto& b : planets) {
                                                    if (b.SpiceID == cBody->SpiceID) continue;
                                                    double stB[6], local_lt; spkgeo_c(b.SpiceID, et, "J2000", cBody->SpiceID, stB, &local_lt);
                                                    glm::dvec3 rb(stB[0], stB[1], stB[2]);
                                                    glm::dvec3 r_rel = p - rb;
                                                    double d_mag = glm::length(r_rel), rb_mag = glm::length(rb);
                                                    if (d_mag > 1.0 && rb_mag > 1.0)
                                                        a += -b.GM * (r_rel/(d_mag*d_mag*d_mag) + rb/(rb_mag*rb_mag*rb_mag));
                                                }
                                                if (cBody->J2 > 1e-9) {
                                                    double R_mat[3][3]; std::string iau = "IAU_" + std::string(cBody->Name);
                                                    for(auto &c: iau) c=toupper(c); pxform_c(iau.c_str(), "J2000", et, R_mat);
                                                    glm::dmat3 rot = glm::dmat3(R_mat[0][0],R_mat[1][0],R_mat[2][0],R_mat[0][1],R_mat[1][1],R_mat[2][1],R_mat[0][2],R_mat[1][2],R_mat[2][2]);
                                                    glm::dvec3 pl = glm::transpose(rot) * p; double r2=rm*rm; double r5=r2*r2*rm;
                                                    double j2f = -1.5*cBody->J2*cBody->GM*cBody->RadiusKM*cBody->RadiusKM/r5;
                                                    a += rot * glm::dvec3(j2f*pl.x*(1.0-5.0*pl.z*pl.z/r2), j2f*pl.y*(1.0-5.0*pl.z*pl.z/r2), j2f*pl.z*(3.0-5.0*pl.z*pl.z/r2));
                                                }
                                                return a;
                                            };

                                            glm::dvec3 k1v=get_acc(r,t), k1r=v;
                                            glm::dvec3 k2v=get_acc(r+k1r*(actual_h/2.0),t+actual_h/2.0), k2r=v+k1v*(actual_h/2.0);
                                            glm::dvec3 k3v=get_acc(r+k2r*(actual_h/2.0),t+actual_h/2.0), k3r=v+k2v*(actual_h/2.0);
                                            glm::dvec3 k4v=get_acc(r+k3r*actual_h,t+actual_h), k4r=v+k3v*actual_h;
                                            v += (actual_h/6.0)*(k1v+2.0*k2v+2.0*k3v+k4v);
                                            r += (actual_h/6.0)*(k1r+2.0*k2r+2.0*k3r+k4r);
                                            t += actual_h; elapsed_t += actual_h;

                                            // Post-step distance check (catches surface crossing within the step)
                                            double d_post = glm::length(r - r_tgt);
                                            if (isImpactMode && d_post <= targetBody->RadiusKM) {
                                                min_dist = d_post;
                                                out_v = glm::length(v - v_tgt);
                                                break;
                                            }
                                            if (d_post < min_dist) {
                                                min_dist = d_post;
                                                out_v = glm::length(v - v_tgt);
                                            }
                                        }
                                        return min_dist;
                                    };

                                    // Single-Objective Jacobian Shooting: converge to target periapsis
                                    bool isImpactSeg = (navTargets[i].objective == MissionObjective::Impact);
                                    double last_dist = 0;
                                    double trash_v;
                                    for (int iter = 0; iter < 12; ++iter) {
                                        double d0 = runVirtualFlight(dv_v, dv_n, dv_b, actual_peri_v);
                                        last_dist = d0;
                                        double err = d0 - r_peri_target;

                                        // Impact success: spacecraft hits the surface (d0 <= body radius)
                                        if (isImpactSeg && d0 <= r_peri_target) {
                                            AddLog("[PILOT] Iter " + std::to_string(iter) + ": IMPACT at " + std::to_string((int)d0) + " km from center — surface confirmed.");
                                            break;
                                        }
                                        if (std::abs(err) < 0.1) break;

                                        double eps = 1e-4;
                                        double ddv = (runVirtualFlight(dv_v+eps,dv_n,dv_b,trash_v) - d0)/eps;
                                        double ddn = (runVirtualFlight(dv_v,dv_n+eps,dv_b,trash_v) - d0)/eps;
                                        double ddb = (runVirtualFlight(dv_v,dv_n,dv_b+eps,trash_v) - d0)/eps;

                                        double grad_mag = ddv*ddv + ddn*ddn + ddb*ddb;
                                        if (grad_mag > 1e-18) {
                                            double step = err / grad_mag;
                                            // 4. Update guess with stability cap
                                            double max_adj = 0.5; // km/s max adjustment per iteration
                                            double adj_v = std::clamp(step * ddv, -max_adj, max_adj);
                                            dv_v -= adj_v * 0.8;
                                            
                                            double adj_n = std::clamp(step * ddn, -max_adj, max_adj);
                                            dv_n -= adj_n * 0.8;
                                            
                                            double adj_b = std::clamp(step * ddb, -max_adj, max_adj);
                                            dv_b -= adj_b * 0.8;
                                        }
                                        AddLog("[PILOT] Iter " + std::to_string(iter) + ": Periapsis=" + std::to_string((int)d0) + " km (Err: " + std::to_string((int)err) + " km)");
                                    }
                                    AddLog("[NAV] Virtual Pilot Converged at " + std::to_string((int)last_dist) + " km radius.");
                                    glm::dvec3 dv = (V * dv_v + N * dv_n + B * dv_b);









                                    
                                    double get_sec = current_t - sc->missionEpochET;
                                    if (get_sec < 0.0) get_sec = 0.0;
                                    int h = (int)(get_sec / 3600);
                                    int m = (int)((get_sec - h*3600)/60);
                                    double s = get_sec - h*3600 - m*60;
                                    
                                    // --- Departure Burn (TLI / Transfer) ---
                                    BurnEntry b;
                                    b.trigger = TriggerType::GET;
                                    b.get_h = h; b.get_m = m; b.get_s = s;
                                    b.apsisType = 0; b.altCondition = 0;
                                    b.altRefBodyID = cBody->SpiceID; b.targetAltKM = 0.0;
                                    b.refBodyID = cBody->SpiceID;
                                    b.enabled = true;

                                    // ALWAYS USE VNB FOR DEPARTURES (use per-segment vnbRef computed above)
                                    {
                                        // vnbRef already computed per segment above (Bug #5 fix)
                                        double stC[6], lt;
                                        spkgeo_c(vnbRef->SpiceID, current_t, "J2000", cBody->SpiceID, stC, &lt);
                                        glm::dvec3 planet_v(stC[3], stC[4], stC[5]);
                                        glm::dvec3 planet_r(stC[0], stC[1], stC[2]);
                                        
                                        // RELATIVE vectors for VNB frame definition
                                        glm::dvec3 v_rel = current_v - planet_v;
                                        glm::dvec3 r_rel = current_r - planet_r;
                                        
                                        // NaN guard: if r_rel is near-zero, use inertial frame
                                        if (glm::length(r_rel) < 1.0) { r_rel = current_r; v_rel = current_v; }
                                        
                                        // The Delta-V in inertial J2000 (Calculated by Virtual Pilot)
                                        glm::dvec3 dv_inertial = dv; 

                                        glm::dvec3 V_axis = glm::normalize(v_rel);
                                        glm::dvec3 cross_rv2 = glm::cross(r_rel, v_rel);
                                        glm::dvec3 N_axis = (glm::length(cross_rv2) > 1e-10) ? glm::normalize(cross_rv2) : glm::dvec3(0,0,1);
                                        glm::dvec3 B_axis = glm::cross(V_axis, N_axis);

                                        // Project the inertial dV onto the spacecraft's local VNB frame
                                        b.dvx = glm::dot(dv_inertial, V_axis);
                                        b.dvy = glm::dot(dv_inertial, B_axis);
                                        b.dvz = glm::dot(dv_inertial, N_axis);
                                        b.isVNB = true;
                                        b.refBodyID = vnbRef->SpiceID;
                                        
                                        AddLog("[NAV-DBG] Final VNB Components: V=" + std::to_string(b.dvx) + 
                                               " B=" + std::to_string(b.dvy) + " N=" + std::to_string(b.dvz));
                                    }
                                    g_BurnTable.push_back(b);


                                    // Advance state to arrival
                                    // FIX: Use actual target body position (not B-plane offset) for multi-segment chaining
                                    current_r = target_pos_center;
                                    current_v = lambert.v2;
                                    current_t = arrival_t;

                                    // --- Arrival Burn depending on objective ---
                                    auto obj = navTargets[i].objective;

                                    if (obj == MissionObjective::OrbitInsertion) {
                                        // LOI: retrograde burn at periapsis to circularize at target altitude
                                        double target_mu = targetBody->GM;
                                        double target_radius = targetBody->RadiusKM;
                                        double r_peri = target_radius + (double)navTargets[i].targetAltKm;

                                        // BUG FIX 2: Use actual periapsis velocity from Virtual Pilot for precise insertion
                                        double v_circ = std::sqrt(target_mu / r_peri);
                                        double dv_loi_mag_val = actual_peri_v - v_circ;

                                        double get_loi = arrival_t - sc->missionEpochET;
                                        int hl = (int)(get_loi / 3600);
                                        int ml = (int)((get_loi - hl*3600)/60);
                                        double sl = get_loi - hl*3600 - ml*60;

                                        BurnEntry loi;
                                        loi.trigger = TriggerType::APSIS;
                                        loi.apsisType = 1; // periapsis
                                        loi.get_h = hl; loi.get_m = ml; loi.get_s = sl;
                                        loi.dvx = -dv_loi_mag_val; // Negative V component = Retrograde
                                        loi.dvy = 0.0;
                                        loi.dvz = 0.0;
                                        loi.altCondition = 0; loi.altRefBodyID = targetBody->SpiceID;
                                        loi.targetAltKM = (double)navTargets[i].targetAltKm;
                                        loi.refBodyID = targetBody->SpiceID; 
                                        loi.isVNB = true; 
                                        loi.enabled = true;

                                        // --- SCIENTIFIC SOI SAFETY GATE ---
                                        double target_soi = -1.0; 
                                        if (targetBody->ParentID != 0) {
                                            double parentGM = 0.0;
                                            for (auto& p : planets) if (p.SpiceID == targetBody->ParentID) parentGM = p.GM;
                                            if (parentGM > 0) {
                                                double d_p = glm::length(targetBody->PositionKM - targetBody->ParentPositionKM);
                                                target_soi = d_p * std::pow(targetBody->GM / parentGM, 0.4);
                                            }
                                        }
                                        
                                        if (target_soi > 0) {
                                            BurnEntry soi_wait;
                                            soi_wait.trigger = TriggerType::ALTITUDE;
                                            soi_wait.altCondition = 1; // <=
                                            soi_wait.altRefBodyID = targetBody->SpiceID;
                                            
                                            // Convert SOI radius to surface altitude
                                            double safe_altitude = (target_soi * 0.1) - targetBody->RadiusKM;
                                            if (safe_altitude < 500.0) safe_altitude = 500.0;
                                            
                                            soi_wait.targetAltKM = safe_altitude; 
                                            soi_wait.refBodyID = 0; // Coast
                                            soi_wait.dvx = 0; soi_wait.dvy = 0; soi_wait.dvz = 0;
                                            soi_wait.enabled = true;
                                            g_BurnTable.push_back(soi_wait);
                                        }

                                        g_BurnTable.push_back(loi);

                                        AddLog("[NAV] LOI burn generated (VNB Retrograde) with SOI Gate: dV=" + std::to_string(dv_loi_mag_val)
                                            + " km/s at " + std::to_string((int)navTargets[i].targetAltKm) + " km alt over " + targetBody->Name);

                                    } else if (obj == MissionObjective::Flyby) {
                                        AddLog("[NAV] Segment " + std::to_string(i) + ": Flyby of " + targetBody->Name);
                                        
                                        // BUG FIX 3: Flyby Vector Bending (Gravity Assist)
                                        // 1. Calculate incoming v_inf
                                        glm::dvec3 v_inf_in = lambert.v2 - target_v; 
                                        double v_inf_mag = glm::length(v_inf_in);
                                        
                                        // 2. Calculate turning angle (delta) for gravity assist
                                        double mu_target = targetBody->GM;
                                        double r_peri = targetBody->RadiusKM + (double)navTargets[i].targetAltKm;
                                        double delta = 2.0 * std::asin(1.0 / (1.0 + (r_peri * v_inf_mag * v_inf_mag) / mu_target));
                                        
                                        // 3. Find rotation axis (normal to orbit plane)
                                        glm::dvec3 rot_axis = glm::normalize(glm::cross(target_v, v_inf_in)); 
                                        if (glm::length(rot_axis) < 1e-6) rot_axis = glm::dvec3(0, 0, 1);
                                        
                                        // 4. Create rotation matrix and calculate outgoing v_inf
                                        glm::dmat4 rot_mat = glm::rotate(glm::dmat4(1.0), delta, rot_axis);
                                        glm::dvec3 v_inf_out = glm::dvec3(rot_mat * glm::dvec4(v_inf_in, 0.0));
                                        
                                        // 5. Patched Conic: Shift starting point for next leg to SOI exit ONLY if there is a next leg
                                        // and we are leaving a non-central body (like the Moon).
                                        if (targetBody->SpiceID != cBody->SpiceID && i < navTargets.size() - 1) {
                                            double r_soi = 66000.0; // Moon SOI fallback
                                            if (targetBody->ParentID != 0) {
                                                double parentGM = 0.0;
                                                for(auto& p: planets) if(p.SpiceID == targetBody->ParentID) parentGM = p.GM;
                                                if (parentGM > 0) {
                                                    double d_p = glm::length(targetBody->PositionKM - targetBody->ParentPositionKM);
                                                    r_soi = d_p * std::pow(targetBody->GM / parentGM, 0.4);
                                                }
                                            }
                                            
                                            glm::dvec3 v_inf_out_hat = glm::normalize(v_inf_out);
                                            current_r = target_pos_center + v_inf_out_hat * r_soi;
                                            current_v = target_v + v_inf_out;
                                            
                                            // Simple TOF adjustment for SOI exit
                                            double time_to_exit = r_soi / std::max(0.001, v_inf_mag);
                                            current_t += time_to_exit;

                                            AddLog("[NAV] Patched Conic: Leg " + std::to_string(i+1) + " starts from SOI exit of " + targetBody->Name);
                                        } else {
                                            // Chaining to central body or last segment
                                            current_v = target_v + v_inf_out;
                                            // current_r stays at target_pos_center (standard approximation)
                                        }

                                    } else if (obj == MissionObjective::Impact) {
                                        // No arrival burn — let it hit the surface
                                        AddLog("[NAV] Segment " + std::to_string(i) + ": Impact trajectory to " + targetBody->Name
                                            + " (target alt=" + std::to_string((int)navTargets[i].targetAltKm) + " km)");
                                    }

                                } else {
                                    AddLog("[NAV] Lambert Solver failed for segment " + std::to_string(i)
                                        + " (r1=" + std::to_string(glm::length(current_r))
                                        + " km, r2=" + std::to_string(glm::length(target_pos_center))
                                        + " km, tof=" + std::to_string(navTargets[i].tofDays) + " d)");
                                    ok = false;
                                    break;
                                }
                                } // end segment block
                            }
                        }
                        if (ok) AddLog("[NAV] Mission Segments Generated!");
                    }
                } else {
                    AddLog("[WARN] Please select spacecraft and add at least one target.");
                }
            }
        } else if (openedFile == "OutputScript.esmat") {
            ImGui::TextColored(ImVec4(0.0f, 0.35f, 0.7f, 1.0f), "OutputScript.esmat");
            ImGui::Separator();

            static char scriptBuf[8192] = {};
            static std::string lastSyncedScript = "";
            if (lastSyncedScript != g_ScriptText) {
                strncpy(scriptBuf, g_ScriptText.c_str(), sizeof(scriptBuf)-1);
                scriptBuf[sizeof(scriptBuf)-1] = '\0';
                lastSyncedScript = g_ScriptText;
            }

            // Count lines for line numbers
            int lineCount2 = 1;
            for (int i = 0; scriptBuf[i] != '\0'; i++)
                if (scriptBuf[i] == '\n') lineCount2++;
            if (lineCount2 < 12) lineCount2 = 12;

            float totalBtnH = ImGui::GetFrameHeightWithSpacing() + 6.0f;
            float edH2 = ImGui::GetContentRegionAvail().y - totalBtnH;
            if (edH2 < 40.0f) edH2 = 40.0f;

            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1,1,1,1));
            ImGui::BeginChild("ScriptEditorScroll", ImVec2(-FLT_MIN, edH2), true,
                              ImGuiWindowFlags_HorizontalScrollbar);

            // Left pane: line numbers
            ImGui::BeginGroup();
            for (int i = 1; i <= lineCount2; i++)
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%3d", i);
            ImGui::EndGroup();
            ImGui::SameLine();

            // Right pane: text editor
            float scriptEdH = ImGui::GetTextLineHeight() * lineCount2
                              + ImGui::GetStyle().FramePadding.y * 2.0f;
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1,1,1,0));
            ImGui::InputTextMultiline("##scripteditor", scriptBuf, sizeof(scriptBuf),
                ImVec2(-FLT_MIN, scriptEdH), ImGuiInputTextFlags_AllowTabInput);
            ImGui::PopStyleColor();

            ImGui::EndChild();
            ImGui::PopStyleColor();

            float hw2 = ImGui::GetContentRegionAvail().x * 0.5f - 4.0f;
            if (ImGui::Button("Save Script", ImVec2(hw2, 0))) {
                g_ScriptText = scriptBuf;
                lastSyncedScript = g_ScriptText;
                if (!g_Project.currentProjectPath.empty()) {
                    std::ofstream sf(g_Project.currentProjectPath + "\\OutputScript.esmat");
                    if (sf.is_open()) sf << g_ScriptText;
                }
                AddLog("[Script] Saved OutputScript.esmat.");
            }
            ImGui::SameLine();
            if (ImGui::Button("Test Parse", ImVec2(-FLT_MIN, 0))) {
                OutputScript tmp;
                if (tmp.parse(std::string(scriptBuf), &planets, &spacecrafts)) {
                    AddLog("[Script] OK - " + std::to_string(tmp.plots.size()) +
                           " plot(s), " + std::to_string(tmp.reports.size()) + " report(s).");
                } else {
                    AddLog("[Script] Parse error: " + tmp.lastError);
                }
            }
        } else if (openedFile == "Solar Wind Data") {
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "\xF0\x9F\x8C\xAC Solar Wind Editor");
            ImGui::Separator();
            ImGui::Spacing();

            // ── New addition row ────────────────────────────────────────────
            static char newWindName[128] = "MySolarWind";
            ImGui::SetNextItemWidth(180);
            ImGui::InputText("##newWindName", newWindName, 128);
            ImGui::SameLine();
            if (ImGui::Button("+ Add Uniform")) {
                bool exists = false;
                for (const auto& w : projectSolarWinds)
                    if (w.Name == std::string(newWindName)) { exists = true; break; }
                if (!exists && strlen(newWindName) > 0) {
                    SolarWindField sw;
                    sw.Name = std::string(newWindName);
                    projectSolarWinds.push_back(sw);
                    AddLog("[OK] Uniform solar wind added: " + sw.Name);
                } else AddLog("[ERROR] This name already exists or is empty.");
            }
            ImGui::SameLine();
            // ── ENLIL 4D import button ──────────────────────────────────────
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.5f, 0.8f, 0.9f));
            if (ImGui::Button("\xF0\x9F\x8C\x90  Load ENLIL Data (.esmatwind)")) {
                // Windows file open dialog
                char fileBuf[MAX_PATH] = "";
                OPENFILENAMEA ofn = {};
                ofn.lStructSize = sizeof(ofn);
                ofn.lpstrFilter = "ESMAT Wind Files\0*.esmatwind\0All Files\0*.*\0";
                ofn.lpstrFile   = fileBuf;
                ofn.nMaxFile    = MAX_PATH;
                ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                ofn.lpstrTitle  = "Select ENLIL 4D Solar Wind File";
                if (GetOpenFileNameA(&ofn)) {
                    SolarWindField sw;
                    if (sw.load4DGrid(std::string(fileBuf))) {
                        // Update if same name exists, else add
                        bool replaced = false;
                        for (auto& existing : projectSolarWinds) {
                            if (existing.Name == sw.Name) {
                                existing = sw;
                                replaced = true;
                                break;
                            }
                        }
                        if (!replaced) projectSolarWinds.push_back(sw);
                        AddLog("[OK] ENLIL 4D loaded: " + sw.Name
                               + "  " + sw.infoString());
                    } else {
                        AddLog("[ERROR] Cannot read .esmatwind file: " + std::string(fileBuf));
                    }
                }
            }
            ImGui::PopStyleColor();

            ImGui::Spacing();
            ImGui::Separator();

            // ── Existing wind list ──────────────────────────────────────────
            for (size_t i = 0; i < projectSolarWinds.size(); i++) {
                auto& sw = projectSolarWinds[i];
                std::string tag = sw.isDataLoaded ? "[ENLIL 4D]" : "[Uniform]";
                std::string header = "\xF0\x9F\x8C\x80 " + sw.Name + "  " + tag;
                ImGui::PushID((int)i);
                bool nodeOpen = ImGui::CollapsingHeader(header.c_str());
                if (nodeOpen) {
                    bool changed = false;

                    if (sw.isDataLoaded) {
                        // ENLIL 4D info (read-only)
                        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.5f, 1.0f),
                            "ENLIL Grid: nt=%d  nr=%d  nphi=%d",
                            sw.grid.nt, sw.grid.nr, sw.grid.nphi);
                            
                        if (sw.grid.nt > 0) {
                            double start_et = sw.grid.refdate_unix + sw.grid.t_s.front() - SolarWindField::ET_TO_UNIX;
                            double end_et   = sw.grid.refdate_unix + sw.grid.t_s.back() - SolarWindField::ET_TO_UNIX;
                            char startStr[32], endStr[32];
                            timout_c(start_et, "YYYY-MM-DD", 32, startStr);
                            timout_c(end_et,   "YYYY-MM-DD", 32, endStr);
                            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "Date Range: %s to %s", startStr, endStr);
                        }
                        if (sw.grid.nr > 0) {
                            double r_min_au = sw.grid.r_m.front() / (1000.0 * 149597870.7);
                            double r_max_au = sw.grid.r_m.back()  / (1000.0 * 149597870.7);
                            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "Radial Scope: %.2f AU to %.2f AU", r_min_au, r_max_au);
                        }
                            
                        ImGui::TextDisabled("Temperature (uniform fallback):");
                        ImGui::SetNextItemWidth(160);
                        changed |= ImGui::InputDouble("Temperature (eV)##temp4d", &sw.temperatureEV, 0.0, 0.0, "%.3f");
                        ImGui::TextDisabled("n, v interpolation based on pos and time is active.");
                    } else {
                        // Uniform parameters
                        ImGui::TextDisabled("Velocity Vector (km/s, J2000 inertial)");
                        ImGui::PushItemWidth(110);
                        changed |= ImGui::InputDouble("Vx##vel", &sw.velocityKMS.x, 0.0, 0.0, "%.2f");
                        ImGui::SameLine();
                        changed |= ImGui::InputDouble("Vy##vel", &sw.velocityKMS.y, 0.0, 0.0, "%.2f");
                        ImGui::SameLine();
                        changed |= ImGui::InputDouble("Vz##vel", &sw.velocityKMS.z, 0.0, 0.0, "%.2f");
                        ImGui::PopItemWidth();
                        ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f),
                            "  |v| = %.2f km/s", sw.getSpeed());
                        ImGui::Spacing();
                        ImGui::SetNextItemWidth(160);
                        changed |= ImGui::InputDouble("Density (cm^-3)##dens", &sw.density, 0.0, 0.0, "%.3f");
                        ImGui::SetNextItemWidth(160);
                        changed |= ImGui::InputDouble("Temperature (eV)##temp", &sw.temperatureEV, 0.0, 0.0, "%.3f");
                    }

                    if (changed) {
                        for (auto& sc : spacecrafts) {
                            if (auto es = std::dynamic_pointer_cast<ElectricSail>(sc)) {
                                if (es->attachedWindName == sw.Name) es->updateCharAccel();
                            }
                        }
                    }

                    ImGui::Spacing();
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.1f, 0.1f, 0.8f));
                    if (ImGui::Button("Delete")) {
                        projectSolarWinds.erase(projectSolarWinds.begin() + i);
                        ImGui::PopStyleColor();
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopStyleColor();
                }
                ImGui::PopID();
            }
        } else if (openedFile == "SimulationSettings.cfg") {
            if (g_Project.currentProjectPath.empty()) {
                ImGui::Text("Editing: %s (Unsaved Project)", openedFile.c_str());
            } else {
                ImGui::Text("Editing: %s", g_Project.currentProjectPath.c_str());
            }
            ImGui::Separator();
            
            ImGui::TextDisabled("// Simulation configuration parameters");
            ImGui::Spacing();
            
            // IDE Style Text Editor
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            ImGui::BeginChild("EditorScroll", ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 18), true, ImGuiWindowFlags_HorizontalScrollbar);
            
            // Count lines
            int lineCount = 1;
            for (int i = 0; g_Project.configText[i] != '\0'; i++) {
                if (g_Project.configText[i] == '\n') lineCount++;
            }
            if (lineCount < 16) lineCount = 16;
            
            // Left Pane: Line Numbers
            ImGui::BeginGroup();
            for (int i = 1; i <= lineCount; i++) {
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%3d", i);
            }
            ImGui::EndGroup();
            
            ImGui::SameLine();
            
            // Right Pane: Text Editor
            float editorHeight = ImGui::GetTextLineHeight() * lineCount + ImGui::GetStyle().FramePadding.y * 2.0f;
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.0f, 1.0f, 1.0f, 0.0f)); // Transparent frame so it blends with child bg
            ImGui::InputTextMultiline("##config_editor", g_Project.configText, sizeof(g_Project.configText), ImVec2(-FLT_MIN, editorHeight), ImGuiInputTextFlags_AllowTabInput);
            ImGui::PopStyleColor();
            
            ImGui::EndChild();
            ImGui::PopStyleColor();
            
            ImGui::Spacing();
            if (ImGui::Button(isSimulationInitialized ? "APPLY CONFIG & RESTART" : "START SIMULATION", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                
                // Clear the strings before parsing to detect missing keys
                startDate[0] = '\0';
                endDate[0] = '\0';
                
                ParseConfigText();
                
                // Validate all required attributes
                std::string sDate(startDate);
                std::string eDate(endDate);
                std::string cfg(g_Project.configText);
                bool hasStepSize = (cfg.find("STEP_SIZE_SEC=") != std::string::npos);
                
                if (sDate == "" || eDate == "") {
                    AddLog("[ERROR] START_DATE and END_DATE are required in config.");
                    isSimulationInitialized = false;
                    isSimulating = false;
                } else if (!isValidSpiceDate(sDate) || !isValidSpiceDate(eDate)) {
                    AddLog("[ERROR] Dates must match YYYY-MM-DDTHH:MM:SS format.");
                    isSimulationInitialized = false;
                    isSimulating = false;
                } else if (!hasStepSize) {
                    AddLog("[ERROR] STEP_SIZE_SEC is required in config. Add: STEP_SIZE_SEC=86400.0");
                    isSimulationInitialized = false;
                    isSimulating = false;
                } else {
                    str2et_c(startDate, &start_et_time);
                    et_time = start_et_time;
                    globalSimEt = start_et_time; // Full reset of global time tracker
                    str2et_c(endDate, &end_et_time);
                    checkSpiceErrors();
                    if (!failed_c()) {
                        isSimulationInitialized = true;
                        isSimulating = true;
                        isSimulationFinished = false; // Reset the finished state so it starts fresh!
                        
                        // Reset output script for fresh run
                        g_Script.clear();
                        g_Script.setSimStart(start_et_time);
                        if (g_Script.parse(g_ScriptText, &planets)) {
                            g_Script.initForSim(g_Project.currentProjectPath);
                            AddLog("[Script] Parsed " + std::to_string(g_Script.plots.size()) + " plot(s), " + std::to_string(g_Script.reports.size()) + " report(s).");
                        } else {
                            AddLog("[Script] Parse error: " + g_Script.lastError);
                        }
                        
                        // Clear all existing orbit trails on simulation restart and reset lastTrailEt tracker
                        for (auto& planet : planets) {
                            if (planet.orbitRenderer) {
                                planet.orbitRenderer->updatePoints(std::vector<TrailPoint>());
                                planet.orbitRenderer->lastTrailEt = start_et_time;
                            }
                        }
                        
                        // Reset spacecrafts and propagate to new start time if date changed
                        for (auto& sc : spacecrafts) {
                            sc->reset(); // Restores InitialPositionKM at missionEpochET
                            
                            // If the simulation start date differs from the spacecraft's mission epoch,
                            // propagate it forward using full N-body + J2 physics (same as main sim loop).
                            // This makes the spacecraft appear at its correct orbital position on the new date,
                            // consistent with how SPICE provides planet/moon positions at the new date.
                            double dt_offset = start_et_time - sc->missionEpochET;
                            if (std::abs(dt_offset) > 1.0) {
                                const double h_prop = 60.0; // 60-second steps (fast & accurate for LEO)
                                double direction = (dt_offset > 0.0) ? 1.0 : -1.0;
                                double remaining = std::abs(dt_offset);
                                double t_prop = sc->missionEpochET;
                                
                                AddLog("[SC-INIT] Propagating " + sc->Name + " by " +
                                       std::to_string((int)(dt_offset / 86400.0)) + " days via N-body...");
                                
                                // Save planet positions so we can restore them after propagation
                                std::vector<glm::dvec3> savedPos(planets.size());
                                std::vector<glm::dvec3> savedVel(planets.size());
                                for (int pi = 0; pi < (int)planets.size(); ++pi) {
                                    savedPos[pi] = planets[pi].PositionKM;
                                    savedVel[pi] = planets[pi].VelocityKMS;
                                }
                                
                                while (remaining > 0.0) {
                                    double step = std::min(h_prop, remaining);
                                    double t_step = t_prop + direction * step;
                                    
                                    // Update all planet positions from SPICE at this time (same as main sim loop)
                                    for (auto& p : planets) {
                                        if (p.SpiceID != 10) {
                                            SpiceDouble st[6], lt;
                                            spkezr_c(std::to_string(p.SpiceID).c_str(), t_step, "J2000", "NONE", "10", st, &lt);
                                            if (!failed_c()) {
                                                p.PositionKM  = glm::dvec3(st[0], st[1], st[2]);
                                                p.VelocityKMS = glm::dvec3(st[3], st[4], st[5]);
                                            } else {
                                                reset_c();
                                            }
                                        }
                                    }
                                    
                                    // Advance spacecraft with full N-body + J2 (RK4)
                                    sc->updatePhysics(direction * step, planets);
                                    t_prop = t_step;
                                    remaining -= step;
                                }
                                
                                // Restore planet positions to what they were before this loop
                                for (int pi = 0; pi < (int)planets.size(); ++pi) {
                                    planets[pi].PositionKM  = savedPos[pi];
                                    planets[pi].VelocityKMS = savedVel[pi];
                                }
                                
                                AddLog("[SC-INIT] Done. Pos: (" +
                                    std::to_string((int)sc->PositionKM.x) + ", " +
                                    std::to_string((int)sc->PositionKM.y) + ", " +
                                    std::to_string((int)sc->PositionKM.z) + ") km");

                                // Update Initial* to propagated state - mission planning uses these
                                sc->InitialPositionKM        = sc->PositionKM;
                                sc->InitialVelocityKMS       = sc->VelocityKMS;
                                sc->initialCenterBodySpiceID = sc->centerBodySpiceID;
                                sc->missionEpochET           = start_et_time;
                            }
                            sc->epochET = start_et_time;
                        }
                        
                        AddLog("Config applied. Restarting from " + std::string(startDate));
                    }
                }
            }
        } else if (openedFile.find(".esmatobj") != std::string::npos && selectedBody) {
            // Update arcball target constantly as the planet moves
            if (camera.isArcball) {
                float renderRadius = (selectedBody->SpiceID == 10)
                    ? selectedBody->RadiusKM * (float)scaleFactor * (planetSizeExaggeration / 10.0f)
                    : selectedBody->RadiusKM * (float)scaleFactor * planetSizeExaggeration;
                // Minimum distance: always stay outside the sphere with a comfortable margin
                camera.MinDistance = (std::max)(renderRadius * 1.5f, 1e-7f);
                
                if (camera.Distance < camera.MinDistance) {
                    camera.Distance = camera.MinDistance;
                }
            }

            ImGui::Text("Viewing Data: %s", openedFile.c_str());
            ImGui::Separator();
            
            // Read .esmatobj file contents to show as read-only
            static std::string currentLoadedObjFile = "";
            static std::string objFileContent = "";
            std::string expectedObjPath = g_Project.currentProjectPath + "\\SolarSystem\\" + openedFile;
            
            if (currentLoadedObjFile != expectedObjPath && !g_Project.currentProjectPath.empty()) {
                currentLoadedObjFile = expectedObjPath;
                std::ifstream f(expectedObjPath);
                if (f.is_open()) {
                    objFileContent = std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                    f.close();
                } else {
                    objFileContent = "Error: Could not load " + expectedObjPath;
                }
            }
            
            if (!g_Project.currentProjectPath.empty()) {
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.95f, 0.95f, 0.95f, 1.0f));
                ImGui::BeginChild("ObjEditorScroll", ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 10), true, ImGuiWindowFlags_HorizontalScrollbar);
                
                int lineCount = 1;
                for (size_t i = 0; i < objFileContent.length(); i++) {
                    if (objFileContent[i] == '\n') lineCount++;
                }
                if (lineCount < 6) lineCount = 6;
                
                ImGui::BeginGroup();
                for (int i = 1; i <= lineCount; i++) {
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%3d", i);
                }
                ImGui::EndGroup();
                ImGui::SameLine();
                
                float editorHeight = ImGui::GetTextLineHeight() * lineCount + ImGui::GetStyle().FramePadding.y * 2.0f;
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::InputTextMultiline("##obj_viewer", (char*)objFileContent.c_str(), objFileContent.capacity() + 1, ImVec2(-FLT_MIN, editorHeight), ImGuiInputTextFlags_ReadOnly);
                ImGui::PopStyleColor();
                
                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::Spacing();
            }

            ImGui::Text("body_name = \"%s\"", selectedBody->Name.c_str());
            ImGui::Text("spice_id  = %d", selectedBody->SpiceID);
            ImGui::Text("radius_km = %.2f", selectedBody->RadiusKM);
            ImGui::Text("j2_val    = %.10f", selectedBody->J2);
            
            ImGui::Spacing();
            ImGui::TextDisabled("// Texture Map");
            if (selectedBody->HasTexture) {
                ImGui::Text("Texture: %s", selectedBody->TexturePath.c_str());
            } else {
                ImGui::Text("Texture: None");
            }
            if (ImGui::Button("Load Texture...")) {
                std::string path = OpenFileDialog("Image Files (*.png;*.jpg;*.jpeg;*.bmp)\\0*.png;*.jpg;*.jpeg;*.bmp\\0All Files (*.*)\\0*.*\\0", "Select Texture Image");
                if (!path.empty()) {
                    unsigned int texID = LoadTexture(path.c_str());
                    if (texID > 0) {
                        if (selectedBody->HasTexture) glDeleteTextures(1, &selectedBody->TextureID); // Free old
                        selectedBody->TextureID = texID;
                        selectedBody->HasTexture = true;
                        selectedBody->TexturePath = path;
                    }
                }
            }
            if (selectedBody->HasTexture) {
                ImGui::SameLine();
                if (ImGui::Button("Remove Texture")) {
                    glDeleteTextures(1, &selectedBody->TextureID);
                    selectedBody->HasTexture = false;
                    selectedBody->TextureID = 0;
                    selectedBody->TexturePath = "";
                }
            }
            ImGui::Spacing();
            if (selectedBody->ParentID != 10 && selectedBody->ParentID != 0) {
                ImGui::TextDisabled("// J2000 Ephemeris (relative to parent %d)", selectedBody->ParentID);
            } else {
                ImGui::TextDisabled("// J2000 Ephemeris (relative to Sun)");
            }
            ImGui::Text("pos_x = %.2f", selectedBody->RelativePositionKM.x);
            ImGui::Text("pos_y = %.2f", selectedBody->RelativePositionKM.y);
            ImGui::Text("pos_z = %.2f", selectedBody->RelativePositionKM.z);
            ImGui::Text("vel   = %.4f", glm::length(selectedBody->RelativeVelocityKMS));
            
            if (selectedBody->SpiceID != 10) {
                const double AU = 149597870.7;
                double distKM    = glm::length(selectedBody->RelativePositionKM);
                double distSunKM = glm::length(selectedBody->PositionKM);
                ImGui::Separator();
                ImGui::TextDisabled("// Distance from parent");
                ImGui::Text("dist_km     = %.0f", distKM);
                ImGui::Text("dist_au     = %.6f", distKM / AU);
                ImGui::TextDisabled("// Distance from Sun");
                ImGui::Text("dist_sun_km = %.0f", distSunKM);
                ImGui::Text("dist_sun_au = %.6f", distSunKM / AU);
                ImGui::Text("vel_sun     = %.4f", glm::length(selectedBody->VelocityKMS));
                
                ImGui::Separator();
                ImGui::TextDisabled("// Keplerian Elements");
                double parent_mu = 0.0;
                for (const auto& p : planets) {
                    if (p.SpiceID == selectedBody->ParentID) {
                        parent_mu = p.GM;
                        break;
                    }
                }
                if (parent_mu > 0.0) {
                    double sma, ecc, inc, raan, argp, nu;
                    SyncKeplerianFromCartesian(selectedBody->RelativePositionKM, selectedBody->RelativeVelocityKMS, parent_mu, et_time, sma, ecc, inc, raan, argp, nu);
                    ImGui::Text("raan_deg    = %.4f", raan);
                    ImGui::Text("inc_deg     = %.4f", inc);
                    ImGui::Text("ecc         = %.6f", ecc);
                    ImGui::Text("sma_km      = %.2f", sma);
                    ImGui::Text("argp_deg    = %.4f", argp);
                    ImGui::Text("true_anom   = %.4f", nu);
                } else {
                    ImGui::Text("raan_deg    = N/A");
                }
            }
            

            ImGui::Separator();
            ImGui::TextDisabled("// Orbital Period");
            if (selectedBody->orbitPeriodSeconds > 0) {
                double days = selectedBody->orbitPeriodSeconds / 86400.0;
                ImGui::Text("period_days  = %.4f", days);
                ImGui::Text("period_years = %.6f", days / 365.25);
            } else if (selectedBody->orbitPeriodSeconds < 0) {
                ImGui::Text("period_days  = inf  // Escape trajectory");
            }
            
            ImGui::Spacing();
            ImGui::TextDisabled("// PCK Rotation Matrix (J2000 to IAU_%s)", ToUpper(selectedBody->Name).c_str());
            ImGui::Text("[ %6.3f, %6.3f, %6.3f ]", selectedBody->currentRotationMatrix[0][0], selectedBody->currentRotationMatrix[1][0], selectedBody->currentRotationMatrix[2][0]);
            ImGui::Text("[ %6.3f, %6.3f, %6.3f ]", selectedBody->currentRotationMatrix[0][1], selectedBody->currentRotationMatrix[1][1], selectedBody->currentRotationMatrix[2][1]);
            ImGui::Text("[ %6.3f, %6.3f, %6.3f ]", selectedBody->currentRotationMatrix[0][2], selectedBody->currentRotationMatrix[1][2], selectedBody->currentRotationMatrix[2][2]);
            
            ImGui::Spacing();
            ImGui::ColorEdit3("Body & Trail Color", glm::value_ptr(selectedBody->Color));
            
            ImGui::Spacing();
            ImGui::Checkbox("Show Orbit Trail", &selectedBody->showOrbit);
            ImGui::SameLine();
            ImGui::Checkbox("Render Body", &selectedBody->showBody);
            
            ImGui::Spacing();
            if (ImGui::Button("FOCUS CAMERA", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                currentOriginBodyName = selectedBody->Name;
                currentOriginSpacecraftName = "";
                float renderRadius = (selectedBody->SpiceID == 10)
                    ? selectedBody->RadiusKM * (float)scaleFactor * (planetSizeExaggeration / 10.0f)
                    : selectedBody->RadiusKM * (float)scaleFactor * planetSizeExaggeration;
                camera.MinDistance = (std::max)(renderRadius * 1.5f, 1e-7f);
                float dist = (std::max)(renderRadius * 12.0f, camera.MinDistance);
                
                camera.SetArcballTarget(glm::vec3(0.0f), dist);
                camera.Yaw = -90.0f;
                camera.Pitch = 45.0f; 
                camera.ProcessMouseMovement(0, 0); 
            }
        } else if (openedFile.find(".esmatspacecraft") != std::string::npos) {
            Spacecraft* sc = nullptr;
            for (auto& s : spacecrafts) {
                if (s->Name == selectedSpacecraftName) { sc = s.get(); break; }
            }

            if (sc) {
                bool isESail = (dynamic_cast<ElectricSail*>(sc) != nullptr);
                ImGui::Text("Spacecraft: %s", sc->Name.c_str());
                
                ImGui::BeginDisabled(true);
                ImGui::InputInt("Spice ID (For Scripts)", &sc->ID);
                ImGui::EndDisabled();
                
                ImGui::Separator();
                
                // Find center body name
                std::string centerName = "Earth";
                CelestialBody* centerBody = nullptr;
                for (auto& p : planets) { if (p.SpiceID == sc->centerBodySpiceID) { centerBody = &p; centerName = p.Name; break; } }

                ImGui::TextDisabled("// State (Inertial J2000 %s-centered)", centerName.c_str());
                ImGui::Text("pos_x = %.2f km", sc->PositionKM.x);
                ImGui::Text("pos_y = %.2f km", sc->PositionKM.y);
                ImGui::Text("pos_z = %.2f km", sc->PositionKM.z);
                ImGui::Text("vel   = %.4f km/s", glm::length(sc->VelocityKMS));
                
                ImGui::Separator();
                if (centerBody) {
                    double dist = glm::length(sc->PositionKM);
                    ImGui::Text("Alt (%s rel) = %.2f km", centerName.c_str(), dist - centerBody->RadiusKM);
                    ImGui::Text("Dist from %s = %.2f km", centerName.c_str(), dist);

                    // --- Heliocentric State ---
                    {
                        constexpr double AU_KM = 149597870.7;
                        // Find Sun body
                        const CelestialBody* sunBody = nullptr;
                        for (const auto& p : planets) { if (p.SpiceID == 10) { sunBody = &p; break; } }

                        // Absolute position and velocity (SSB/Sun frame)
                        glm::dvec3 absPos = centerBody->PositionKM + sc->PositionKM;
                        glm::dvec3 absVel = centerBody->VelocityKMS + sc->VelocityKMS;

                        // Distance from Sun (subtract Sun pos if Sun is not at origin)
                        glm::dvec3 sunPos = sunBody ? sunBody->PositionKM : glm::dvec3(0.0);
                        double dist_sun_km = glm::length(absPos - sunPos);
                        double dist_sun_au = dist_sun_km / AU_KM;

                        // Heliocentric velocity (relative to Sun's velocity)
                        glm::dvec3 sunVel = sunBody ? sunBody->VelocityKMS : glm::dvec3(0.0);
                        double vel_helio = glm::length(absVel - sunVel);

                        ImGui::Separator();
                        ImGui::TextDisabled("// Heliocentric State  [ESMATCraft(%d).attr]", sc->ID);
                        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "vel_sun     = %.4f km/s", vel_helio);
                        ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f),  "dist_sun_au = %.6f AU",   dist_sun_au);
                        ImGui::Text(                                          "dist_sun_km = %.0f km",   dist_sun_km);
                    }
                    
                    // --- Orbital Elements (Keplerian) ---
                    double mu = centerBody->GM; // km^3/s^2
                    if (mu > 0.0) {
                        glm::dvec3 r = sc->PositionKM;
                        glm::dvec3 v = sc->VelocityKMS;
                        double r_mag = glm::length(r);
                        double v_mag = glm::length(v);
                        double v2    = v_mag * v_mag;
                        
                        // Specific orbital energy (vis-viva)
                        double eps = v2 / 2.0 - mu / r_mag;
                        
                        // Semi-major axis
                        double a = (eps < 0.0) ? (-mu / (2.0 * eps)) : std::numeric_limits<double>::infinity();
                        
                        // Eccentricity vector: e = (1/mu)*((v²  - mu/r)*r - (r·v)*v)
                        double rdotv = glm::dot(r, v);
                        glm::dvec3 e_vec = (1.0 / mu) * ((v2 - mu / r_mag) * r - rdotv * v);
                        double ecc = glm::length(e_vec);
                        
                        // Orbital period (only meaningful for elliptic orbits)
                        double period_s = (eps < 0.0 && a > 0.0) ? (2.0 * 3.14159265358979 * sqrt(a * a * a / mu)) : -1.0;
                        
                        ImGui::Separator();
                        ImGui::TextDisabled("// Keplerian Elements");
                        ImGui::Text("SMA (a)  = %.2f km", a);
                        ImGui::Text("Ecc (e)  = %.6f", ecc);
                        if (period_s > 0.0) {
                            if (period_s < 3600.0)
                                ImGui::Text("Period   = %.1f s", period_s);
                            else
                                ImGui::Text("Period   = %.4f h", period_s / 3600.0);
                        } else {
                            ImGui::Text("Period   = N/A (hyperbolic)");
                        }
                    }
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.0f, 0.4f, 0.8f, 1.0f), "\xEF\x9B\xB1 Initial State (%s-centered)", centerName.c_str());
                
                ImGui::BeginDisabled(true);
                ImGui::SetNextItemWidth(150);
                if (ImGui::DragScalarN("Init Pos (KM)", ImGuiDataType_Double, &sc->InitialPositionKM.x, 3, 10.0f)) {
                    if (!isSimulating) sc->PositionKM = sc->InitialPositionKM;
                }
                ImGui::SetNextItemWidth(150);
                if (ImGui::DragScalarN("Init Vel (KMS)", ImGuiDataType_Double, &sc->InitialVelocityKMS.x, 3, 0.01f)) {
                    if (!isSimulating) sc->VelocityKMS = sc->InitialVelocityKMS;
                }
                ImGui::EndDisabled();

                // --- Epoch Strings for UI ---
                char epochStr[128];
                timout_c(sc->epochET, "YYYY-MON-DD HR:MN:SC ::UTC", 128, epochStr);
                ImGui::Separator();
                ImGui::Text("State Epoch (UTC): %s", epochStr);
                
                char missionEpochStr[128];
                timout_c(sc->missionEpochET, "YYYY-MON-DD HR:MN:SC ::UTC", 128, missionEpochStr);
                ImGui::Text("Mission Start Epoch (GET=0 UTC): %s", missionEpochStr);
                

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::ColorEdit3("Spacecraft Color##sc", glm::value_ptr(sc->Color));
                ImGui::Checkbox("Show Trajectory", &sc->showTrajectory);
                ImGui::SameLine();
                ImGui::Checkbox("Show Model", &sc->showBody);

                if (auto es = dynamic_cast<ElectricSail*>(sc)) {
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f), "\xE2\x9A\xA1 Electric Sail Editor");
                    
                    if (ImGui::Button("VIEW PHYSICS PARAMETERS", ImVec2(-1, 40))) {
                        showEsailParametersWindow = true;
                    }
                }

                ImGui::Spacing();
            if (ImGui::Button("FOCUS CAMERA", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                currentOriginSpacecraftName = sc->Name;
                currentOriginBodyName = "";
                    // Focus on spacecraft with a small distance
                    camera.MinDistance = 1e-7f; 
                    camera.SetArcballTarget(glm::vec3(0.0f), 0.001f);
                    camera.Yaw = -90.0f;
                }
            }
        } else {
            ImGui::TextDisabled("No file opened.");
        }
        ImGui::End();

        if (showEsailParametersWindow) {
            ImGui::Begin("\xE2\x9A\xA1 Electric Sail Physics Parameters (View Only)", &showEsailParametersWindow);
            if (selectedSpacecraftName != "") {
                Spacecraft* selectedSc = nullptr;
                for (auto& s : spacecrafts) {
                    if (s->Name == selectedSpacecraftName) { selectedSc = s.get(); break; }
                }
                if (auto es = dynamic_cast<ElectricSail*>(selectedSc)) {
                    // --- Header Panel ---
                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "Spacecraft: %s [3D E-Sail Physics Engine]", es->Name.c_str());
                    ImGui::Separator();
                    
                    ImGui::BeginChild("EsailParamsScroll", ImVec2(0, -110), true);

                    // --- SECTION A: SPIN & GEOMETRY ---
                    if (ImGui::CollapsingHeader("\xE2\x9A\x99 Spin & Geometry", ImGuiTreeNodeFlags_DefaultOpen)) {
                        ImGui::BeginDisabled(true); 
                        ImGui::TextDisabled("Physical construction and rotation dynamics.");
                        double tempLen = es->esailLengthKM;
                        if (ImGui::DragScalar("Tether Length (km)", ImGuiDataType_Double, &tempLen, 0.1f)) {
                            es->updateTetherLengthWithAdiabaticInvariant(tempLen);
                        }
                        ImGui::DragInt("Tether Count", &es->esailTetherCount, 1, 4, 300);
                        
                        double raDeg = glm::degrees(es->esailSpinRA);
                        if (ImGui::DragScalar("Spin Axis RA (deg)", ImGuiDataType_Double, &raDeg, 0.5f)) es->esailSpinRA = glm::radians(raDeg);
                        
                        double decDeg = glm::degrees(es->esailSpinDEC);
                        if (ImGui::DragScalar("Spin Axis DEC (deg)", ImGuiDataType_Double, &decDeg, 0.5f)) es->esailSpinDEC = glm::radians(decDeg);
                        
                        ImGui::DragScalar("Spin Rate (RPM)", ImGuiDataType_Double, &es->spinRateRPM, 0.01f);
                        ImGui::DragScalar("Flatness/Deflection (deg)", ImGuiDataType_Double, &es->esailDeflectionAngleDeg, 0.1f);
                        ImGui::EndDisabled();
                        ImGui::Spacing();
                    }

                    // --- SECTION B: ENVIRONMENT ---
                    if (ImGui::CollapsingHeader("\xF0\x9F\x8C\x80 Environment", ImGuiTreeNodeFlags_DefaultOpen)) {
                        ImGui::BeginDisabled(true);
                        ImGui::TextDisabled("Plasma models and distance scaling.");
                        ImGui::Spacing();
                        
                        ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f), "Solar Wind Data (Local Position):");
                        ImGui::Text("Attached Wind: %s", es->attachedWindName.empty() ? "None" : es->attachedWindName.c_str());
                        ImGui::Text("Density (n):   %.2f cm^-3", es->esailSolarWindDensity);
                        ImGui::Text("Speed (v):     %.2f km/s", es->esailSolarWindSpeedKMS);
                        ImGui::Text("Temp (T_e):    %.2f eV", es->esailSolarWindTempEV);
                        
                        ImGui::EndDisabled();
                        ImGui::Spacing();
                    }

                    // --- SECTION C: MEOE TANI (Diagnostics) ---
                    if (ImGui::CollapsingHeader("\xF0\x9F\x93\x8A MEOE State (Diagnostics)")) {
                        ImGui::TextDisabled("Current Modified Equinoctial Orbital Elements.");
                        ImGui::Value("p (km)", (float)es->meoe_p);
                        ImGui::Value("f", (float)es->meoe_f);
                        ImGui::Value("g", (float)es->meoe_g);
                        ImGui::Value("h", (float)es->meoe_h);
                        ImGui::Value("k", (float)es->meoe_k);
                        ImGui::Value("L (rad)", (float)es->meoe_L);
                        ImGui::Spacing();
                    }

                    ImGui::EndChild();
                    
                    ImGui::Separator();
                    es->updateCharAccel(); // Recalculate based on current SW and settings
                    
                    // --- Status & Diagnostics ---
                    ImVec4 statusCol = (es->controllerStatus == "NOMINAL") ? ImVec4(0,1,0,1) : ImVec4(1,0.5f,0,1);
                    ImGui::Text("Status: "); ImGui::SameLine();
                    ImGui::TextColored(statusCol, "%s", es->controllerStatus.c_str());
                    
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Nominal Char Accel (a_c): %.6f mm/s^2", es->esailCharAccelCalc);

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::TextDisabled("E-Sail Thrust (gravity excluded):");

                    double thrustN  = glm::length(es->esailNetForceN);
                    double thrustMN = thrustN * 1000.0;                              // mN
                    double thrustAccMM = (es->esailMassKG > 0.0)
                        ? (thrustN / es->esailMassKG) * 1000.0 : 0.0;               // mm/s²

                    ImVec4 thrustCol = (thrustN > 0.001) ? ImVec4(0.2f,1.0f,0.5f,1.0f)
                                                         : ImVec4(0.6f,0.6f,0.6f,1.0f);
                    ImGui::TextColored(thrustCol, "  Thrust:       %.4f mN  (%.6f mm/s^2)", thrustMN, thrustAccMM);
                    ImGui::Text("  Components:   X=%.4f  Y=%.4f  Z=%.4f  N",
                        es->esailNetForceN.x, es->esailNetForceN.y, es->esailNetForceN.z);
                    ImGui::Text("  Distance:     %.4f AU", es->esailDistSunAU);
                    ImGui::Text("  Pitch Angle:  %.2f deg", es->esailPitchAngleDeg);


                    if (ImGui::Button("Close", ImVec2(-1, 30))) showEsailParametersWindow = false;
                } else {
                    ImGui::TextDisabled("Selected spacecraft is not an Electric Sail.");
                }
            } else {
                ImGui::TextDisabled("No spacecraft selected.");
            }
            ImGui::End();
        }

        // 4. Console
        ImGui::Begin("Console Output");
        for (const auto& log : consoleLogs) {
            ImGui::TextUnformatted(log.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
        ImGui::End();

        // 4b. Output Panel — File Browser (one .esmatplot per plot)
        ImGui::Begin("Output");
        {
            if (!isProjectLoaded) {
                ImGui::TextDisabled("No project loaded.");
            } else if (g_Script.plots.empty() && g_Script.reports.empty()) {
                ImGui::TextDisabled("No outputs defined.");
                ImGui::Spacing();
                ImGui::TextDisabled("Edit  OutputScript.esmat  in the Explorer");
                ImGui::TextDisabled("to add plot() and report() commands,");
                ImGui::TextDisabled("then restart the simulation.");
            } else {
                // ---- Plot files ----
                for (auto& pd : g_Script.plots) {
                    std::string fileLabel = "\xF0\x9F\x93\x88 " + pd.title + ".esmatplot";
                    bool sel = pd.isOpen;
                    if (ImGui::Selectable(fileLabel.c_str(), sel)) {
                        pd.isOpen = !pd.isOpen;
                    }
                    if (!pd.xValues.empty()) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(%d pts)", (int)pd.xValues.size());
                    }
                }
                // ---- Report files ----
                for (auto& rd : g_Script.reports) {
                    std::string rLabel = "\xF0\x9F\x93\x91 " + rd.filename;
                    ImGui::TextColored(ImVec4(0.5f,0.7f,0.3f,1.0f), "%s", rLabel.c_str());
                }
            }
        }
        ImGui::End();

        // --- Floating Plot Windows (one per PlotDef with isOpen=true) ---
        for (auto& pd : g_Script.plots) {
            if (!pd.isOpen) continue;
            std::string winId = pd.title + "##plotwin";
            ImGui::SetNextWindowSize(ImVec2(640.0f, 420.0f), ImGuiCond_FirstUseEver);
            if (ImGui::Begin(winId.c_str(), &pd.isOpen,
                             ImGuiWindowFlags_NoSavedSettings)) {
                // Header info
                ImGui::TextDisabled("X: %s   |   %zu Y-channel(s)   |   %d samples",
                    pd.xAxis.legend().c_str(), pd.lines.size(), (int)pd.xValues.size());
                ImGui::SameLine();

                // Export CSV button
                if (ImGui::SmallButton("Export CSV...")) {
                    OPENFILENAMEA ofn;
                    char csvPath[MAX_PATH] = {};
                    std::string defName = pd.title + ".csv";
                    strncpy(csvPath, defName.c_str(), sizeof(csvPath)-1);
                    ZeroMemory(&ofn, sizeof(ofn));
                    ofn.lStructSize = sizeof(ofn);
                    ofn.lpstrFile   = csvPath;
                    ofn.nMaxFile    = sizeof(csvPath);
                    ofn.lpstrFilter = "CSV Files (*.csv)\0*.csv\0All Files\0*.*\0";
                    ofn.lpstrTitle  = "Export Plot as CSV";
                    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
                    if (GetSaveFileNameA(&ofn)) {
                        if (pd.exportCSV(csvPath))
                            AddLog("[Output] Exported: " + std::string(csvPath));
                        else
                            AddLog("[Output] ERROR: Could not write CSV.");
                    }
                }
                ImGui::Separator();

                // Plot area
                ImVec2 plotSz = ImGui::GetContentRegionAvail();
                if (plotSz.y < 80.0f) plotSz.y = 80.0f;

                if (pd.xValues.empty()) {
                    ImGui::TextDisabled("No data yet. Run the simulation to populate this plot.");
                } else if (ImPlot::BeginPlot(("##plt_" + pd.title).c_str(), plotSz)) {
                    ImPlot::SetupAxis(ImAxis_X1, pd.xlabel.empty() ? pd.xAxis.legend().c_str() : pd.xlabel.c_str());
                    ImPlot::SetupAxis(ImAxis_Y1, pd.ylabel.empty() ? "Value" : pd.ylabel.c_str());
                    if (pd.xAxis.isDateAxis())
                        ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
                    for (size_t i = 0; i < pd.lines.size(); ++i) {
                        auto& l = pd.lines[i];
                        if (l.yValues.empty()) continue;
                        const auto& yv = l.yValues;
                        const auto& xv = pd.xValues;
                        
                        std::string label = l.label.empty() ? l.yAxis.legend() : l.label;

                        ImPlot::PlotLine(
                            label.c_str(),
                            xv.data(),
                            yv.data(),
                            (int)yv.size(),
                            { ImPlotProp_LineColor, (l.color.a >= 0.0f) ? ImVec4(l.color.r, l.color.g, l.color.b, l.color.a) : ImVec4(0,0,0,-1) }
                        );

                        if (pd.showExtrema) {
                            // Find max and min indices for this line
                            size_t maxIdx = 0, minIdx = 0;
                            for (size_t k = 1; k < yv.size(); ++k) {
                                if (yv[k] > yv[maxIdx]) maxIdx = k;
                                if (yv[k] < yv[minIdx]) minIdx = k;
                            }
                            double yMax = yv[maxIdx], xMax = xv[maxIdx];
                            double yMin = yv[minIdx], xMin = xv[minIdx];

                            // Colors for highlights (tinted version of line color or defaults)
                            ImVec4 hiCol = (l.color.a >= 0.0f) ? ImVec4(l.color.r, l.color.g, l.color.b, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
                            ImVec4 maxCol = ImVec4(1.0f, hiCol.y * 0.5f, hiCol.z * 0.5f, 0.7f);
                            ImVec4 minCol = ImVec4(hiCol.x * 0.5f, hiCol.y * 0.5f, 1.0f, 0.7f);

                            // Draw horizontal lines at max/min
                            double maxLine[1] = { yMax };
                            ImPlot::PlotInfLines((label + " MAX##hmax" + std::to_string(i)).c_str(),
                                maxLine, 1, { 
                                    ImPlotProp_LineColor, maxCol,
                                    ImPlotProp_Flags, ImPlotInfLinesFlags_Horizontal 
                                });

                            double minLine[1] = { yMin };
                            ImPlot::PlotInfLines((label + " MIN##hmin" + std::to_string(i)).c_str(),
                                minLine, 1, { 
                                    ImPlotProp_LineColor, minCol,
                                    ImPlotProp_Flags, ImPlotInfLinesFlags_Horizontal 
                                });

                            // Annotate max point
                            char maxLabel[128];
                            snprintf(maxLabel, sizeof(maxLabel), "MAX\n%.4g", yMax);
                            ImPlot::Annotation(xMax, yMax, maxCol, ImVec2(8, -8), true, "%s", maxLabel);

                            // Annotate min point
                            char minLabel[128];
                            snprintf(minLabel, sizeof(minLabel), "MIN\n%.4g", yMin);
                            ImPlot::Annotation(xMin, yMin, minCol, ImVec2(8, 8), true, "%s", minLabel);
                        }
                    }
                    ImPlot::EndPlot();

                    if (pd.showExtrema) {
                        // Summary text below the plot
                        for (size_t i = 0; i < pd.lines.size(); ++i) {
                            auto& l = pd.lines[i];
                            if (l.yValues.empty()) continue;
                            const auto& yv = l.yValues;
                            const auto& xv = pd.xValues;
                            size_t maxIdx = 0, minIdx = 0;
                            for (size_t k = 1; k < yv.size(); ++k) {
                                if (yv[k] > yv[maxIdx]) maxIdx = k;
                                if (yv[k] < yv[minIdx]) minIdx = k;
                            }
                            std::string label = l.label.empty() ? l.yAxis.legend() : l.label;
                            ImVec4 txtCol = (l.color.a >= 0.0f) ? ImVec4(l.color.r, l.color.g, l.color.b, 1.0f) : ImVec4(1.0f,1.0f,1.0f,1.0f);
                            
                            ImGui::TextColored(txtCol, "[%s]", label.c_str());
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(txtCol.x, txtCol.y, txtCol.z, 0.8f),
                                "MAX: %.6g  @  x=%.6g     |     MIN: %.6g  @  x=%.6g",
                                yv[maxIdx], xv[maxIdx], yv[minIdx], xv[minIdx]);
                        }
                    }
                }
            }
            ImGui::End();
        }



        // 4a. Mission Sequence Panel


        ImGui::Begin("Mission");
        if (isProjectLoaded && selectedSpacecraftName != "") {
            Spacecraft* sc = nullptr;
            for (auto& s : spacecrafts) {
                if (s->Name == selectedSpacecraftName) { sc = s.get(); break; }
            }
            if (sc) {
                ImGui::Text("Sequence for: %s", sc->Name.c_str());
                ImGui::Separator();
                
                ImGui::TextColored(ImVec4(0.0f, 0.7f, 0.3f, 1.0f), "Sequence Status: %s", sc->missionSequence.isFinished ? "FINISHED" : (isSimulating ? "RUNNING" : "READY"));
                
                ImGui::BeginChild("MissionCommands", ImVec2(0, -60), true);
                for (size_t i = 0; i < sc->missionSequence.commands.size(); i++) {
                    auto& cmd = sc->missionSequence.commands[i];
                    if (sc->missionSequence.currentCommandIndex == i && !sc->missionSequence.isFinished && isSimulating) {
                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), ">> [%d] %s", (int)i, cmd->GetName().c_str());
                    } else if (i < sc->missionSequence.currentCommandIndex || sc->missionSequence.isFinished) {
                        ImGui::TextDisabled("   [%d] %s (Done)", (int)i, cmd->GetName().c_str());
                    } else {
                        ImGui::Text("   [%d] %s", (int)i, cmd->GetName().c_str());
                    }
                }
                ImGui::EndChild();
                
                if (ImGui::Button("Add Propagate (3 Days)")) {
                    sc->missionSequence.AddCommand(std::make_shared<PropagateCommand>(3.0 * 86400.0));
                }
                ImGui::SameLine();
                if (ImGui::Button("Add Burn (V-Bar 1km/s)")) {
                    sc->missionSequence.AddCommand(std::make_shared<ImpulsiveBurnCommand>(glm::dvec3(1.0, 0.0, 0.0), true)); // 1 km/s in V direction
                }
                
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "\xE2\x9A\x9B Mission Plan (Event-Based)");
                
                std::vector<std::string> dynBodyNames;
                std::vector<int> dynBodyIDs;
                dynBodyNames.push_back("CENTER");
                dynBodyIDs.push_back(0);
                for (const auto& p : planets) {
                    dynBodyNames.push_back(ToUpper(p.Name));
                    dynBodyIDs.push_back(p.SpiceID);
                }

                if (ImGui::BeginTable("BurnSchedule", 6, 
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                    
                    ImGui::TableSetupColumn("Trigger");
                    ImGui::TableSetupColumn("Parameters");
                    ImGui::TableSetupColumn("ΔVx (km/s)");
                    ImGui::TableSetupColumn("ΔVy (km/s)");
                    ImGui::TableSetupColumn("ΔVz (km/s)");
                    ImGui::TableSetupColumn("Ref Body");
                    // ImGui::TableSetupColumn("##enabled"); // Disabled toggle visually for space, keeping logic
                    ImGui::TableHeadersRow();
                    
                    for (int i = 0; i < g_BurnTable.size(); i++) {
                        auto& b = g_BurnTable[i];
                        ImGui::TableNextRow();
                        
                        // 1. Trigger Column
                        ImGui::TableSetColumnIndex(0);
                        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
                        int trigType = (int)b.trigger;
                        const char* trigNames[] = { "Time (GET)", "Orbital Event", "Altitude" };
                        if (ImGui::Combo(("##trig"+std::to_string(i)).c_str(), &trigType, trigNames, IM_ARRAYSIZE(trigNames))) {
                            b.trigger = (TriggerType)trigType;
                        }
                        ImGui::PopItemWidth();
                        
                        // 2. Parameters Column
                        ImGui::TableSetColumnIndex(1);
                        if (b.trigger == TriggerType::GET) {
                            ImGui::PushItemWidth(30);
                            ImGui::InputDouble(("##h"+std::to_string(i)).c_str(), &b.get_h, 0, 0, "%.0f"); ImGui::SameLine(); ImGui::Text(":"); ImGui::SameLine();
                            ImGui::InputDouble(("##m"+std::to_string(i)).c_str(), &b.get_m, 0, 0, "%.0f"); ImGui::SameLine(); ImGui::Text(":"); ImGui::SameLine();
                            ImGui::PushItemWidth(45);
                            ImGui::InputDouble(("##s"+std::to_string(i)).c_str(), &b.get_s, 0, 0, "%.1f");
                            ImGui::PopItemWidth(); ImGui::PopItemWidth();
                        } 
                        else if (b.trigger == TriggerType::APSIS) {
                            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
                            const char* apsisNames[] = { "Apoapsis", "Periapsis" };
                            ImGui::Combo(("##apsis"+std::to_string(i)).c_str(), &b.apsisType, apsisNames, IM_ARRAYSIZE(apsisNames));
                            ImGui::PopItemWidth();
                        }
                        else if (b.trigger == TriggerType::ALTITUDE) {
                            // Body selector
                            int bIdx = 0;
                            for (size_t j = 0; j < dynBodyIDs.size(); ++j) {
                                if (dynBodyIDs[j] == b.altRefBodyID) {
                                    bIdx = j;
                                    break;
                                }
                            }
                            ImGui::PushItemWidth(80);
                            if (ImGui::BeginCombo(("##altbody"+std::to_string(i)).c_str(), dynBodyNames[bIdx].c_str())) {
                                for (size_t j = 0; j < dynBodyNames.size(); j++) {
                                    bool isSelected = (bIdx == j);
                                    if (ImGui::Selectable(dynBodyNames[j].c_str(), isSelected)) {
                                        b.altRefBodyID = dynBodyIDs[j];
                                    }
                                    if (isSelected) ImGui::SetItemDefaultFocus();
                                }
                                ImGui::EndCombo();
                            }
                            ImGui::PopItemWidth();
                            ImGui::SameLine();
                            // Operator selector
                            const char* ops[] = { " < ", "<=" , ">=" , " > " };
                            ImGui::PushItemWidth(48);
                            ImGui::Combo(("##altcnd"+std::to_string(i)).c_str(), &b.altCondition, ops, IM_ARRAYSIZE(ops));
                            ImGui::PopItemWidth();
                            ImGui::SameLine();
                            // Altitude value
                            ImGui::PushItemWidth(65);
                            ImGui::InputDouble(("##alt"+std::to_string(i)).c_str(), &b.targetAltKM, 0, 0, "%.0f");
                            ImGui::PopItemWidth();
                            ImGui::SameLine();
                            ImGui::TextDisabled("km");
                        }

                        // 3. Delta-V X
                        ImGui::TableSetColumnIndex(2);
                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                        ImGui::InputDouble(("##dvx"+std::to_string(i)).c_str(), &b.dvx, 0, 0, "%.5f");
                        
                        // 4. Delta-V Y
                        ImGui::TableSetColumnIndex(3);
                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                        ImGui::InputDouble(("##dvy"+std::to_string(i)).c_str(), &b.dvy, 0, 0, "%.5f");
                        
                        // 5. Delta-V Z
                        ImGui::TableSetColumnIndex(4);
                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                        ImGui::InputDouble(("##dvz"+std::to_string(i)).c_str(), &b.dvz, 0, 0, "%.5f");
                        
                        // 6. Ref Body & Frame
                        ImGui::TableSetColumnIndex(5);
                        ImGui::SetNextItemWidth(80);
                        int refBIdx = 0;
                        for (size_t j = 0; j < dynBodyIDs.size(); ++j) {
                            if (dynBodyIDs[j] == b.refBodyID) {
                                refBIdx = j;
                                break;
                            }
                        }
                        if (ImGui::BeginCombo(("##ref"+std::to_string(i)).c_str(), dynBodyNames[refBIdx].c_str())) {
                            for (size_t j = 0; j < dynBodyNames.size(); j++) {
                                bool isSelected = (refBIdx == j);
                                if (ImGui::Selectable(dynBodyNames[j].c_str(), isSelected)) {
                                    b.refBodyID = dynBodyIDs[j];
                                }
                                if (isSelected) ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(50);
                        int frameIdx = b.isVNB ? 0 : 1;
                        const char* frameNames[] = { "VNB", "J2K" };
                        if (ImGui::Combo(("##frm"+std::to_string(i)).c_str(), &frameIdx, frameNames, IM_ARRAYSIZE(frameNames))) {
                            b.isVNB = (frameIdx == 0);
                        }
                        ImGui::SameLine();
                        ImGui::Checkbox(("##en"+std::to_string(i)).c_str(), &b.enabled);
                    }
                    ImGui::EndTable();
                }

                // Add / remove row
                if (ImGui::Button("+ Add Burn")) {
                    g_BurnTable.push_back({TriggerType::GET, 0, 0, 0.0, 0, 1000.0, true, 399, 0, 0, 0, 399, true, true});
                }
                ImGui::SameLine();
                if (!g_BurnTable.empty() && ImGui::Button("- Remove Last")) {
                    g_BurnTable.pop_back();
                }
                ImGui::SameLine();
                if (ImGui::Button("Load CSV")) {
                    std::string csvPath = OpenFileDialog("CSV Files\0*.csv\0All Files\0*.*\0", "Load Mission Plan CSV");
                    if (!csvPath.empty()) {
                        LoadMissionPlanFromCSV(csvPath, planets);
                    }
                }

                ImGui::Spacing();

                // Build sequence from table
                if (ImGui::Button("BUILD SEQUENCE FROM TABLE", 
                    ImVec2(ImGui::GetContentRegionAvail().x, 30))) {
                    
                    sc->missionSequence.commands.clear();
                    sc->missionSequence.Reset();
                    
                    double lastGET = 0.0;
                    
                    for (auto& b : g_BurnTable) {
                        if (!b.enabled) continue;
                        
                        double burnGET = lastGET;

                        if (b.trigger == TriggerType::GET) {
                            burnGET = b.get_h * 3600.0 + b.get_m * 60.0 + b.get_s;
                            sc->missionSequence.AddCommand(std::make_shared<PropagateToGETCommand>(burnGET, sc->missionEpochET));
                            lastGET = burnGET;
                        } 
                        else if (b.trigger == TriggerType::APSIS) {
                            sc->missionSequence.AddCommand(std::make_shared<PropagateToApsisCommand>(b.apsisType == 0, b.refBodyID));
                            // Since we don't naturally know the GET of the apsis until we hit it, we just carry lastGET forward.
                        }
                        else if (b.trigger == TriggerType::ALTITUDE) {
                            sc->missionSequence.AddCommand(std::make_shared<PropagateToAltitudeCommand>(b.altRefBodyID, b.targetAltKM, b.altCondition));
                        }
                        sc->missionSequence.AddCommand(
                            std::make_shared<ImpulsiveBurnCommand>(glm::dvec3(
                                b.dvx,
                                b.dvy,
                                b.dvz), b.isVNB, b.refBodyID));
                        
                        lastGET = burnGET;
                    }
                    
                    AddLog("[Mission] Sequence built: " + 
                           std::to_string(g_BurnTable.size()) + " burns from table.");
                }
                
                ImGui::Separator();
                if (ImGui::Button("Clear Sequence")) {
                    sc->missionSequence.commands.clear();
                    sc->missionSequence.Reset();
                }
            }
        } else {
            ImGui::TextDisabled("Select a Spacecraft from Resources \\nto view Mission Sequence.");
        }
        ImGui::End();



        // --- 0. Mission Output 2D Plot ---
        ImGui::Begin("Mission Output");
        if (selectedSpacecraftName != "") {
            // Plot configuration
            if (ImPlot::BeginPlot("Earth-Moon Rotating Reference Frame", ImVec2(-1, -1))) {

                ImPlot::SetupAxis(ImAxis_X1, "Longitudinal / Earth-Moon Axis (km)");
                ImPlot::SetupAxis(ImAxis_Y1, "Lateral Axis / Orbital Drift (km)");
                // Maintain equidistant scaling so orbits don't squeeze artificially
                ImPlot::SetupAxesLimits(-400000, 400000, -400000, 400000, ImPlotCond_Once);
                // Earth Center Reference
                double ex[1] = {0.0}, ey[1] = {0.0};
                ImPlot::PlotScatter("Earth (0,0)", ex, ey, 1);
                
                // Low Earth Orbit (LEO) Reference Ring (altitude ~250km -> r = 6628km)
                double leoR = 6628.0;
                std::vector<double> cx, cy;
                for (int i=0; i<=100; ++i) {
                    double theta = 2.0 * 3.1415926535 * i / 100.0;
                    cx.push_back(leoR * cos(theta)); cy.push_back(leoR * sin(theta));
                }
                ImPlot::PlotLine("LEO", cx.data(), cy.data(), cx.size());
                
                // Moon Runtime Position in Rotating Frame
                CelestialBody* earthRef = nullptr;
                CelestialBody* moonRef = nullptr;
                for(auto& b: planets) {
                    if(b.SpiceID == 399) earthRef = &b;
                    if(b.SpiceID == 301) moonRef = &b;
                }
                if (earthRef && moonRef) {
                    glm::dvec3 moonEC = moonRef->PositionKM - earthRef->PositionKM;
                    double dist = glm::length(glm::vec2(moonEC.x, moonEC.y));
                    double mx[1] = {dist}, my[1] = {0.0};
                    ImPlot::PlotScatter("Moon", mx, my, 1);
                }

                // Spacecraft Path Tracking - uses missionPath2D (rotating frame coords)
                Spacecraft* d_sc = nullptr;
                for(auto& sc: spacecrafts) { if(sc->Name == selectedSpacecraftName) { d_sc = sc.get(); break; } }
                if (d_sc) {
                    auto& pts = d_sc->missionPath2D;
                    if (!pts.empty()) {
                        std::vector<double> px, py, mx, my;
                        px.reserve(pts.size()); py.reserve(pts.size());
                        for (auto& pt : pts) {
                            px.push_back(pt.x);   // rotated CR3BP x
                            py.push_back(pt.y);   // rotated CR3BP y
                            if (pt.isManeuver) {
                                mx.push_back(pt.x);
                                my.push_back(pt.y);
                            }
                        }
                        // Rotating-frame flight path
                        ImPlot::PlotLine("Flight Path", px.data(), py.data(), (int)px.size());
                        
                        // Maneuver markers
                        if(!mx.empty()) {
                            ImPlot::PlotScatter("Maneuvers", mx.data(), my.data(), (int)mx.size());
                        }

                        // Current spacecraft position in rotating frame
                        {
                            CelestialBody* cBodyNow = nullptr;
                            CelestialBody* earthNow = nullptr;
                            CelestialBody* moonNow  = nullptr;
                            for(auto& b: planets) {
                                if(b.SpiceID == d_sc->centerBodySpiceID) cBodyNow = &b;
                                if(b.SpiceID == 399) earthNow = &b;
                                if(b.SpiceID == 301) moonNow  = &b;
                            }
                            if(earthNow && moonNow) {
                                glm::dvec3 absNow = d_sc->PositionKM + (cBodyNow ? cBodyNow->PositionKM : glm::dvec3(0.0));
                                glm::dvec3 ecNow  = absNow - earthNow->PositionKM;
                                glm::dvec3 mEC    = moonNow->PositionKM - earthNow->PositionKM;
                                double thN = atan2(mEC.y, mEC.x);
                                double cN  = cos(thN), sN = sin(thN);
                                double curX = ecNow.x * cN + ecNow.y * sN;
                                double curY = -ecNow.x * sN + ecNow.y * cN;
                                double spX[1] = {curX}, spY[1] = {curY};
                                ImPlot::PlotScatter("Spacecraft Now", spX, spY, 1);
                            }
                        }
                        
                        // Epoch/UTC Date Tooltip Overlay
                        if (ImPlot::IsPlotHovered()) {
                            ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                            double minDist = 1e12; double clEt = 0;
                            for (auto& pt : pts) {
                                double dist = std::pow(pt.x - mouse.x, 2) + std::pow(pt.y - mouse.y, 2);
                                if (dist < minDist) { minDist = dist; clEt = pt.et; }
                            }
                            char dateStr[64];
                            et2utc_c(clEt, "C", 0, 64, dateStr);
                            ImGui::BeginTooltip();
                            ImGui::Text("UTC Hover: %s", dateStr);
                            ImGui::EndTooltip();
                        }
                    }
                }
                ImPlot::EndPlot();
            }
        } else {
            ImGui::TextColored(ImVec4(1,0,0,1), "No Spacecraft Selected! Please create or select a spacecraft configuration.");
        }
        ImGui::End();

        // 5. View (The FBO mapping)
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0)); // No padding for the image
        ImGui::Begin("View");
        
        isSolarViewHovered = ImGui::IsWindowHovered();
        ImVec2 vpSize = ImGui::GetContentRegionAvail();
        viewportWidth = vpSize.x;
        viewportHeight = vpSize.y;
        
        // Render the FBO texture
        ImGui::Image((void*)(intptr_t)textureColorBuffer, vpSize, ImVec2(0, 1), ImVec2(1, 0));
        
        // Overlay HUD labels manually onto this window's draw list inside the image bounds
        ImVec2 winPos = ImGui::GetCursorScreenPos(); // Top-left of the image content area
        winPos.y -= vpSize.y; // ImGui Cursor is at the bottom after drawing the Image, so shift it back to top
        
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        for (auto& planet : planets) {
            glm::vec3 worldPos = glm::vec3((planet.PositionKM - worldOriginKM) * scaleFactor);
            glm::vec4 clipSpacePos = projection * view * glm::vec4(worldPos, 1.0f);
            if (clipSpacePos.w > 0.0f) {
                glm::vec3 ndcSpacePos = glm::vec3(clipSpacePos) / clipSpacePos.w;
                float screenX = ((ndcSpacePos.x + 1.0f) / 2.0f) * viewportWidth;
                float screenY = ((1.0f - ndcSpacePos.y) / 2.0f) * viewportHeight;
                
                if (screenX > 0 && screenX < viewportWidth && screenY > 0 && screenY < viewportHeight) {
                    ImU32 textColor = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, 0.7f));
                    ImVec2 textSize = ImGui::CalcTextSize(planet.Name.c_str());
                    float textX = winPos.x + screenX - (textSize.x / 2.0f);
                    float yOffset = (planet.SpiceID == 10) ? 30.0f : 15.0f; 
                    float textY = winPos.y + screenY + yOffset; 
                    drawList->AddText(ImVec2(textX, textY), textColor, planet.Name.c_str());
                }
            }
        }
        
        for (auto& label : axisLabels) {
            glm::vec4 clipSpacePos = projection * view * glm::vec4(label.pos, 1.0f);
            if (clipSpacePos.w > 0.0f) {
                glm::vec3 ndcSpacePos = glm::vec3(clipSpacePos) / clipSpacePos.w;
                float screenX = ((ndcSpacePos.x + 1.0f) / 2.0f) * viewportWidth;
                float screenY = ((1.0f - ndcSpacePos.y) / 2.0f) * viewportHeight;
                if (screenX > 0 && screenX < viewportWidth && screenY > 0 && screenY < viewportHeight) {
                    ImU32 textColor = ImGui::ColorConvertFloat4ToU32(ImVec4(label.color.x, label.color.y, label.color.z, 1.0f));
                    ImVec2 textSize = ImGui::CalcTextSize(label.text.c_str());
                    ImVec2 pos = ImVec2(winPos.x + screenX - (textSize.x / 2.0f), winPos.y + screenY - (textSize.y / 2.0f));
                    
                    // Simulate Bold Type by overdrawing with 1px offsets
                    drawList->AddText(pos, textColor, label.text.c_str());
                    drawList->AddText(ImVec2(pos.x + 1, pos.y), textColor, label.text.c_str());
                    drawList->AddText(ImVec2(pos.x, pos.y + 1), textColor, label.text.c_str());
                    drawList->AddText(ImVec2(pos.x + 1, pos.y + 1), textColor, label.text.c_str());
                }
            }
        }
        for (auto& sc : spacecrafts) {
            CelestialBody* centerBody = nullptr;
            for (auto& p : planets) { if (p.SpiceID == sc->centerBodySpiceID) { centerBody = &p; break; } }
            if (!centerBody) continue;

            glm::vec3 worldPos = glm::vec3((sc->PositionKM + centerBody->PositionKM - worldOriginKM) * scaleFactor);
            glm::vec4 clipSpacePos = projection * view * glm::vec4(worldPos, 1.0f);
            if (clipSpacePos.w > 0.0f) {
                glm::vec3 ndcSpacePos = glm::vec3(clipSpacePos) / clipSpacePos.w;
                float screenX = ((ndcSpacePos.x + 1.0f) / 2.0f) * viewportWidth;
                float screenY = ((1.0f - ndcSpacePos.y) / 2.0f) * viewportHeight;

                if (screenX > 0 && screenX < viewportWidth && screenY > 0 && screenY < viewportHeight) {
                    ImU32 textColor = ImGui::ColorConvertFloat4ToU32(ImVec4((float)sc->Color.r, (float)sc->Color.g, (float)sc->Color.b, 0.9f));
                    ImVec2 textSize = ImGui::CalcTextSize(sc->Name.c_str());
                    float textX = winPos.x + screenX - (textSize.x / 2.0f);
                    float textY = winPos.y + screenY + 15.0f; 
                    drawList->AddText(ImVec2(textX, textY), textColor, sc->Name.c_str());
                }
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();
        } // End of isProjectLoaded IDE Wrap

        // Draw "Create New Project" Modal on top of everything
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Create New Project", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Enter details for the new simulation project.");
            ImGui::Separator();
            
            ImGui::Text("Project Name:");
            ImGui::InputText("##projname", newProjectName, IM_ARRAYSIZE(newProjectName));
            
            ImGui::Text("Location:");
            ImGui::InputText("##projloc", newProjectLocation, IM_ARRAYSIZE(newProjectLocation));
            ImGui::SameLine();
            if (ImGui::Button("Browse Dir...")) {
                std::string path = OpenFolderDialog("Select Target Directory for New Project");
                if (!path.empty()) {
                    strncpy(newProjectLocation, path.c_str(), sizeof(newProjectLocation) - 1);
                }
            }
            
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.0f, 0.3f, 0.7f, 1.0f), "SPICE Kernels");
            ImGui::Separator();
            
            // ---- SPK ----
            ImGui::TextDisabled("SPK  - Ephemerides (Trajectories)   e.g. de432s.bsp");
            ImGui::SetNextItemWidth(-140);
            ImGui::InputText("##projspk", newProjectSpkPath, IM_ARRAYSIZE(newProjectSpkPath), ImGuiInputTextFlags_ReadOnly);
            ImGui::SameLine();
            if (ImGui::Button("Browse...##spk", ImVec2(130, 0))) {
                std::string path = OpenFileDialog("SPK Files (*.bsp)\0*.bsp\0All Files (*.*)\0*.*\0", "Select SPK Ephemeris Kernel");
                if (!path.empty()) {
                    strncpy(newProjectSpkPath, path.c_str(), sizeof(newProjectSpkPath) - 1);
                    ParseSpiceFilesForNewProject();
                }
            }
            ImGui::Spacing();
            
            // ---- PCK (Rotation/Shape) ----
            ImGui::TextDisabled("PCK  - Physical Constants (Shape, Rotation)   e.g. pck00011.tpc");
            ImGui::SetNextItemWidth(-140);
            ImGui::InputText("##projpck", newProjectPckPath, IM_ARRAYSIZE(newProjectPckPath), ImGuiInputTextFlags_ReadOnly);
            ImGui::SameLine();
            if (ImGui::Button("Browse...##pck", ImVec2(130, 0))) {
                std::string path = OpenFileDialog("PCK Files (*.tpc;*.bpc)\0*.tpc;*.bpc\0All Files (*.*)\0*.*\0", "Select Physical Constants PCK Kernel");
                if (!path.empty()) {
                    strncpy(newProjectPckPath, path.c_str(), sizeof(newProjectPckPath) - 1);
                    ParseSpiceFilesForNewProject();
                }
            }
            ImGui::Spacing();

            // ---- GM PCK (Gravitational Parameters) ----
            ImGui::TextDisabled("GM PCK  - Gravitational Parameters   e.g. gm_de440.tpc");
            ImGui::SetNextItemWidth(-140);
            ImGui::InputText("##projgmpck", newProjectGmPckPath, IM_ARRAYSIZE(newProjectGmPckPath), ImGuiInputTextFlags_ReadOnly);
            ImGui::SameLine();
            if (ImGui::Button("Browse...##gmpck", ImVec2(130, 0))) {
                std::string path = OpenFileDialog("PCK Files (*.tpc)\0*.tpc\0All Files (*.*)\0*.*\0", "Select GM PCK Kernel (e.g. gm_de440.tpc)");
                if (!path.empty()) {
                    strncpy(newProjectGmPckPath, path.c_str(), sizeof(newProjectGmPckPath) - 1);
                    ParseSpiceFilesForNewProject(); 
                }
            }
            // Geophysical Kernel is now built-in.
            ImGui::Spacing();

            // ---- LSK ----
            ImGui::TextDisabled("LSK  - Leap Seconds   e.g. naif0012.tls");
            ImGui::SetNextItemWidth(-140);
            ImGui::InputText("##projlsk", newProjectLskPath, IM_ARRAYSIZE(newProjectLskPath), ImGuiInputTextFlags_ReadOnly);
            ImGui::SameLine();
            if (ImGui::Button("Browse...##lsk", ImVec2(130, 0))) {
                std::string path = OpenFileDialog("LSK Files (*.tls)\0*.tls\0All Files (*.*)\0*.*\0", "Select Leap Seconds LSK Kernel");
                if (!path.empty()) {
                    strncpy(newProjectLskPath, path.c_str(), sizeof(newProjectLskPath) - 1);
                    ParseSpiceFilesForNewProject();
                }
            }

            if (!parsedCoverageStart.empty()) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.0f, 0.7f, 0.3f, 1.0f), "SPK Coverage: %s to %s", parsedCoverageStart.c_str(), parsedCoverageEnd.c_str());
                
                ImGui::Text("Detected Celestial Bodies:");
                
                // Precompute combo strings list for parent selection
                std::vector<std::string> comboItems;
                std::vector<int> comboIds;
                comboItems.push_back("Sun (10)");
                comboIds.push_back(10);
                for (const auto& bb : parsedBodies) {
                    if (bb.id != 10) {
                        comboItems.push_back(bb.name + " (" + std::to_string(bb.id) + ")");
                        comboIds.push_back(bb.id);
                    }
                }

                ImGui::BeginChild("parsed_bodies", ImVec2(0, 150), true);
                for (auto& b : parsedBodies) {
                    ImGui::Checkbox(b.name.c_str(), &b.selected);
                    ImGui::SameLine(180);
                    
                    // Dropdown for parent
                    ImGui::SetNextItemWidth(250);
                    std::string comboLabel = "Parent##" + std::to_string(b.id);
                    int current_idx = 0;
                    for (int n = 0; n < comboIds.size(); n++) {
                        if (comboIds[n] == b.parentId) { current_idx = n; break; }
                    }
                    if (ImGui::BeginCombo(comboLabel.c_str(), comboItems[current_idx].c_str())) {
                        for (int n = 0; n < comboItems.size(); n++) {
                            bool is_selected = (current_idx == n);
                            if (ImGui::Selectable(comboItems[n].c_str(), is_selected)) {
                                b.parentId = comboIds[n];
                            }
                            if (is_selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine();
                    
                    if (b.radius > 0.0) {
                        ImGui::TextDisabled("ID: %5d  Radius: %6.1f km", b.id, b.radius);
                    } else {
                        ImGui::TextDisabled("ID: %5d  Radius: Point Mass", b.id);
                    }
                }
                ImGui::EndChild();
            }
            
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            
            bool canCreate = !std::string(newProjectName).empty() && 
                             !std::string(newProjectLocation).empty() && 
                             !std::string(newProjectSpkPath).empty() && 
                             !std::string(newProjectPckPath).empty() && 
                             !std::string(newProjectLskPath).empty();
            
            bool hasSelection = false;
            for (const auto& b : parsedBodies) {
                if (b.selected) hasSelection = true;
            }
            if (!hasSelection) canCreate = false;

            if (!canCreate) ImGui::BeginDisabled();
            if (ImGui::Button("Create", ImVec2(120, 0))) {
                std::string projNameStr(newProjectName);
                std::string locStr(newProjectLocation);
                
                std::string fullPath = locStr + "\\" + projNameStr;
                    
                // Setup default project state
                g_Project = SimulationProject();
                ParseConfigText();
                // Speed is preserved globally across projects now
                isSimulationInitialized = false;
                isSimulating = false;
                isSimulationFinished = false;
                
                // Save and load the newly created folder project
                SaveProjectToFolder(fullPath, std::vector<CelestialBody>(), newProjectSpkPath, newProjectPckPath, newProjectLskPath, newProjectGmPckPath, parsedBodies);
                if (LoadProjectFromFolder(fullPath)) {
                    isProjectLoaded = true;
                    resetLayoutRequested = true;
                    ImGui::CloseCurrentPopup();
                } else {
                    AddLog("[ERROR] Failed to load the newly created project.");
                }
            }
            if (!canCreate) ImGui::EndDisabled();
            
            ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Draw "Upload SPK Kernel" Modal on top of everything (only if project is loaded)
        if (openUploadKernelModal) {
            ImGui::OpenPopup("Upload SPK Kernel");
            openUploadKernelModal = false;
        }
        
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Upload SPK Kernel", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Upload a new SPK (Ephemeris) Kernel to the current project.");
            ImGui::Separator();
            
            // ---- SPK ----
            ImGui::TextDisabled("SPK  - Ephemerides (Trajectories)   e.g. jup365.bsp");
            ImGui::SetNextItemWidth(-140);
            ImGui::InputText("##uploadspk", uploadKernelSpkPath, IM_ARRAYSIZE(uploadKernelSpkPath), ImGuiInputTextFlags_ReadOnly);
            ImGui::SameLine();
            if (ImGui::Button("Browse...##upspk", ImVec2(130, 0))) {
                std::string path = OpenFileDialog("SPK Files (*.bsp;*.spk)\0*.bsp;*.spk\0All Files (*.*)\0*.*\0", "Select SPK Ephemeris Kernel");
                if (!path.empty()) {
                    strncpy(uploadKernelSpkPath, path.c_str(), sizeof(uploadKernelSpkPath) - 1);
                    ParseSpkForUpload();
                }
            }
            ImGui::Spacing();

            if (!uploadParsedBodies.empty()) {
                ImGui::Separator();
                ImGui::Text("Select the celestial bodies you want to add to the simulation:");
                
                // Precompute combo strings list for parent selection based on currently active planets
                std::vector<std::string> parentItems;
                std::vector<int> parentIds;
                parentItems.push_back("Sun (10)");
                parentIds.push_back(10);
                
                // Add already existing planets to the potential parents list
                for (const auto& p : planets) {
                    if (p.SpiceID != 10) {
                        parentItems.push_back(p.Name + " (" + std::to_string(p.SpiceID) + ")");
                        parentIds.push_back(p.SpiceID);
                    }
                }
                // Add newly parsed bodies to the potential parents list (so a moon can parent another moon from the same file)
                for (const auto& bb : uploadParsedBodies) {
                    if (bb.id != 10) {
                        // Check if it's already in the list to avoid duplicates if ID matches an existing one
                        bool exists = false;
                        for (int pid : parentIds) { if (pid == bb.id) { exists = true; break; } }
                        if (!exists) {
                            parentItems.push_back(bb.name + " (" + std::to_string(bb.id) + ") [New]");
                            parentIds.push_back(bb.id);
                        }
                    }
                }

                ImGui::BeginChild("upload_parsed_bodies", ImVec2(0, 200), true);
                for (auto& b : uploadParsedBodies) {
                    ImGui::Checkbox(b.name.c_str(), &b.selected);
                    ImGui::SameLine(180);
                    
                    // Dropdown for parent
                    ImGui::SetNextItemWidth(250);
                    std::string comboLabel = "Parent##up" + std::to_string(b.id);
                    int current_idx = 0;
                    for (int n = 0; n < parentIds.size(); n++) {
                        if (parentIds[n] == b.parentId) { current_idx = n; break; }
                    }
                    if (ImGui::BeginCombo(comboLabel.c_str(), parentItems[current_idx].c_str())) {
                        for (int n = 0; n < parentItems.size(); n++) {
                            bool is_selected = (current_idx == n);
                            if (ImGui::Selectable(parentItems[n].c_str(), is_selected)) {
                                b.parentId = parentIds[n];
                            }
                            if (is_selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine();
                    
                    if (b.radius > 0.0) {
                        ImGui::TextDisabled("ID: %5d  Radius: %6.1f km", b.id, b.radius);
                    } else {
                        ImGui::TextDisabled("ID: %5d  Radius: Point Mass", b.id);
                    }
                }
                ImGui::EndChild();
            }
            
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            
            bool canUpload = !std::string(uploadKernelSpkPath).empty();
            bool hasSelection = false;
            for (const auto& b : uploadParsedBodies) {
                if (b.selected) hasSelection = true;
            }
            if (!hasSelection) canUpload = false;

            if (!canUpload) ImGui::BeginDisabled();
            if (ImGui::Button("Load Selected Objects", ImVec2(180, 0))) {
                std::string sourceSpkPath(uploadKernelSpkPath);
                std::string spkName = sourceSpkPath.substr(sourceSpkPath.find_last_of("\\/") + 1);
                std::string destSpkPath = g_Project.currentProjectPath + "\\datas\\" + spkName;
                
                // 1. Copy the SPK file to the project's datas/ directory
                if (CopyFileA(sourceSpkPath.c_str(), destSpkPath.c_str(), FALSE)) {
                    // 2. Append the filename to kernels.txt
                    std::string kernelsFile = g_Project.currentProjectPath + "\\datas\\kernels.txt";
                    std::ofstream kmd(kernelsFile, std::ios::app);
                    if (kmd.is_open()) {
                        kmd << spkName << "\n";
                        kmd.close();
                    }
                    
                    // 3. Load the kernel into the active SPICE session immediately
                    furnsh_c(destSpkPath.c_str());
                    AddLog("Successfully copied and loaded new kernel: " + spkName);
                    
                    // 4. Create the .esmatobj files for the selected bodies in SolarSystem/
                    std::string solarDir = g_Project.currentProjectPath + "\\SolarSystem";
                    for (const auto& b : uploadParsedBodies) {
                        if (b.selected) {
                            // Generate random default color for new body
                            srand(b.id * 88888);
                            float r = ((float)(rand() % 100)) / 100.0f * 0.5f + 0.5f;
                            float g = ((float)(rand() % 100)) / 100.0f * 0.5f + 0.5f;
                            float b_col = ((float)(rand() % 100)) / 100.0f * 0.5f + 0.5f;

                            std::string objPath = solarDir + "\\" + b.name + ".esmatobj";
                            std::ofstream objF(objPath);
                            if (objF.is_open()) {
                                objF << "BODY_NAME=" << b.name << "\n";
                                objF << "SPICE_ID=" << b.id << "\n";
                                objF << "PARENT_ID=" << b.parentId << "\n";
                                objF << "RADIUS_KM=" << b.radius << "\n";
                                objF << "COLOR=" << r << "," << g << "," << b_col << "\n";
                                objF << "SHOW_BODY=TRUE\n";
                                objF << "SHOW_ORBIT=TRUE\n";
                                
                                // Assign default textures if they happen to be major bodies
                                std::string texFile = "";
                                if (b.id == 1 || b.id == 199) texFile = "MercuryTexture.jpg";
                                else if (b.id == 2 || b.id == 299) texFile = "VenusTexture.jpg";
                                else if (b.id == 3 || b.id == 399) texFile = "EarthTexture.jpg";
                                else if (b.id == 301) texFile = "MoonTexture.jpg";
                                else if (b.id == 4 || b.id == 499) texFile = "MarsTexture.jpg";
                                else if (b.id == 5 || b.id == 599) texFile = "JupiterTexture.jpg";
                                else if (b.id == 6 || b.id == 699) texFile = "SaturnTexture.jpg";
                                else if (b.id == 7 || b.id == 799) texFile = "UranusTexture.jpg";
                                else if (b.id == 8 || b.id == 899) texFile = "NeptuneTexture.jpg";

                                if (!texFile.empty()) {
                                    objF << "TEXTURE_PATH=" << ResolvePath("data\\textures\\" + texFile) << "\n";
                                }

                                objF << "J2=" << b.j2 << "\n";
                                objF << "READ_ONLY=TRUE\n";
                                objF << "SPICE_DATA_LINKED=TRUE\n";
                                objF.close();
                            }
                        }
                    }
                    
                    // Clear the modal state
                    uploadKernelSpkPath[0] = '\0';
                    uploadParsedBodies.clear();
                    
                    // Inform the user
                    AddLog("Created objects for selected bodies. They will appear momentarily.");
                    ImGui::CloseCurrentPopup();
                } else {
                    AddLog("[ERROR] Failed to copy the SPK file to the project's datas directory.");
                }
            }
            if (!canUpload) ImGui::EndDisabled();
            
            ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                uploadKernelSpkPath[0] = '\0';
                uploadParsedBodies.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Draw "Create Spacecraft" Modal
        if (openNewSpacecraftModal) {
            ImGui::OpenPopup("Create Spacecraft");
            openNewSpacecraftModal = false;
        }

        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Create Spacecraft", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Enter initial state (J2000) for the new Spacecraft.");
            ImGui::Separator();
            
            ImGui::Text("Spacecraft Name:");
            ImGui::InputText("##scname", newSpacecraftName, IM_ARRAYSIZE(newSpacecraftName));
            
            ImGui::Spacing();
            ImGui::Text("NAIF / Object ID:");
            ImGui::InputInt("##scid", &newSpacecraftID);
            
            ImGui::Spacing();
            ImGui::InputText("State Vector Epoch (UTC)", newSpacecraftEpoch, IM_ARRAYSIZE(newSpacecraftEpoch));
            ImGui::InputText("Mission Start Epoch (GET=0 UTC)", newSpacecraftMissionEpoch, IM_ARRAYSIZE(newSpacecraftMissionEpoch));
            ImGui::Spacing();
            
            // Center body dropdown (List available planets)
            static int selectedCenterBodyIdx = 0;
            std::vector<std::string> comboNames;
            std::vector<int> comboIDs;
            for (int i = 0; i < planets.size(); i++) {
                comboNames.push_back(planets[i].Name);
                comboIDs.push_back(planets[i].SpiceID);
                if (planets[i].SpiceID == 399) { // default to Earth
                    selectedCenterBodyIdx = i;
                }
            }
            if (planets.empty()) {
                comboNames.push_back("None (Load SPK first)");
                comboIDs.push_back(-1);
            }
            
            ImGui::Text("Center Body:");
            if (ImGui::BeginCombo("##Center Body", comboNames.empty() ? "" : comboNames[selectedCenterBodyIdx].c_str())) {
                for (int n = 0; n < comboNames.size(); n++) {
                    bool is_selected = (selectedCenterBodyIdx == n);
                    if (ImGui::Selectable(comboNames[n].c_str(), is_selected)) {
                        selectedCenterBodyIdx = n;
                        // Sync Keplerian from current Cartesian state using new Mu
                        if (selectedCenterBodyIdx >= 0 && selectedCenterBodyIdx < (int)planets.size()) {
                             double parsed_et = 0.0;
                             std::string epochStr(newSpacecraftEpoch);
                             { size_t p = epochStr.find(" ::"); if (p != std::string::npos) epochStr = epochStr.substr(0, p); }
                             if (epochStr.find("UTC") == std::string::npos && epochStr.find("TDB") == std::string::npos) epochStr += " UTC";
                             str2et_c(epochStr.c_str(), &parsed_et);
                             SyncKeplerianFromCartesian(newSpacecraftPos, newSpacecraftVel, planets[selectedCenterBodyIdx].GM, parsed_et, 
                                                        kepSMA, kepECC, kepINC, kepRAAN, kepArgPe, kepTrueAnom);
                        }
                    }
                    if (is_selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            
            ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "Presets:");
            
            ImGui::Spacing();
            ImGui::Separator();
            
            ImGui::Checkbox("Initialize from SPK Kernel", &useSpkForSpacecraft);
            static bool continuousSpkLink = false;
            if (useSpkForSpacecraft) {
                ImGui::Indent();
                ImGui::Checkbox("Link Continuously to SPK", &continuousSpkLink);
                ImGui::InputText("SPK File", scSpkPath, IM_ARRAYSIZE(scSpkPath));
                ImGui::SameLine();
                if (ImGui::Button("Browse##spk")) {
                    std::string file = OpenFileDialog("SPICE Kernel (*.bsp)\0*.bsp\0All Files\0*.*\0", "Select SPK Kernel");
                    if (!file.empty()) {
                        strncpy(scSpkPath, file.c_str(), sizeof(scSpkPath) - 1);
                        scSpkPath[sizeof(scSpkPath) - 1] = '\0';
                        ParseSpkForSpacecraft();
                    }
                }
                
                if (!scParsedBodies.empty()) {
                    std::vector<std::string> spkNames;
                    for (const auto& b : scParsedBodies) spkNames.push_back(b.name);
                    
                    ImGui::Text("Target Body in SPK:");
                    if (ImGui::BeginCombo("##SC SPK Body", spkNames[selectedScSpkBodyIdx].c_str())) {
                        for (int n = 0; n < spkNames.size(); n++) {
                            bool is_selected = (selectedScSpkBodyIdx == n);
                            if (ImGui::Selectable(spkNames[n].c_str(), is_selected)) {
                                selectedScSpkBodyIdx = n;
                            }
                            if (is_selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    
                    if (ImGui::Button("Fetch State Vector")) {
                        if (selectedCenterBodyIdx >= 0 && selectedCenterBodyIdx < comboIDs.size()) {
                            int targetSpkId = scParsedBodies[selectedScSpkBodyIdx].id;
                            int centerId = comboIDs[selectedCenterBodyIdx];
                            
                            // Get Epoch
                            SpiceDouble parsed_et = 0.0;
                            std::string epochStr(newSpacecraftEpoch);
                            { size_t p = epochStr.find(" ::"); if (p != std::string::npos) epochStr = epochStr.substr(0, p); }
                            if (epochStr.find("UTC") == std::string::npos && epochStr.find("TDB") == std::string::npos) epochStr += " UTC";
                            str2et_c(epochStr.c_str(), &parsed_et);
                            
                            if (!failed_c()) {
                                // Important: We need to temporarily load the selected SPK so spkezr_c can find it
                                furnsh_c(scSpkPath);
                                
                                SpiceDouble state[6];
                                SpiceDouble lt;
                                spkezr_c(std::to_string(targetSpkId).c_str(), parsed_et, "J2000", "NONE", std::to_string(centerId).c_str(), state, &lt);
                                
                                if (!failed_c()) {
                                    newSpacecraftPos = glm::dvec3(state[0], state[1], state[2]);
                                    newSpacecraftVel = glm::dvec3(state[3], state[4], state[5]);
                                    
                                    // Also sync Keplerian inputs
                                    SyncKeplerianFromCartesian(newSpacecraftPos, newSpacecraftVel, planets[selectedCenterBodyIdx].GM, parsed_et, 
                                                               kepSMA, kepECC, kepINC, kepRAAN, kepArgPe, kepTrueAnom);

                                    AddLog("[SUCCESS] Fetched state from SPK for " + scParsedBodies[selectedScSpkBodyIdx].name + " relative to " + comboNames[selectedCenterBodyIdx]);
                                } else {
                                    char errMsg[1024];
                                    getmsg_c("SHORT", 1024, errMsg);
                                    AddLog("[ERROR] Failed to fetch state from SPK: " + std::string(errMsg));
                                    reset_c();
                                }
                                
                                unload_c(scSpkPath); // Unload immediately
                            } else {
                                AddLog("[ERROR] Invalid Epoch provided for state fetch.");
                                reset_c();
                            }
                        }
                    }
                } else if (strlen(scSpkPath) > 0) {
                    ImGui::TextColored(ImVec4(1, 0, 0, 1), "Failed to parse SPK or no bodies found.");
                }
                
                ImGui::Unindent();
            }
            
            ImGui::Spacing();
            ImGui::Separator();
            
            // --- Coordinate System Selection ---
            ImGui::Text("Coordinate System:");
            ImGui::SameLine();
            if (ImGui::RadioButton("Cartesian", !useKeplerianInputs)) {
                useKeplerianInputs = false;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Keplerian", useKeplerianInputs)) {
                useKeplerianInputs = true;
                // Sync current Cartesian to Keplerian
                double parsed_et = 0.0;
                std::string epochStr(newSpacecraftEpoch);
                { size_t p = epochStr.find(" ::"); if (p != std::string::npos) epochStr = epochStr.substr(0, p); }
                if (epochStr.find("UTC") == std::string::npos && epochStr.find("TDB") == std::string::npos) epochStr += " UTC";
                str2et_c(epochStr.c_str(), &parsed_et);
                if (selectedCenterBodyIdx >= 0 && selectedCenterBodyIdx < (int)planets.size()) {
                    SyncKeplerianFromCartesian(newSpacecraftPos, newSpacecraftVel, planets[selectedCenterBodyIdx].GM, parsed_et, 
                                               kepSMA, kepECC, kepINC, kepRAAN, kepArgPe, kepTrueAnom);
                }
            }

            if (!useKeplerianInputs) {
                ImGui::TextColored(ImVec4(0.0f, 0.3f, 0.7f, 1.0f), "Relative Position (KM)");
                ImGui::PushItemWidth(140);
                ImGui::InputDouble("X##pos", &newSpacecraftPos.x, 0.0, 0.0, "%.3f"); ImGui::SameLine();
                ImGui::InputDouble("Y##pos", &newSpacecraftPos.y, 0.0, 0.0, "%.3f"); ImGui::SameLine();
                ImGui::InputDouble("Z##pos", &newSpacecraftPos.z, 0.0, 0.0, "%.3f");
                ImGui::PopItemWidth();
                
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.0f, 0.3f, 0.7f, 1.0f), "Relative Velocity (KM/S)");
                ImGui::PushItemWidth(140);
                ImGui::InputDouble("Vx##vel", &newSpacecraftVel.x, 0.0, 0.0, "%.6f"); ImGui::SameLine();
                ImGui::InputDouble("Vy##vel", &newSpacecraftVel.y, 0.0, 0.0, "%.6f"); ImGui::SameLine();
                ImGui::InputDouble("Vz##vel", &newSpacecraftVel.z, 0.0, 0.0, "%.6f");
                ImGui::PopItemWidth();
            } else {
                double mu = (selectedCenterBodyIdx >= 0 && selectedCenterBodyIdx < (int)planets.size()) ? planets[selectedCenterBodyIdx].GM : 398600.44;
                double rad = (selectedCenterBodyIdx >= 0 && selectedCenterBodyIdx < (int)planets.size()) ? planets[selectedCenterBodyIdx].RadiusKM : 6371.0;
                
                double parsed_et = 0.0;
                std::string epochStr(newSpacecraftEpoch);
                { size_t p = epochStr.find(" ::"); if (p != std::string::npos) epochStr = epochStr.substr(0, p); }
                if (epochStr.find("UTC") == std::string::npos && epochStr.find("TDB") == std::string::npos) epochStr += " UTC";
                str2et_c(epochStr.c_str(), &parsed_et);

                ImGui::TextColored(ImVec4(0.0f, 0.7f, 0.3f, 1.0f), "Keplerian Elements");
                
                bool changed = false;
                ImGui::PushItemWidth(180);
                
                double alt = kepSMA - rad;
                if (ImGui::InputDouble("Altitude (km)", &alt, 1.0, 10.0, "%.3f")) {
                    kepSMA = alt + rad;
                    changed = true;
                }
                if (ImGui::InputDouble("Eccentricity (e)", &kepECC, 0.0001, 0.01, "%.6f")) {
                    if (kepECC < 0) kepECC = 0;
                    changed = true;
                }
                if (ImGui::InputDouble("Inclination (deg)", &kepINC, 0.1, 1.0, "%.4f")) changed = true;
                if (ImGui::InputDouble("RAAN (deg)", &kepRAAN, 0.1, 1.0, "%.4f")) changed = true;
                if (ImGui::InputDouble("Arg of Periapsis (deg)", &kepArgPe, 0.1, 1.0, "%.4f")) changed = true;
                if (ImGui::InputDouble("True Anomaly (deg)", &kepTrueAnom, 0.1, 1.0, "%.4f")) changed = true;
                
                ImGui::PopItemWidth();
                
                if (changed) {
                    SyncCartesianFromKeplerian(kepSMA, kepECC, kepINC, kepRAAN, kepArgPe, kepTrueAnom, mu, parsed_et, 
                                               newSpacecraftPos, newSpacecraftVel);
                }
            }
            
            if (isAddingESail) {
                ImGui::Spacing(); ImGui::Separator();
                ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f), "\xE2\x9A\xA1 Electric Sail Configuration");
                
                if (ImGui::BeginTabBar("ModalEsailTabs")) {
                    // --- TAB 1: SPIN & GEOMETRY ---
                    if (ImGui::BeginTabItem("Spin & Geometry")) {
                        ImGui::DragFloat("Tether Length (km)", &modalEsailLength, 0.1f, 1.0f, 100.0f, "%.1f");
                        ImGui::DragInt("Tether Count", &modalEsailCount, 1, 4, 300);
                        ImGui::DragFloat("Spin Axis RA (deg)", &modalEsailSpinRA, 0.5f, 0.0f, 360.0f, "%.1f");
                        ImGui::DragFloat("Spin Axis DEC (deg)", &modalEsailSpinDEC, 0.5f, -90.0f, 90.0f, "%.1f");
                        ImGui::DragFloat("Spin Rate (RPM)", &modalEsailSpinRate, 0.01f, 0.1f, 10.0f);
                        ImGui::DragFloat("Tether Deflection (deg)", &modalEsailDeflectionAngle, 0.1f, 0.0f, 45.0f, "%.1f");
                        ImGui::DragFloat("Dry Mass (kg)", &modalEsailMass, 1.0f, 10.0f, 2000.0f, "%.1f");
                        ImGui::EndTabItem();
                    }

                    // --- TAB 2: ENVIRONMENT ---
                    if (ImGui::BeginTabItem("Environment")) {
                        
                        ImGui::Spacing(); ImGui::Separator();
                        std::vector<std::string> swComboNames;
                        for (auto& sw : projectSolarWinds) swComboNames.push_back(sw.Name);
                        if (swComboNames.empty()) swComboNames.push_back("None Available");
                        
                        ImGui::Text("Attached Solar Wind:");
                        int safeIdx = selectedSolarWindIdx < (int)swComboNames.size() ? selectedSolarWindIdx : 0;
                        if (ImGui::BeginCombo("##SW_MODAL", swComboNames[safeIdx].c_str())) {
                            for (int n = 0; n < (int)swComboNames.size(); n++) {
                                if (projectSolarWinds.empty()) break;
                                bool is_selected = (selectedSolarWindIdx == n);
                                if (ImGui::Selectable(swComboNames[n].c_str(), is_selected)) selectedSolarWindIdx = n;
                                if (is_selected) ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }

                double m_p = 1.67262192e-27;
                double q_e = 1.60217662e-19;
                double eps_0 = 8.8541878128e-12;
                double n_sw = 5.0; // cm^-3
                double v_sw = 400000.0; // m/s

                if (!projectSolarWinds.empty() && selectedSolarWindIdx < projectSolarWinds.size()) {
                    // Exactly 1 AU (Earth orbit) should be used as reference when calculating nominal thrust!
                    // Previously (0,0,0), i.e. Sun's center was used, which caused 1/r^2 rule to return billions of times higher density.
                    double one_AU_km = 149597870.7;
                    n_sw = projectSolarWinds[selectedSolarWindIdx].getDensity(one_AU_km, 0, 0, et_time);
                    v_sw = glm::length(projectSolarWinds[selectedSolarWindIdx].getVelocity(one_AU_km, 0, 0, et_time)) * 1000.0;
                }

                double V_w = (m_p * v_sw * v_sw) / (2.0 * q_e);
                double sigma = 0.18 * (std::max)(0.0, (double)modalEsailVoltage * 1000.0 - V_w) * std::sqrt(eps_0 * m_p * (n_sw * 1.0e6) * v_sw * v_sw);

                modalEsailThrustMax = (double)modalEsailCount * (modalEsailLength * 1000.0) * sigma;
                modalEsailCharAccel = (modalEsailThrustMax / modalEsailMass) * 1000.0;

                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Nominal Thrust: %.4f N | Char Accel: %.4f mm/s^2", modalEsailThrustMax, modalEsailCharAccel);
            }

            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            
            bool canCreateSC = !std::string(newSpacecraftName).empty() && !std::string(newSpacecraftEpoch).empty() && !planets.empty();
            
            if (!canCreateSC) ImGui::BeginDisabled();
            if (ImGui::Button(isEditingSpacecraft ? "Save Settings" : "Spawn Spacecraft", ImVec2(140, 0))) {
                bool idConflict = false;
                for (const auto& sc : spacecrafts) {
                    if (isEditingSpacecraft && sc->Name == std::string(originalSpacecraftName)) continue;
                    if (sc->ID == newSpacecraftID && newSpacecraftID != -1) { idConflict = true; break; }
                }
                for (const auto& p : planets) {
                    if (p.SpiceID == newSpacecraftID) { idConflict = true; break; }
                }

                if (idConflict) {
                    AddLog("[ERROR] Cannot spawn! Object ID " + std::to_string(newSpacecraftID) + " is already in use by another object.");
                } else if (g_Project.currentProjectPath.empty()) {
                    AddLog("[ERROR] Cannot save spacecraft, project path is empty.");
                } else {
                    std::shared_ptr<Spacecraft> newSc;
                    if (isAddingESail) {
                        auto es = std::make_shared<ElectricSail>(newSpacecraftName, glm::vec3(0.0f, 1.0f, 1.0f));
                        es->esailLengthKM = modalEsailLength;
                        es->esailTetherCount = modalEsailCount;
                        es->esailCharAccel = modalEsailCharAccel;
                        es->esailSpinRA = glm::radians((double)modalEsailSpinRA);
                        es->esailSpinDEC = glm::radians((double)modalEsailSpinDEC);
                        es->esailDeflectionAngleDeg = modalEsailDeflectionAngle;
                        es->esailVoltageKV = modalEsailVoltage;
                        es->esailMassKG = modalEsailMass;

                        es->spinRateRPM = modalEsailSpinRate;





                        if (!projectSolarWinds.empty() && selectedSolarWindIdx < projectSolarWinds.size()) {
                            es->attachedWindName = projectSolarWinds[selectedSolarWindIdx].Name;
                        } else {
                            es->attachedWindName = "";
                        }
                        
                        newSc = es;
                    } else {
                        newSc = std::make_shared<Spacecraft>(newSpacecraftName, glm::vec3(0.0f, 1.0f, 1.0f));
                    }

                    newSc->ID = newSpacecraftID;
                    
                    if (useSpkForSpacecraft && continuousSpkLink && !scParsedBodies.empty() && strlen(scSpkPath) > 0) {
                        newSc->isSpiceLinked = true;
                        newSc->linkedSpiceID = scParsedBodies[selectedScSpkBodyIdx].id;
                        
                        std::string spkFileName = std::string(scSpkPath);
                        size_t lastSlash = spkFileName.find_last_of("\\/");
                        if (lastSlash != std::string::npos) spkFileName = spkFileName.substr(lastSlash + 1);
                        
                        std::string destPath = g_Project.currentProjectPath + "\\datas\\" + spkFileName;
                        if (!fileExists(destPath)) {
                            CopyFileA(scSpkPath, destPath.c_str(), FALSE);
                            std::ofstream kmd(g_Project.currentProjectPath + "\\datas\\kernels.txt", std::ios::app);
                            if (kmd.is_open()) { kmd << spkFileName << "\n"; kmd.close(); }
                            furnsh_c(destPath.c_str());
                            AddLog("[INFO] Kernel copied and loaded for linked spacecraft: " + spkFileName);
                        }
                    }
                    
                    newSc->centerBodySpiceID = comboIDs[selectedCenterBodyIdx];
                    newSc->initialCenterBodySpiceID = newSc->centerBodySpiceID;
                    newSc->InitialPositionKM = newSpacecraftPos;
                    newSc->InitialVelocityKMS = newSpacecraftVel;
                    newSc->PositionKM = newSc->InitialPositionKM;
                    newSc->VelocityKMS = newSc->InitialVelocityKMS;
                    
                    SpiceDouble parsed_et = 0.0;
                    std::string epochStr(newSpacecraftEpoch);
                    for (const char* suffix : { " ::UTC", " ::TDB", "::UTC", "::TDB" }) {
                        size_t p = epochStr.find(suffix); if (p != std::string::npos) { epochStr = epochStr.substr(0, p); break; }
                    }
                    if (epochStr.find("UTC") == std::string::npos && epochStr.find("TDB") == std::string::npos) epochStr += " UTC";
                    str2et_c(epochStr.c_str(), &parsed_et);
                    if (!failed_c()) newSc->epochET = parsed_et;
                    else { reset_c(); newSc->epochET = 0.0; }
                    
                    SpiceDouble parsed_mission_et = 0.0;
                    std::string missionEpochStr(newSpacecraftMissionEpoch);
                    for (const char* suffix : { " ::UTC", " ::TDB", "::UTC", "::TDB" }) {
                        size_t p = missionEpochStr.find(suffix); if (p != std::string::npos) { missionEpochStr = missionEpochStr.substr(0, p); break; }
                    }
                    if (missionEpochStr.find("UTC") == std::string::npos && missionEpochStr.find("TDB") == std::string::npos) missionEpochStr += " UTC";
                    str2et_c(missionEpochStr.c_str(), &parsed_mission_et);
                    if (!failed_c()) newSc->missionEpochET = parsed_mission_et;
                    else { reset_c(); newSc->missionEpochET = newSc->epochET; }
                    
                    std::string modelPath = ResolvePath("data\\models\\testsatmodel.glb");
                    if (fileExists(modelPath)) {
                        newSc->model = std::make_shared<ModelGLTF>(modelPath);
                        if (!newSc->model->loaded) AddLog("[WARN] Spacecraft model failed to load.");
                    }
                    
                    if (isEditingSpacecraft) {
                        for (auto& s : spacecrafts) {
                            if (s->Name == std::string(originalSpacecraftName)) {
                                newSc->Color = s->Color;
                                newSc->showBody = s->showBody;
                                newSc->showTrajectory = s->showTrajectory;
                                s = newSc;
                                s->reset(); // Snaps to new initial properties 
                                break;
                            }
                        }
                        if (std::string(originalSpacecraftName) != std::string(newSpacecraftName)) {
                            std::string oldPath = g_Project.currentProjectPath + "\\Spacecrafts\\" + std::string(originalSpacecraftName) + ".esmatspacecraft";
                            remove(oldPath.c_str());
                        }
                    } else {
                        spacecrafts.push_back(newSc);
                    }
                    
                    // Save to file
                    std::ofstream f(g_Project.currentProjectPath + "\\Spacecrafts\\" + newSpacecraftName + ".esmatspacecraft");
                    if (f.is_open()) {
                        f << "name=" << newSpacecraftName << "\n";
                        f << "id=" << newSc->ID << "\n";
                        f << "center=" << newSc->centerBodySpiceID << "\n";
                        f << "color=0.00,1.00,1.00\n";
                        f << "model=data\\models\\testsatmodel.glb\n";
                        f << "pos=" << newSc->InitialPositionKM.x << "," << newSc->InitialPositionKM.y << "," << newSc->InitialPositionKM.z << "\n";
                        f << "vel=" << newSc->InitialVelocityKMS.x << "," << newSc->InitialVelocityKMS.y << "," << newSc->InitialVelocityKMS.z << "\n";
                        f << "epoch=" << newSpacecraftEpoch << "\n";
                        f << "mission_epoch=" << newSpacecraftMissionEpoch << "\n";
                        
                        if (auto es = std::dynamic_pointer_cast<ElectricSail>(newSc)) {
                            f << "has_esail=1\n";
                            f << "mass=" << es->esailMassKG << "\n";
                            f << "esail_len=" << es->esailLengthKM << "\n";
                            f << "esail_cnt=" << es->esailTetherCount << "\n";
                            f << "esail_vol=" << es->esailVoltageKV << "\n";
                            f << "esail_mass=" << es->esailMassKG << "\n";
                            f << "esail_accel=" << es->esailCharAccel << "\n";
                            f << "esail_ra=" << es->esailSpinRA << "\n";
                            f << "esail_dec=" << es->esailSpinDEC << "\n";
                            f << "esail_deflection=" << es->esailDeflectionAngleDeg << "\n";
                            f << "esail_wind_name=" << es->attachedWindName << "\n";
                            f << "esail_spin_rate=" << es->spinRateRPM << "\n";




                        } else {
                            f << "mass=1000.0\n";
                        }
                        if (newSc->isSpiceLinked) {
                            f << "is_spice_linked=1\n";
                            f << "linked_spice_id=" << newSc->linkedSpiceID << "\n";
                        }
                        f.close();
                        if (isEditingSpacecraft) {
                            AddLog("[SUCCESS] Spacecraft '" + std::string(newSpacecraftName) + "' settings updated.");
                        } else {
                            AddLog("[SUCCESS] Spacecraft '" + std::string(newSpacecraftName) + "' created.");
                        }
                        
                        // Reset modal state
                        newSpacecraftPos = glm::dvec3(0.0);
                        newSpacecraftVel = glm::dvec3(0.0);
                        useSpkForSpacecraft = false;
                        continuousSpkLink = false;
                        scSpkPath[0] = '\0';
                        scParsedBodies.clear();
                    } else {
                        AddLog("[ERROR] Failed to save spacecraft file.");
                    }
                    ImGui::CloseCurrentPopup();
                }
            }
            if (!canCreateSC) ImGui::EndDisabled();
            
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100, 0))) {
                ImGui::CloseCurrentPopup();
            }
            
            ImGui::EndPopup();
        }

        // Deleted E-Sail 3D Designer ImGui Window from here (Moved to Standalone Mode)


        // Second Pass: Draw ImGui to Screen
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, SCR_WIDTH, SCR_HEIGHT);
        glClearColor(0.12f, 0.12f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwTerminate();
    return 0;
}

void processInput(GLFWwindow *window) {
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        glfwSetWindowShouldClose(window, true);
    
    // Only accept keyboard movement if standard ImGui inputs aren't grabbing it
    // and we are actually interacting with the View
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureKeyboard && !isSolarViewHovered) return;

    if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) camera.ProcessKeyboard(FORWARD, deltaTime);
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) camera.ProcessKeyboard(BACKWARD, deltaTime);
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) camera.ProcessKeyboard(LEFT, deltaTime);
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) camera.ProcessKeyboard(RIGHT, deltaTime);
    }
}

void mouse_callback(GLFWwindow* window, double xposIn, double yposIn) {
    float xpos = static_cast<float>(xposIn);
    float ypos = static_cast<float>(yposIn);
    
    if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
        if (!isMousePressed) {
            lastX = xpos;
            lastY = ypos;
            isMousePressed = true;
        }

        float xoffset = xpos - lastX;
        float yoffset = lastY - ypos; // reversed since y-coordinates go from bottom to top

        lastX = xpos;
        lastY = ypos;

        if (isSolarViewHovered) {
            camera.ProcessMouseMovement(xoffset, yoffset);
        }
    } else {
        isMousePressed = false;
    }
}

void scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
    if (isSolarViewHovered) {
        camera.ProcessMouseScroll(static_cast<float>(yoffset));
    }
}
void framebuffer_size_callback(GLFWwindow* window, int width, int height) {
    glViewport(0, 0, width, height);
    if(height == 0) height = 1;
    SCR_WIDTH = width;
    SCR_HEIGHT = height;
}