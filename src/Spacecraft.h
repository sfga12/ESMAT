#pragma once
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "CelestialBody.h"
#include "LineRenderer.h"
#include "SpiceUsr.h"
#include "Model.h"
#include <memory>
#include "MissionSystem.h"

struct PlotPoint2D {
    double x, y;
    double et;
    bool isManeuver;
};

class Spacecraft {
public:
    std::string Name;
    int ID;
    int centerBodySpiceID;        // The active body this spacecraft is centered on
    int initialCenterBodySpiceID; // For reset
    
    glm::dvec3 PositionKM;        // Relative to centerBody
    glm::dvec3 VelocityKMS;       // Relative to centerBody
    glm::dvec3 InitialPositionKM; // Set relative to initialCenterBody
    glm::dvec3 InitialVelocityKMS;
    glm::vec3 Color;
    bool showBody;
    bool showTrajectory;
    double epochET = 0.0;        // Spacecraft's local state vector time in ET 
    double missionEpochET = 0.0; // Spacecraft's designated Mission Start Time (GET 0) in ET
    
    // SPICE Data Link
    bool isSpiceLinked = false;
    int linkedSpiceID = -1;
    
    std::shared_ptr<LineRenderer> trajectoryRenderer;
    std::shared_ptr<ModelGLTF> model;
    
    // Mission Sequence system
    MissionSequence missionSequence;
    std::vector<PlotPoint2D> missionPath2D;

    Spacecraft(std::string name, glm::vec3 color) 
        : Name(name), Color(color), showBody(true), showTrajectory(true), centerBodySpiceID(399), initialCenterBodySpiceID(399), ID(-1) {
        PositionKM = glm::dvec3(0.0);
        VelocityKMS = glm::dvec3(0.0);
        InitialPositionKM = glm::dvec3(0.0);
        InitialVelocityKMS = glm::dvec3(0.0);
        epochET = 0.0;
        missionEpochET = 0.0;
        trajectoryRenderer = std::make_shared<LineRenderer>();
    }

    virtual ~Spacecraft() = default;

    void reset() {
        centerBodySpiceID = initialCenterBodySpiceID;
        PositionKM = InitialPositionKM;
        VelocityKMS = InitialVelocityKMS;
        if (trajectoryRenderer) {
            trajectoryRenderer->pointsKM.clear();
        }
        missionSequence.Reset();
        missionPath2D.clear();
    }

