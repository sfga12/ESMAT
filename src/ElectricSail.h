#pragma once
#include "Spacecraft.h"
#include "ESailMesh.h"
#include "SolarWind.h"
#include <vector>
#include <cmath>
#include <algorithm>
#include <glm/glm.hpp>
#include <string>
#include <memory>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

extern std::vector<SolarWindField> projectSolarWinds;

struct TetherState {
    double phi = 0.0;           // Phase angle (rad) — updated with dt in each step
    double Lambda = 0.0;        // Coning angle (rad)
    double beta = 0.0;          // Lagging angle (rad)
    double L_km = 20.0;         // Instant length (km)
    double Phi_kV = 25.0;       // Applied voltage (kV)
    double tension_cN = 0.3;    // Tension force (cN)
};

class ElectricSail : public Spacecraft {
public:
    // Physical Constants (Section 12)
    static constexpr double EPS0   = 8.8541878128e-12; // F/m
    static constexpr double MP     = 1.6726219e-27;    // kg
    static constexpr double ME     = 9.10938356e-31;   // kg
    static constexpr double QE     = 1.60217662e-19;   // C
    static constexpr double AU_KM  = 149597870.7;      // km
    static constexpr double MU_SUN = 1.32712440018e11; // km^3/s^2

    // --- Group 0: Core (Backward Compatible) ---
    double esailLengthKM          = 20.0;
    int    esailTetherCount       = 50;
    double esailVoltageKV         = 25.0;
    double esailMassKG            = 150.0;
    double esailDeflectionAngleDeg = 10.0;
    double esailCharAccel         = 1.0;  // mm/s^2 at 1 AU
    double esailSpinRA            = 0.0;  // Radians
    double esailSpinDEC           = 0.0;  // Radians

    // --- Group A: Spin Dynamics ---
    double spinRateRPM             = 1.0;
    double esailSpinRateMin        = 0.1;
    double esailSpinRateMax        = 5.0;
    double esailTetherTension      = 0.3; // cN
    double esailRemoteUnitMassKG   = 0.0025;
    double esailTetherDiameterUM   = 75.0;
    double esailTetherDensityKgM3  = 2700.0; // Al
    double esailTetherElasticGPa   = 70.0;
    double esailTetherTensileGPa   = 0.14;

    // --- Group B: Control Infrastructure (Future script system) ---
    // Active control is not applied in the physics loop currently.
    // Scripts will update these parameters to perform control in the future.
    double minVoltageKV         = 0.0;   // kV — lower voltage limit
    double maxVoltageKV         = 40.0;  // kV — upper voltage limit
    bool   useDriftCompensation = false; // drift compensation (script control)
    double driftCoefficient     = 0.1;   // k_d coefficient
    double controlGain          = 0.0;   // general control gain

    double esailSolarWindSpeedKMS  = 0.0;
    double esailSolarWindDensity   = 0.0;  // cm^-3
    double esailSolarWindTempEV    = 0.0;




    std::string attachedWindName   = "";

    // --- Group E: MEOE (Diagnostic, read-only) ---
    double meoe_p = 0.0;
    double meoe_f = 0.0;
    double meoe_g = 0.0;
    double meoe_h = 0.0;
    double meoe_k = 0.0;
    double meoe_L = 0.0;

    // Diagnostic (read-only, updated at runtime)
    double esailCharAccelCalc = 0.0;
    double esailPitchAngleDeg = 0.0;
    double esailDistSunAU     = 1.0;
    glm::dvec3 esailNetForceN = {0.0, 0.0, 0.0};
    std::string controllerStatus = "IDLE";

