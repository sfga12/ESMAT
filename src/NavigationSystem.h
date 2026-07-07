#pragma once
#include <glm/glm.hpp>
#include <cmath>

inline double StumpffC(double z) {
    if (z > 0) return (1.0 - std::cos(std::sqrt(z))) / z;
    if (z < 0) return (std::cosh(std::sqrt(-z)) - 1.0) / (-z);
    return 1.0 / 2.0;
}

inline double StumpffS(double z) {
    if (z > 0) return (std::sqrt(z) - std::sin(std::sqrt(z))) / std::pow(z, 1.5);
    if (z < 0) return (std::sinh(std::sqrt(-z)) - std::sqrt(-z)) / std::pow(-z, 1.5);
    return 1.0 / 6.0;
}

struct LambertResult {
    glm::dvec3 v1;
    glm::dvec3 v2;
    bool success;
};

// Solves Lambert's Problem using Universal Variables and Bisection method
inline LambertResult SolveLambert(const glm::dvec3& r1, const glm::dvec3& r2, double tof_sec, double mu, int centerBodyID = 0, bool prograde = true) {
    LambertResult result;
    result.success = false;

    double r1_mag = glm::length(r1);
    double r2_mag = glm::length(r2);
    
    // Singularity Nudge: If target is nearly radial (0 or 180 deg), nudge slightly to avoid A=0
    glm::dvec3 r2_nudged = r2;
    double cos_dNu_check = glm::dot(r1, r2) / (r1_mag * r2_mag);
    if (std::abs(cos_dNu_check) > 0.9999) {
        r2_nudged += glm::dvec3(0.001, 0.001, 0.001); // 1 meter nudge
        r2_mag = glm::length(r2_nudged);
    }

    double cos_dNu = glm::dot(r1, r2_nudged) / (r1_mag * r2_mag);
    cos_dNu = glm::clamp(cos_dNu, -1.0, 1.0);
    
    double dNu = std::acos(cos_dNu);
    
    // Prograde/retrograde detection using transfer plane normal.
    // Convention: "prograde" (short-way) = dNu <= pi (the geometrically shorter arc).
    // "retrograde" (long-way, prograde=false) = dNu > pi.
    // We do NOT use a fixed Z-axis reference because it fails when the transfer plane
    // is inclined (e.g. Earth-Moon geometry at departure near +Y axis gives cross(r1,r2)
    // pointing in -Z, which the old code misidentified as "retrograde", then added 267°
    // to the transfer angle instead of using the correct 93° short-way arc).
    // 
    // The short-way arc is always acos(cos_dNu) ∈ [0, π].
    // If long-way is desired (prograde=false), we flip to 2π - dNu.
    if (!prograde) dNu = 2.0 * 3.14159265358979323846 - dNu;


    double A = std::sin(dNu) * std::sqrt((r1_mag * r2_mag) / (1.0 - std::cos(dNu)));
    if (A == 0.0) return result;

    double z_low = -4.0 * 3.141592653589 * 3.141592653589;
    double z_high = 4.0 * 3.141592653589 * 3.141592653589;
    double z = 0.0;
    
    double C = 0.0, S = 0.0, y = 0.0;
    bool converged = false;
    double TOL = 1e-6;

    for (int i = 0; i < 500; i++) {
        C = StumpffC(z);
        S = StumpffS(z);
        y = r1_mag + r2_mag + A * (z * S - 1.0) / std::sqrt(C);
        
        if (y < 0.0) {
            // z is too large
            z_high = z;
            z = (z_low + z_high) / 2.0;
            continue;
        }
        
        double x = std::sqrt(y / C);
        double tof_calc = (std::pow(x, 3) * S + A * std::sqrt(y)) / std::sqrt(mu);
        
        if (std::abs(tof_calc - tof_sec) < TOL) {
            converged = true;
            break;
        }
        
        if (tof_calc <= tof_sec) {  
            // T(z) is an increasing function of z, so if calc < target, we need to increase z
            z_low = z;
        } else {
            z_high = z; 
        }
        z = (z_low + z_high) / 2.0;
    }

    if (!converged) return result;

    double f = 1.0 - y / r1_mag;
    double g = A * std::sqrt(y / mu);
    double g_dot = 1.0 - y / r2_mag;

    result.v1 = (r2 - r1 * f) / g;
    result.v2 = (r2 * g_dot - r1) / g;
    result.success = true;

    return result;
}

// Analytically propagates an unperturbed Keplerian orbit (e < 1) by dt seconds
inline glm::dvec3 PropagateKepler(const glm::dvec3& r0, const glm::dvec3& v0, double dt, double mu, glm::dvec3& v_out) {
    double r0_mag = glm::length(r0);
    double v0_mag = glm::length(v0);
    
    // Specific orbital energy
    double eps = (v0_mag * v0_mag) / 2.0 - mu / r0_mag;
    
    // Hyperbolic / Parabolic bypass (returns naive linear fallback)
    if (eps >= 0.0) {
        v_out = v0;
        return r0 + v0 * dt; 
    }
    
    double a = -mu / (2.0 * eps);
    double n = std::sqrt(mu / (a * a * a));
    
    // Eccentricity vector
    double rdotv = glm::dot(r0, v0);
    glm::dvec3 e_vec = ( (v0_mag * v0_mag - mu / r0_mag) * r0 - rdotv * v0 ) / mu;
    double e = glm::length(e_vec);
    
    // Fallback if numerical issues make e >= 1
    if (e >= 1.0) {
        v_out = v0; return r0;
    }
    
    // Initial Eccentric Anomaly E0
    double sinE0 = rdotv / (e * std::sqrt(mu * a));
    double cosE0 = (1.0 - r0_mag / a) / e;
    double E0 = std::atan2(sinE0, cosE0);
    
    // Initial Mean Anomaly M0
    double M0 = E0 - e * std::sin(E0);
    
    // Future Mean Anomaly M(t)
    double Mt = M0 + n * dt;
    
    // Newton-Raphson iteration for future Eccentric Anomaly E(t)
    double E = Mt;
    for (int i = 0; i < 20; ++i) {
        double f = E - e * std::sin(E) - Mt;
        double f_prime = 1.0 - e * std::cos(E);
        double dE = f / f_prime;
        E -= dE;
        if (std::abs(dE) < 1e-8) break;
    }
    
    // Exact f and g coefficients
    double dfE = E - E0;
    double f_coeff = 1.0 - (a / r0_mag) * (1.0 - std::cos(dfE));
    double g_coeff = dt - std::sqrt(a * a * a / mu) * (dfE - std::sin(dfE));
    
    glm::dvec3 rt = f_coeff * r0 + g_coeff * v0;
    double rt_mag = glm::length(rt);
    
    double f_dot = -(std::sqrt(mu * a) / (rt_mag * r0_mag)) * std::sin(dfE);
    double g_dot = 1.0 - (a / rt_mag) * (1.0 - std::cos(dfE));
    
    v_out = f_dot * r0 + g_dot * v0;
    return rt;
}