    // N-body gravity acceleration calculation (RELATIVE to center body)
    // a_rel = a_sc_inertial - a_center_inertial
    virtual glm::dvec3 computeAcceleration(double et, const std::vector<CelestialBody>& bodies, double dt_offset = 0.0) {
        glm::dvec3 accSC(0.0);
        glm::dvec3 accCenter(0.0);

        const CelestialBody* centerBody = nullptr;
        for (const auto& b : bodies) {
            if (b.SpiceID == centerBodySpiceID) {
                centerBody = &b;
                break;
            }
        }

        if (!centerBody) return glm::dvec3(0.0);

        // Absolute position of SC: centerBody->PositionKM (start) + shift + PositionKM (relative)
        glm::dvec3 centerPosAtT = centerBody->PositionKM + centerBody->VelocityKMS * dt_offset;
        glm::dvec3 scAbsPos = centerPosAtT + PositionKM;

        for (const auto& body : bodies) {
            double gm = body.GM;
            if (gm <= 0.0) continue;

            glm::dvec3 bodyPosAtT = body.PositionKM + body.VelocityKMS * dt_offset;

            // 1. Force on spacecraft from this body
            glm::dvec3 rSC = bodyPosAtT - scAbsPos;
            double dSC = glm::length(rSC);
            if (dSC > 1.0) {
                accSC += gm * rSC / (dSC * dSC * dSC);
                
                // J2 Perturbation on Spacecraft
                if (body.J2 > 0.0) {
                    glm::dvec3 r_sc_from_body = -rSC; // FROM body TO Spacecraft
                    glm::dmat3 R_body = glm::dmat3(body.currentRotationMatrix);
                    // Rotation matrix also technically drifts, but over 600s it's minor compared to position
                    glm::dmat3 R_body_inv = glm::transpose(R_body);
                    glm::dvec3 r_local = R_body_inv * r_sc_from_body;
                    
                    double x = r_local.x; double y = r_local.y; double z = r_local.z;
                    double r2 = dSC * dSC; double z2_over_r2 = (z * z) / r2;
                    double R_eq = body.RadiusKM;
                    double j2_factor = -1.5 * body.J2 * gm * R_eq * R_eq / (r2 * r2 * dSC);
                    
                    glm::dvec3 aJ2_local;
                    aJ2_local.x = j2_factor * x * (1.0 - 5.0 * z2_over_r2);
                    aJ2_local.y = j2_factor * y * (1.0 - 5.0 * z2_over_r2);
                    aJ2_local.z = j2_factor * z * (3.0 - 5.0 * z2_over_r2);
                    accSC += R_body * aJ2_local;
                }
            }

            // 2. Force on center body (e.g. Earth) from this body
            if (body.SpiceID != centerBodySpiceID) {
                glm::dvec3 rCenter = bodyPosAtT - centerPosAtT;
                double dCenter = glm::length(rCenter);
                if (dCenter > 1.0) {
                    accCenter += gm * rCenter / (dCenter * dCenter * dCenter);
                    
                    // J2 Perturbation on Center Body
                    if (body.J2 > 0.0) {
                        glm::dvec3 r_cb_from_body = -rCenter;
                        glm::dmat3 R_body = glm::dmat3(body.currentRotationMatrix);
                        glm::dmat3 R_body_inv = glm::transpose(R_body);
                        glm::dvec3 r_local = R_body_inv * r_cb_from_body;
                        
                        double x = r_local.x; double y = r_local.y; double z = r_local.z;
                        double r2 = dCenter * dCenter; double z2_over_r2 = (z * z) / r2;
                        double R_eq = body.RadiusKM;
                        double j2_factor = -1.5 * body.J2 * gm * R_eq * R_eq / (r2 * r2 * dCenter);
                        
                        glm::dvec3 aJ2_local;
                        aJ2_local.x = j2_factor * x * (1.0 - 5.0 * z2_over_r2);
                        aJ2_local.y = j2_factor * y * (1.0 - 5.0 * z2_over_r2);
                        aJ2_local.z = j2_factor * z * (3.0 - 5.0 * z2_over_r2);
                        accCenter += R_body * aJ2_local;
                    }
                }
            }
        }
        return accSC - accCenter;
    }

    void updatePhysics(double dt, const std::vector<CelestialBody>& bodies) {
        glm::dvec3 initialPos = PositionKM;
        glm::dvec3 initialVel = VelocityKMS;

        // k1
        glm::dvec3 k1_pos = initialVel;
        glm::dvec3 k1_vel = computeAcceleration(epochET, bodies, 0.0);

        // k2
        PositionKM = initialPos + 0.5 * k1_pos * dt;
        VelocityKMS = initialVel + 0.5 * k1_vel * dt;
        glm::dvec3 k2_pos = VelocityKMS;
        glm::dvec3 k2_vel = computeAcceleration(epochET + 0.5 * dt, bodies, 0.5 * dt);

        // k3
        PositionKM = initialPos + 0.5 * k2_pos * dt;
        VelocityKMS = initialVel + 0.5 * k2_vel * dt;
        glm::dvec3 k3_pos = VelocityKMS;
        glm::dvec3 k3_vel = computeAcceleration(epochET + 0.5 * dt, bodies, 0.5 * dt);

        // k4
        PositionKM = initialPos + k3_pos * dt;
        VelocityKMS = initialVel + k3_vel * dt;
        glm::dvec3 k4_pos = VelocityKMS;
        glm::dvec3 k4_vel = computeAcceleration(epochET + dt, bodies, dt);

        // Final state compilation
        PositionKM = initialPos + (dt / 6.0) * (k1_pos + 2.0 * k2_pos + 2.0 * k3_pos + k4_pos);
        VelocityKMS = initialVel + (dt / 6.0) * (k1_vel + 2.0 * k2_vel + 2.0 * k3_vel + k4_vel);

        epochET += dt;

        checkCenterBody(bodies);
    }