    // ─────────────────────────────────────────────────────────────────────────
    //  updateMEOE
    //  [FIX-1] Removed hardcoded arg_peri = 0.
    //          Full calculation done from ascending node vector.
    //  [FIX-1b] Handled equatorial singularity (node_vec ≈ 0).
    // ─────────────────────────────────────────────────────────────────────────
    void updateMEOE(double mu) {
        if (mu <= 0.0) return;
        glm::dvec3 r = PositionKM;
        glm::dvec3 v = VelocityKMS;
        double r_mag = glm::length(r);
        double v_mag = glm::length(v);
        if (r_mag < 1e-6) return;

        glm::dvec3 h_vec = glm::cross(r, v);
        double h_mag = glm::length(h_vec);
        meoe_p = (h_mag * h_mag) / mu;

        glm::dvec3 e_vec = (1.0 / mu)
            * ((v_mag * v_mag - mu / r_mag) * r - glm::dot(r, v) * v);
        double e_mag = glm::length(e_vec);

        double incl = std::acos(
            glm::clamp(h_vec.z / (h_mag + 1e-12), -1.0, 1.0));

        double v_true = 0.0;
        if (e_mag > 1e-7) {
            v_true = std::acos(glm::clamp(
                glm::dot(glm::normalize(e_vec), glm::normalize(r)), -1.0, 1.0));
            if (glm::dot(r, v) < 0.0) v_true = 2.0 * M_PI - v_true;
        }

        // [FIX-1] Ascending node vector: n = z_inertial × h
        glm::dvec3 z_inertial(0.0, 0.0, 1.0);
        glm::dvec3 node_vec = glm::cross(z_inertial, h_vec);
        double node_mag = glm::length(node_vec);

        double Omega    = 0.0;
        double arg_peri = 0.0;

        if (node_mag > 1e-7) {
            // General case: node_vec.x = -h.y, node_vec.y = h.x
            Omega = std::atan2(h_vec.x, -h_vec.y);
            if (Omega < 0.0) Omega += 2.0 * M_PI;

            if (e_mag > 1e-7) {
                glm::dvec3 n_hat = node_vec / node_mag;
                glm::dvec3 e_hat = e_vec   / e_mag;
                arg_peri = std::acos(
                    glm::clamp(glm::dot(n_hat, e_hat), -1.0, 1.0));
                if (e_vec.z < 0.0) arg_peri = 2.0 * M_PI - arg_peri;
            }
        } else {
            // [FIX-1b] Equatorial singularity (i ≈ 0)
            Omega = 0.0;
            if (e_mag > 1e-7) {
                arg_peri = std::atan2(e_vec.y, e_vec.x);
                if (arg_peri < 0.0) arg_peri += 2.0 * M_PI;
            }
        }

        meoe_f = e_mag * std::cos(arg_peri + Omega);
        meoe_g = e_mag * std::sin(arg_peri + Omega);
        meoe_h = std::tan(incl / 2.0) * std::cos(Omega);
        meoe_k = std::tan(incl / 2.0) * std::sin(Omega);
        meoe_L = Omega + arg_peri + v_true;
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  updateTetherLengthWithAdiabaticInvariant
    //  [FIX-2] cos²(Λ) term is now used in formula.
    //  Formula: l³·ω·cos²(Λ) = constant
    //  ω_new = ω_old · (l_old/l_new)³ · (cos²Λ_old / cos²Λ_new)
    //
    //  newLambdaDeg < 0 → Λ does not change (cos² ratio becomes 1).
    // ─────────────────────────────────────────────────────────────────────────
    void updateTetherLengthWithAdiabaticInvariant(double newLengthKM,
                                                   double newLambdaDeg = -1.0) {
        if (newLengthKM <= 0.1 || esailLengthKM <= 0.1) return;

        double lambdaOld = esailDeflectionAngleDeg;
        double lambdaNew = (newLambdaDeg >= 0.0) ? newLambdaDeg : lambdaOld;

        double cos2_old = std::pow(std::cos(glm::radians(lambdaOld)), 2.0);
        double cos2_new = std::pow(std::cos(glm::radians(lambdaNew)), 2.0);
        if (cos2_new < 1e-9) cos2_new = 1e-9;

        double lengthRatio = esailLengthKM / newLengthKM;

        // [FIX-2] Full adiabatic formula
        double newSpinRate = spinRateRPM
                             * std::pow(lengthRatio, 3.0)
                             * (cos2_old / cos2_new);

        esailLengthKM           = newLengthKM;
        esailDeflectionAngleDeg = lambdaNew;
        spinRateRPM             = newSpinRate;

        for (auto& t : tethers) {
            t.L_km   = esailLengthKM;
            t.Lambda = glm::radians(esailDeflectionAngleDeg);
        }
        updateCharAccel();
    }

    std::shared_ptr<ESailMesh> esailRenderer;
    std::vector<TetherState>   tethers;

    ElectricSail(std::string name, glm::vec3 color)
        : Spacecraft(name, color) {
        tethers.resize(esailTetherCount);
        resetTethers();
        updateCharAccel();
    }

    void resetTethers() {
        tethers.clear();
        tethers.resize(esailTetherCount);
        for (int i = 0; i < esailTetherCount; ++i) {
            tethers[i].phi    = (2.0 * M_PI * i) / esailTetherCount;
            tethers[i].Lambda = glm::radians(esailDeflectionAngleDeg);
            tethers[i].L_km   = esailLengthKM;
            tethers[i].Phi_kV = esailVoltageKV;
        }
    }

    // ── postStep: Called AFTER simulation step (after Spacecraft::updatePhysics)
    // Updates spin phase accumulator with dt (prevents large et×ω float precision loss).
    void postStep(double dt) {
        double omega_e_rad_s = spinRateRPM * (M_PI / 30.0);
        for (auto& t : tethers)
            t.phi = std::fmod(t.phi + omega_e_rad_s * dt, 2.0 * M_PI);
    }

    void updateCharAccel() {
        // esailNetForceN is up to date during simulation, use directly.
        if (glm::length(esailNetForceN) > 0.0) {
            esailCharAccelCalc = (glm::length(esailNetForceN) / esailMassKG) * 1000.0;
        } else {
            // Pre-simulation calculation: PIC formula (cached members)
            double u     = esailSolarWindSpeedKMS * 1000.0; // m/s
            double n     = esailSolarWindDensity  * 1.0e6;  // m^-3
            double T_e_J = esailSolarWindTempEV   * QE;     // J
            if (!attachedWindName.empty()) {
                for (const auto& sw : projectSolarWinds) {
                    if (sw.Name == attachedWindName) {
                        u     = sw.getSpeed() * 1000.0;
                        n     = sw.getDensity(0, 0, 0, 0) * 1.0e6;
                        T_e_J = sw.getTemperatureEV(0, 0, 0) * QE;
                        break;
                    }
                }
            }
            if (u > 1.0 && n > 1.0) {
                double phi_v   = std::max(1.0, esailVoltageKV * 1000.0);
                double r_w     = (esailTetherDiameterUM * 1e-6) / 2.0;
                double lam_D   = std::sqrt((EPS0 * std::max(T_e_J, 1e-30))
                                           / (n * QE * QE));
                double pv      = glm::clamp((MP * u * u) / (QE * phi_v), 0.0, 0.5);
                double denom   = std::max(std::pow(lam_D / r_w, pv) - 1.0, 0.1);
                double f_ci    = (6.18 * MP * u * u * std::sqrt(n * EPS0 * T_e_J))
                                 / (QE * denom);
                double totF    = (double)esailTetherCount * esailLengthKM * 1000.0 * f_ci;
                esailCharAccelCalc = (totF / esailMassKG) * 1000.0;
            } else {
                esailCharAccelCalc = 0.0;
            }
        }

        // Spin-Coning consistency check — f_ci_ref from PIC model
        double u_ref   = esailSolarWindSpeedKMS * 1000.0;
        double n_ref   = esailSolarWindDensity  * 1.0e6;
        double T_ref   = esailSolarWindTempEV   * QE;
        double phi_v   = std::max(1.0, esailVoltageKV * 1000.0);
        double r_w     = (esailTetherDiameterUM * 1e-6) / 2.0;
        double sin_lam = std::sin(glm::radians(esailDeflectionAngleDeg));

        if (u_ref > 1.0 && n_ref > 1.0 && sin_lam > 1e-5) {
            double lam_D   = std::sqrt((EPS0 * std::max(T_ref, 1e-30)) / (n_ref * QE * QE));
            double pv      = glm::clamp((MP * u_ref * u_ref) / (QE * phi_v), 0.0, 0.5);
            double denom   = std::max(std::pow(lam_D / r_w, pv) - 1.0, 0.1);
            double f_ci_ref= (6.18 * MP * u_ref * u_ref * std::sqrt(n_ref * EPS0 * T_ref))
                             / (QE * denom);
            double lam_coeff = ((double)esailTetherCount * f_ci_ref) / esailMassKG;
            double cos_a     = std::cos(glm::radians(esailPitchAngleDeg));
            double we_exp    = std::sqrt(std::abs(lam_coeff * cos_a
                              / ((esailLengthKM * 1000.0) * sin_lam)));
            double we_rpm    = we_exp * (30.0 / M_PI);
            if (std::abs(we_rpm - spinRateRPM) / (spinRateRPM + 1e-9) > 0.20)
                controllerStatus = "WARNING: Inconsistent Spin. Expected: "
                                   + std::to_string(we_rpm) + " RPM";
            else
                controllerStatus = "NOMINAL";
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  computeAcceleration — Main physics loop
    //
    //  [FIX-3] delta_g is now applied to voltage as g_total = g_a + δg.
    //          (1 + delta_g) multiplier on force is removed.
    //  [FIX-4] I-tether `continue` removed. All tethers produce thrust.
    //          T-tether: modulated voltage. I-tether: baseline voltage.
    //  [FIX-5] V_eff_ratio orphan removed.
    //  [FIX-6] Parker spiral: uses v_wind, sw.getVelocity() direction.
    //  [FIX-7] T_e is queried dynamically from SolarWindField.
    //  [FIX-8] Variable shadowing: omega_orbit_rad_s ← different name from member variable.
    // ─────────────────────────────────────────────────────────────────────────
    glm::dvec3 computeAcceleration(double et,
                                    const std::vector<CelestialBody>& bodies,
                                    double dt_offset = 0.0) override
    {
        glm::dvec3 totalAcc = Spacecraft::computeAcceleration(et, bodies, dt_offset);

        const CelestialBody* sunBody    = nullptr;
        const CelestialBody* centerBody = nullptr;
        for (const auto& b : bodies) {
            if (b.SpiceID == 10)               sunBody    = &b;
            if (b.SpiceID == centerBodySpiceID) centerBody = &b;
        }
        if (!sunBody || !centerBody) return totalAcc;

        glm::dvec3 centerPosAtT = centerBody->PositionKM + centerBody->VelocityKMS * dt_offset;
        glm::dvec3 scAbsPos = centerPosAtT + PositionKM;
        
        glm::dvec3 sunPosAtT = sunBody->PositionKM + sunBody->VelocityKMS * dt_offset;
        glm::dvec3 r_sun_sc = scAbsPos - sunPosAtT;
        double dist_sun_km  = glm::length(r_sun_sc);
        if (dist_sun_km < 1e-6) return totalAcc;

        esailDistSunAU = dist_sun_km / AU_KM;

        // ── 1. Environment Synchronization ─────────────────────────────────────────
        double     u_sw      = esailSolarWindSpeedKMS * 1000.0; // m/s
        double     n_e       = esailSolarWindDensity  * 1.0e6;  // m^-3
        double     T_e_J     = esailSolarWindTempEV   * QE;     // J
        glm::dvec3 v_wind_kms;                                   // km/s

        bool windFound = false;
        for (auto& sw : projectSolarWinds) {
            if (sw.Name != attachedWindName) continue;

            // [FIX-6] Preserve Parker spiral direction — do not take only magnitude
            glm::dvec3 vel_kms = sw.getVelocity(
                scAbsPos.x, scAbsPos.y, scAbsPos.z, et);
            double speed_kms = glm::length(vel_kms);
            if (speed_kms > 1e-9) {
                v_wind_kms = vel_kms;       // preserve direction
                u_sw       = speed_kms * 1000.0; // m/s magnitude
            }

            // [FIX-7] T_e dynamic query
            n_e   = sw.getDensity(scAbsPos.x, scAbsPos.y, scAbsPos.z, et) * 1.0e6;
            T_e_J = sw.getTemperatureEV(scAbsPos.x, scAbsPos.y, scAbsPos.z) * QE;
            windFound = true;
            break;
        }

        if (!windFound) {
            v_wind_kms = glm::normalize(r_sun_sc) * (u_sw / 1000.0);
        }
        
        // --- Diagnostic Cache ---
        esailSolarWindSpeedKMS = u_sw / 1000.0;
        esailSolarWindDensity = n_e / 1.0e6;
        esailSolarWindTempEV = T_e_J / QE;


        // ── 2. Relative Velocity (v_app = v_wind − v_sc) ────────────────────────────
        glm::dvec3 scAbsVel  = centerBody->VelocityKMS + VelocityKMS;
        glm::dvec3 v_app     = v_wind_kms - scAbsVel;
        double v_app_mag     = glm::length(v_app);
        // Soft transition (0.05–0.15 km/s): smooth blend instead of discontinuous cutoff
        double blend = glm::clamp((v_app_mag - 0.05) / 0.10, 0.0, 1.0);
        if (blend < 1e-6) return totalAcc;
        glm::dvec3 v_app_dir = v_app / v_app_mag;

        // ── 4. Orientation (M_body) ───────────────────────────────────────────────
        glm::dvec3 z_axis(
            std::cos(esailSpinDEC) * std::cos(esailSpinRA),
            std::cos(esailSpinDEC) * std::sin(esailSpinRA),
            std::sin(esailSpinDEC));
        esailPitchAngleDeg = glm::degrees(
            std::acos(glm::clamp(glm::dot(z_axis, v_app_dir), -1.0, 1.0)));

        glm::dvec3 tmp    = (std::abs(z_axis.z) < 0.99)
                            ? glm::dvec3(0, 0, 1) : glm::dvec3(1, 0, 0);
        glm::dvec3 y_axis = glm::normalize(glm::cross(z_axis, tmp));
        glm::dvec3 x_axis = glm::normalize(glm::cross(y_axis, z_axis));
        glm::dmat3 M_body = glm::dmat3(x_axis, y_axis, z_axis);

        // ── 5. Pre-Loop Constants ─────────────────────────────────────────
        double omega_e_rad_s = spinRateRPM * (M_PI / 30.0);

        // Debye length — physical formula: λ_D = sqrt(ε₀ T_e / (n_e q²))
        // T_e_J and n_e already queried from SolarWind; no hardcoded value needed.
        double lambda_e_r = std::sqrt(
            (EPS0 * std::max(T_e_J, 1e-30))
            / (std::max(n_e, 1.0) * QE * QE)
        ); // meters
        double r_w = (esailTetherDiameterUM * 1e-6) / 2.0;

        // ── 6. Per-Tether Loop ─────────────────────────────────────────────
        if (tethers.size() != (size_t)esailTetherCount) resetTethers();
        glm::dvec3 a_esail(0.0);
        glm::dvec3 totalForceVec(0.0, 0.0, 0.0);

        // All tethers run with fixed nominal voltage — min/max limits applied
        double phi_volts = glm::clamp(
            esailVoltageKV * 1000.0,
            std::max(1.0, minVoltageKV * 1000.0),
            maxVoltageKV * 1000.0
        ); // V

        // Force magnitude — PIC model (Li et al.)
        // Distance scale not applied: SolarWind data provides position dependent n/u.
        double K_c       = 6.18;
        double power_val = glm::clamp((MP * u_sw * u_sw) / (QE * phi_volts), 0.0, 0.5);
        double denom     = std::max(std::pow(lambda_e_r / r_w, power_val) - 1.0, 0.1);
        double f_ci      = (K_c * MP * u_sw * u_sw
                            * std::sqrt(n_e * EPS0 * T_e_J))
                           / (QE * denom);
        double F_scalar  = f_ci * blend; // blend: soft v_app transition

        for (int i = 0; i < esailTetherCount; i++) {
            TetherState& t = tethers[i];

            // Spin angle: t.phi is updated each step via postStep().
            // Used directly here — no et×ω float loss.
            double current_phi = t.phi;
            double F_mag_i     = F_scalar * (t.L_km * 1000.0);

            // ── Lag angle (β) ─────────────────────────────────────────────
            if (spinRateRPM > 0.01) {
                // m_eff = remote unit + half tether mass (distributed mass)
                double tether_cross = M_PI * r_w * r_w;
                double tether_mass  = tether_cross * (t.L_km * 1000.0)
                                      * esailTetherDensityKgM3;
                double m_eff = esailRemoteUnitMassKG + tether_mass * 0.5;

                double tan_beta = F_mag_i
                    / (m_eff * omega_e_rad_s * omega_e_rad_s
                       * (t.L_km * 1000.0) + 1e-9);
                t.beta = std::atan(glm::clamp(tan_beta, -0.5, 0.5));
            }

            // ── Tether direction vector ─────────────────────────────────────────────
            glm::dvec3 u_b(
                std::cos(t.Lambda) * std::cos(current_phi + t.beta),
                std::cos(t.Lambda) * std::sin(current_phi + t.beta),
                -std::sin(t.Lambda));
            glm::dvec3 u_inertial = M_body * u_b;

            // ── Acceleration contribution ──────────────────────────────────────────────────
            // F comes from the component of solar wind v_app perpendicular to tether axis.
            glm::dvec3 force_dir = v_app_dir
                - glm::dot(v_app_dir, u_inertial) * u_inertial;

            // [FIX-3] g_total is already in phi_volts. NO extra (1+delta_g).
            glm::dvec3 tetherForceN = F_mag_i * force_dir;
            totalForceVec += tetherForceN;
            a_esail += ((tetherForceN / esailMassKG) / 1000.0);
        }

        esailNetForceN     = totalForceVec;
        esailCharAccelCalc = (glm::length(totalForceVec) / esailMassKG) * 1000.0; // mm/s²
        updateMEOE(centerBody->GM);
        return totalAcc + a_esail;
    }
};
