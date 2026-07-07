#pragma once
#include <glm/glm.hpp>
#include <string>
#include <cmath>
#include <vector>
#include <fstream>
#include <cstring>
#include <cstdint>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
//  .esmatwind binary format v1
//
//  Header (256 byte, little-endian):
//    char[16]   magic = "ESMATWIND_4D\0\0\0"
//    uint32     version = 1
//    char[128]  name (null-padded)
//    int64      refdate_unix (UTC, seconds since 1970-01-01)
//    int32      nt, nr, nphi
//    uint8[88]  _padding
//
//  Data (float32, little-endian, immediately after):
//    float32[nt]           t_array   (seconds from refdate)
//    float32[nr]           r_array   (meters)
//    float32[nphi]         phi_array (radians, 0..2pi)
//    float32[nt*nphi*nr]   density   (cm^-3, layout: [t][phi][r])
//    float32[nt*nphi*nr]   velocity  (km/s radial, layout: [t][phi][r])
//
//  Out of bounds query → returns 0.0 (not an error).
// ─────────────────────────────────────────────────────────────────────────────

#pragma pack(push, 1)
struct ESMATWindHeader_v1 {
    char     magic[16];
    uint32_t version;
    char     name[128];
    int64_t  refdate_unix;
    int32_t  nt, nr, nphi;
    uint8_t  _pad[88];
};
#pragma pack(pop)
static_assert(sizeof(ESMATWindHeader_v1) == 256, "Header boyutu 256 olmali");

// ─────────────────────────────────────────────────────────────────────────────

struct SolarWindField {
    std::string Name        = "Unnamed Wind";
    bool        isDataLoaded = false;   // true → 4D ENLIL grid active

    // ── Uniform (fallback) parameters ──────────────────────────────────────
    glm::dvec3 velocityKMS   = {400.0, 0.0, 0.0};  // km/s, J2000
    double     density       = 7.0;                  // cm^-3
    double     temperatureEV = 12.0;                 // eV

    // ── 4D ENLIL Grid ────────────────────────────────────────────────────────
    struct ENLILGrid {
        int32_t  nt = 0, nr = 0, nphi = 0;
        int64_t  refdate_unix = 0;          // Unix timestamp (UTC seconds)

        std::vector<float> t_s;             // [nt]   seconds from refdate
        std::vector<float> r_m;             // [nr]   meters
        std::vector<float> phi_rad;         // [nphi] radians

        std::vector<float> density_grid;    // [nt*nphi*nr] cm^-3
        std::vector<float> velocity_grid;   // [nt*nphi*nr] km/s (radial)

        // Inline indeks: [t][phi][r] → flat index
        inline int idx(int ti, int phii, int ri) const {
            return ti * nphi * nr + phii * nr + ri;
        }
    };
    ENLILGrid grid;

    // ── Loading ───────────────────────────────────────────────────────────────
    bool load4DGrid(const std::string& filePath) {
        std::ifstream f(filePath, std::ios::binary);
        if (!f.is_open()) return false;

        ESMATWindHeader_v1 hdr;
        f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
        if (!f.good()) return false;

        // Magic check
        static const char EXPECTED_MAGIC[16] = "ESMATWIND_4D\0\0\0";
        if (std::memcmp(hdr.magic, EXPECTED_MAGIC, 16) != 0) return false;
        if (hdr.version != 1) return false;

        // Name
        hdr.name[127] = '\0';
        Name = std::string(hdr.name);

        grid.nt           = hdr.nt;
        grid.nr           = hdr.nr;
        grid.nphi         = hdr.nphi;
        grid.refdate_unix = hdr.refdate_unix;

        if (grid.nt <= 0 || grid.nr <= 0 || grid.nphi <= 0) return false;

        const int64_t Ndata = (int64_t)grid.nt * grid.nphi * grid.nr;

        // Axes
        grid.t_s.resize(grid.nt);
        grid.r_m.resize(grid.nr);
        grid.phi_rad.resize(grid.nphi);

        auto readVec = [&](std::vector<float>& v) {
            f.read(reinterpret_cast<char*>(v.data()), v.size() * sizeof(float));
        };
        readVec(grid.t_s);
        readVec(grid.r_m);
        readVec(grid.phi_rad);

        // Grid data
        grid.density_grid.resize(Ndata);
        grid.velocity_grid.resize(Ndata);
        readVec(grid.density_grid);
        readVec(grid.velocity_grid);

        if (!f.good()) return false;

        isDataLoaded = true;
        return true;
    }