    void checkCenterBody(const std::vector<CelestialBody>& bodies) {
        // Prevent frame jumps during the first 1 second of simulation for stability
        if (epochET < missionEpochET + 1.0) return;

        const CelestialBody* currentCenter = nullptr;
        for (const auto& b : bodies) {
            if (b.SpiceID == centerBodySpiceID) {
                currentCenter = &b;
                break;
            }
        }
        if (!currentCenter) return;

        glm::dvec3 scAbsPos = currentCenter->PositionKM + PositionKM;
        
        const CelestialBody* bestBody = nullptr;
        double smallestSOI = 1e16; 
        
        for (const auto& body : bodies) {
            if (body.GM <= 0.0) continue;
            
            double r_soi = 1e15; 
            
            if (body.SpiceID != 10 && body.ParentID != 0) {
                double parentGM = 0.0;
                for (const auto& p : bodies) {
                    if (p.SpiceID == body.ParentID) { parentGM = p.GM; break; }
                }
                
                if (parentGM > 0.0) {
                    double d_parent = glm::length(body.PositionKM - body.ParentPositionKM);
                    r_soi = d_parent * std::pow(body.GM / parentGM, 0.4);
                }
            }
            
            double dist = glm::length(body.PositionKM - scAbsPos);
            double effective_soi = (body.SpiceID == centerBodySpiceID) ? (r_soi * 1.05) : (r_soi * 0.95);
            
            if (dist <= effective_soi) {
                if (r_soi <= smallestSOI) {
                    smallestSOI = r_soi;
                    bestBody = &body;
                }
            }
        }

        if (bestBody && bestBody->SpiceID != centerBodySpiceID) {
            transitionToCenter(*bestBody, *currentCenter);
        }
    }

    void transitionToCenter(const CelestialBody& newCenter, const CelestialBody& oldCenter) {
        glm::dvec3 posShift = oldCenter.PositionKM - newCenter.PositionKM;
        glm::dvec3 velShift = oldCenter.VelocityKMS - newCenter.VelocityKMS;

        PositionKM += posShift;
        VelocityKMS += velShift;
        centerBodySpiceID = newCenter.SpiceID;

        // Debug: print Moon-relative state at SOI transition
        double r_moon = glm::length(PositionKM);
        double v_rel  = glm::length(VelocityKMS);
        // Impact parameter: b = |r x v_hat| = r * sin(angle between r and v)
        glm::dvec3 r_hat = PositionKM / r_moon;
        glm::dvec3 v_hat = VelocityKMS / v_rel;
        double b_soi = r_moon * glm::length(glm::cross(r_hat, v_hat));
        // v_inf^2 = v^2 - 2*GM/r → periapsis from b and v_inf
        double v_inf_sq = v_rel*v_rel - 2.0*newCenter.GM/r_moon;
        if (v_inf_sq < 0) v_inf_sq = 0;
        double v_inf = std::sqrt(v_inf_sq);
        double r_p_pred = (v_inf > 0.01)
            ? (newCenter.GM/v_inf_sq) * (std::sqrt(1.0 + (b_soi*v_inf_sq/newCenter.GM)*(b_soi*v_inf_sq/newCenter.GM)) - 1.0)
            : 0.0;
        printf("[SOI-DBG] SOI entry to %s: r=%d km  v_rel=%.4f km/s  b=%d km  v_inf=%.4f km/s  pred_periapsis=%d km\n",
               newCenter.Name.c_str(), (int)r_moon, v_rel, (int)b_soi, v_inf, (int)r_p_pred);
        fflush(stdout);
    }
};