    // ── Inner interpolation helpers ────────────────────────────────────────

    // Binary search — finds lower index in sorted array
    static int _lowerBound(const std::vector<float>& arr, float val) {
        int lo = 0, hi = (int)arr.size() - 1;
        while (lo < hi - 1) {
            int mid = (lo + hi) / 2;
            if (arr[mid] <= val) lo = mid; else hi = mid;
        }
        return lo;
    }

    // Trilinear interpolation: (r_m, phi_rad, t_unix_s) → scalar value
    // Layout: density_grid[t][phi][r]
    // phi periodic: 0..2pi
    float _interp(const std::vector<float>& data,
                  double r_metre, double phi_r, double t_unix_s) const
    {
        const ENLILGrid& g = grid;
        if (g.nt == 0) return 0.0f;

        // t: offset from refdate in seconds
        double t_off = t_unix_s - (double)g.refdate_unix;

        // Out of bounds → 0
        if (t_off < g.t_s.front() || t_off > g.t_s.back()) return 0.0f;
        if (r_metre < g.r_m.front() || r_metre > g.r_m.back())  return 0.0f;

        // phi: 0..2pi wrapping
        phi_r = std::fmod(phi_r, 2.0 * M_PI);
        if (phi_r < 0.0) phi_r += 2.0 * M_PI;

        // Time index
        int ti = _lowerBound(g.t_s, (float)t_off);
        ti = std::min(ti, g.nt - 2);
        float wt = (float)((t_off - g.t_s[ti]) / (g.t_s[ti+1] - g.t_s[ti] + 1e-30f));
        wt = std::max(0.0f, std::min(1.0f, wt));

        // Radial index
        int ri = _lowerBound(g.r_m, (float)r_metre);
        ri = std::min(ri, g.nr - 2);
        float wr = (float)((r_metre - g.r_m[ri]) / (g.r_m[ri+1] - g.r_m[ri] + 1e-30f));
        wr = std::max(0.0f, std::min(1.0f, wr));

        // phi index (supports periodic wrapping)
        int pi0 = _lowerBound(g.phi_rad, (float)phi_r);
        pi0 = std::min(pi0, g.nphi - 1);
        int pi1 = (pi0 + 1) % g.nphi;   // wrapping transition
        float dph = g.phi_rad[pi0];
        float dph1 = (pi1 > pi0) ? g.phi_rad[pi1]
                                  : g.phi_rad[pi0] + (float)(2.0 * M_PI);
        float wphi = (float)((phi_r - dph) / (dph1 - dph + 1e-30f));
        wphi = std::max(0.0f, std::min(1.0f, wphi));

        // 8-point trilinear
        auto V = [&](int t_, int p_, int r_) -> float {
            return data[g.idx(
                std::min(t_, g.nt-1),
                p_ % g.nphi,
                std::min(r_, g.nr-1)
            )];
        };

        float v000 = V(ti,   pi0, ri  );  float v100 = V(ti+1, pi0, ri  );
        float v010 = V(ti,   pi1, ri  );  float v110 = V(ti+1, pi1, ri  );
        float v001 = V(ti,   pi0, ri+1);  float v101 = V(ti+1, pi0, ri+1);
        float v011 = V(ti,   pi1, ri+1);  float v111 = V(ti+1, pi1, ri+1);

        float a0 = v000*(1-wt) + v100*wt;
        float a1 = v010*(1-wt) + v110*wt;
        float a2 = v001*(1-wt) + v101*wt;
        float a3 = v011*(1-wt) + v111*wt;

        float b0 = a0*(1-wphi) + a1*wphi;
        float b1 = a2*(1-wphi) + a3*wphi;

        return b0*(1-wr) + b1*wr;
    }

    // J2000 Cartesian km → heliocentric spherical (r_m, phi_rad)
    // x, y, z: Heliocentric, km
    static void _toSpherical(double x_km, double y_km, double z_km,
                              double& r_m, double& phi_rad)
    {
        r_m     = std::sqrt(x_km*x_km + y_km*y_km + z_km*z_km) * 1000.0;  // km → m
        phi_rad = std::atan2(y_km, x_km);
        if (phi_rad < 0.0) phi_rad += 2.0 * M_PI;
    }

    // ─── Query Methods ─────────────────────────────────────────────────────
    // x, y, z: Heliocentric J2000 (km)
    // time_et: SPICE ET (seconds from J2000 epoch)
    //           SPICE J2000 epoch = 2000-01-01T11:58:55.816 UTC
    //           Unix epoch       = 1970-01-01T00:00:00.000 UTC
    //           Difference       = 946727935.816 seconds
    static constexpr double ET_TO_UNIX = 946727935.816;

    glm::dvec3 getVelocity(double x, double y, double z, double time_et) const {
        if (isDataLoaded) {
            double r_m, phi;
            _toSpherical(x, y, z, r_m, phi);
            double t_unix = time_et + ET_TO_UNIX;
            float  v_kms  = _interp(grid.velocity_grid, r_m, phi, t_unix);
            if (v_kms < 0.0f) v_kms = 0.0f;
            // Direction: Radial from Sun (velocity vector is radial)
            double r_km = r_m / 1000.0;
            if (r_km > 1e-6) {
                double inv = (double)v_kms / r_km;
                return glm::dvec3(x * inv, y * inv, z * inv);
            }
            return glm::dvec3(v_kms, 0.0, 0.0);
        }
        
        // Uniform (Fallback) Solar Wind Physical Correction:
        // Solar wind always propagates radially (spherically) outward from the Sun.
        double r_km = std::sqrt(x*x + y*y + z*z);
        double speed = glm::length(velocityKMS);
        if (r_km > 1.0) {
            double inv = speed / r_km;
            return glm::dvec3(x * inv, y * inv, z * inv);
        }
        return glm::dvec3(speed, 0.0, 0.0);
    }

    double getDensity(double x, double y, double z, double time_et) const {
        if (isDataLoaded) {
            double r_m, phi;
            _toSpherical(x, y, z, r_m, phi);
            double t_unix = time_et + ET_TO_UNIX;
            float  n      = _interp(grid.density_grid, r_m, phi, t_unix);
            return (double)(n < 0.0f ? 0.0f : n);
        }
        
        // Uniform (Fallback) Solar Wind Physical Correction:
        // Density entered in UI is the density at 1 AU (Earth orbit) distance.
        // Solar wind density decreases inversely proportional to the square of distance (1/r^2).
        double r_km = std::sqrt(x*x + y*y + z*z);
        if (r_km < 1.0) r_km = 1.0;
        double r_au = r_km / 149597870.7; // 1 AU in km
        return density / (r_au * r_au);
    }

    double getTemperatureEV(double /*x*/, double /*y*/, double /*z*/) const {
        // Temperature not in 4D grid, uniform value is used
        return temperatureEV;
    }

    // Speed helper (km/s)
    double getSpeed() const { return glm::length(velocityKMS); }

    // Info string
    std::string infoString() const {
        if (!isDataLoaded)
            return "[Uniform]  v=" + std::to_string((int)glm::length(velocityKMS))
                   + " km/s  n=" + std::to_string(density) + " cm^-3";
        return "[ENLIL 4D]  nt=" + std::to_string(grid.nt)
               + " nr=" + std::to_string(grid.nr)
               + " nphi=" + std::to_string(grid.nphi);
    }
};
