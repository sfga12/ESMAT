#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <glm/glm.hpp>
#include "CelestialBody.h"

#include "Spacecraft.h"

// -----------------------------------------------------------------------
//  Axis/Channel Source
// -----------------------------------------------------------------------
enum class AxisSource { SystemDay, SystemTime, SystemDate, BodyAttr, CraftAttr };

struct AxisChannel {
    AxisSource  source   = AxisSource::SystemDay;
    std::string bodyName; // e.g. "EARTH"
    int         bodyID   = -1;      // e.g. 399
    std::string attr;     // e.g. "dist_sun_km"

    std::string legend() const {
        switch (source) {
            case AxisSource::SystemDay:  return "Day";
            case AxisSource::SystemTime: return "ET (sec)";
            case AxisSource::SystemDate: return "Date (UTC)";
            case AxisSource::BodyAttr:   
                return (bodyID != -1) ? (std::to_string(bodyID) + "." + attr) : (bodyName + "." + attr);
            case AxisSource::CraftAttr:
                return "Craft(" + std::to_string(bodyID) + ")." + attr;
        }
        return "";
    }
    bool isDateAxis() const { return source == AxisSource::SystemDate; }
};

// -----------------------------------------------------------------------
//  LineDef — one line within a plot
// -----------------------------------------------------------------------
struct LineDef {
    AxisChannel yAxis;
    std::string label;
    glm::vec4   color = glm::vec4(0,0,0,-1); // IMPLOT_AUTO_COL (-1 alpha)
    std::vector<double> yValues;
};

// -----------------------------------------------------------------------
//  PlotDef — one plot window (figure)
// -----------------------------------------------------------------------
struct PlotDef {
    std::string title;
    AxisChannel xAxis;
    std::vector<LineDef> lines;

    // Recorded data
    std::vector<double> xValues;

    // Persistence
    std::ofstream plotFile;
    bool headerWritten = false;

    // Custom Axis Labels
    std::string xlabel;
    std::string ylabel;

    // Non-copyable (ofstream), but movable
    PlotDef() = default;
    PlotDef(const PlotDef&) = delete;
    PlotDef& operator=(const PlotDef&) = delete;
    PlotDef(PlotDef&&) = default;
    PlotDef& operator=(PlotDef&&) = default;

    bool isOpen = false; // UI window state
    bool showExtrema = true; // Whether to show MAX/MIN annotations

    static constexpr int MAX_SAMPLES = 1000000; // Increased for long-term simulations (e.g. Voyager 1977-2035)
    bool isFull() const { return (int)xValues.size() >= MAX_SAMPLES; }

    void clear() {
        xValues.clear();
        for (auto& l : lines) l.yValues.clear();
        if (plotFile.is_open()) plotFile.close();
        headerWritten = false;
    }

    void initData() {
        for (auto& l : lines) l.yValues.clear();
    }

    bool exportCSV(const std::string& path) const {
        std::ofstream f(path);
        if (!f.is_open()) return false;
        f << xAxis.legend();
        for (auto& l : lines) f << "," << (l.label.empty() ? l.yAxis.legend() : l.label);
        f << "\n";
        for (size_t i = 0; i < xValues.size(); ++i) {
            f << xValues[i];
            for (auto& l : lines) {
                f << ",";
                if (i < l.yValues.size()) f << l.yValues[i];
            }
            f << "\n";
        }
        return true;
    }
};

// -----------------------------------------------------------------------
//  ReportDef — one report() command from the script
// -----------------------------------------------------------------------
struct ReportDef {
    std::string filename;
    std::vector<AxisChannel> columns;
    std::ofstream fileStream;
    bool headerWritten = false;

    // Non-copyable (ofstream)
    ReportDef() = default;
    ReportDef(const ReportDef&) = delete;
    ReportDef& operator=(const ReportDef&) = delete;
    ReportDef(ReportDef&&) = default;
    ReportDef& operator=(ReportDef&&) = default;
};

// -----------------------------------------------------------------------
//  OutputScript — parses and executes an .esmatscript file
// -----------------------------------------------------------------------
struct OutputScript {
    std::vector<PlotDef>   plots;
    std::vector<ReportDef> reports;
    double      simStartET = 0.0;
    std::string lastError;

    void setSimStart(double et) { simStartET = et; }

    // Parse script text → populate plots / reports
    bool parse(const std::string& text, const std::vector<CelestialBody>* planets = nullptr, const std::vector<std::shared_ptr<Spacecraft>>* spacecrafts = nullptr);

    // Call after parse() to open report files and init data vectors
    void initForSim(const std::string& projectFolder);

    // Call each sim step — fill plots and report streams
    void record(double et, const std::vector<CelestialBody>& planets, const std::vector<std::shared_ptr<Spacecraft>>& spacecrafts);

    // Clear recorded data (keep plot/report definitions, close reports)
    void clear();

    // Close all report and plot file streams
    void closeOutputs();

    // Default script template (shown in editor on new project)
    static std::string defaultTemplate();

private:
    static std::string trim(const std::string& s);

    // Split `s` by `delim`, respecting parenthesis depth and quotes
    static std::vector<std::string> splitTopLevel(const std::string& s, char delim);

    // Extract title + key=value pairs from a function call string
    bool parseFuncArgs(const std::string& stmt, std::string& title,
                       std::vector<std::pair<std::string,std::string>>& kv);

    // Parse a single axis channel expression (esmat.xxx or ESMATObject(...).attr or ESMATCraft(...).attr)
    bool parseChannel(const std::string& expr, AxisChannel& out, const std::vector<CelestialBody>* planets = nullptr, const std::vector<std::shared_ptr<Spacecraft>>* spacecrafts = nullptr);

    // Evaluate a channel value at a given sim step
    bool getChannelValue(const AxisChannel& ch, double et,
                         const std::vector<CelestialBody>& planets,
                         const std::vector<std::shared_ptr<Spacecraft>>& spacecrafts, double& val) const;

    // Evaluate a named attribute for a body
    static double getBodyAttr(const CelestialBody& body, const std::string& attr);
    
    // Evaluate a named attribute for a spacecraft
    static double getCraftAttr(const Spacecraft& sc, const std::string& attr, const std::vector<CelestialBody>& planets);

    // Parse color string to RGBA
    static glm::vec4 parseColor(const std::string& name);
};

// -----------------------------------------------------------------------
//  Inline Implementations
// -----------------------------------------------------------------------

inline std::string OutputScript::trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

inline std::vector<std::string> OutputScript::splitTopLevel(const std::string& s, char delim) {
    std::vector<std::string> res;
    int depth = 0; bool inStr = false; char strChar = 0;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (inStr) {
            cur += c;
            if (c == strChar) inStr = false;
        } else if (c == '"' || c == '\'') {
            inStr = true; strChar = c; cur += c;
        } else if (c == '(' || c == '[') { depth++; cur += c; }
        else if (c == ')' || c == ']')   { depth--; cur += c; }
        else if (c == delim && depth == 0) { res.push_back(trim(cur)); cur.clear(); }
        else { cur += c; }
    }
    if (inStr) {
        // Find line/context: this is hard without more info, so we just set error.
        // We'll return an empty vector and set an error in lastError.
        return {}; 
    }
    res.push_back(trim(cur));
    return res;
}

inline bool OutputScript::parseFuncArgs(const std::string& stmt, std::string& title,
                                         std::vector<std::pair<std::string,std::string>>& kv) {
    size_t po = stmt.find('('), pc = stmt.rfind(')');
    if (po == std::string::npos || pc == std::string::npos) return false;
    auto args = splitTopLevel(stmt.substr(po + 1, pc - po - 1), ',');
    if (args.empty() && stmt.substr(po + 1, pc - po - 1).find_first_not_of(" \t\n\r") != std::string::npos) {
        lastError = "Syntax error in arguments (possibly unclosed quote).";
        return false;
    }
    if (args.empty()) return false;

    size_t startIdx = 0;
    std::string t = trim(args[0]);
    // If first arg has '=', it's a key-value, not a title
    if (t.find('=') == std::string::npos) {
        if (t.size() < 2 || (t.front() != '"' && t.front() != '\'') || t.back() != t.front()) {
            lastError = "Position argument (title/filename) must be a strictly quoted string. Found: " + t;
            return false;
        }
        title = t.substr(1, t.size() - 2);
        startIdx = 1;
    } else {
        title = "Plot"; // Default title if first arg is a kv
    }

    for (size_t i = startIdx; i < args.size(); ++i) {
        auto& a = args[i];
        size_t eq = a.find('=');
        if (eq == std::string::npos) continue;
        kv.push_back({ trim(a.substr(0, eq)), trim(a.substr(eq + 1)) });
    }
    return true;
}

inline bool OutputScript::parseChannel(const std::string& expr, AxisChannel& out, const std::vector<CelestialBody>* planets, const std::vector<std::shared_ptr<Spacecraft>>* spacecrafts) {
    std::string e = trim(expr);
    if (e == "esmat.day")  { out.source = AxisSource::SystemDay;  return true; }
    if (e == "esmat.time") { out.source = AxisSource::SystemTime; return true; }
    if (e == "esmat.date") { out.source = AxisSource::SystemDate; return true; }

    size_t eo = e.find("ESMATObject(");
    size_t co = e.find("ESMATCraft(");
    
    if (eo != std::string::npos || co != std::string::npos) {
        bool isCraft = (co != std::string::npos);
        size_t startPos = isCraft ? co : eo;
        size_t op = e.find('(', startPos);
        size_t cp = e.find(')', op);
        if (op == std::string::npos || cp == std::string::npos) { lastError = "Invalid object syntax: " + e; return false; }
        
        std::string inner = trim(e.substr(op + 1, cp - op - 1));
        if (!inner.empty() && (inner.front() == '"' || inner.front() == '\'')) {
            lastError = "Object selectors now strictly require numeric IDs. Name \"" + inner + "\" is not allowed.";
            return false;
        } else {
            try {
                out.bodyID = std::stoi(inner);
                out.bodyName.clear();
                
                // Validate against project objects
                if (isCraft) {
                    if (spacecrafts) {
                        bool found = false;
                        for (const auto& sc : *spacecrafts) {
                            if (sc->ID == out.bodyID) { found = true; break; }
                        }
                        if (!found && out.bodyID != -1) { // Tolerate -1 for uninitialized fallback
                            lastError = "Craft ID " + std::to_string(out.bodyID) + " not found in current project.";
                            return false;
                        }
                    }
                } else {
                    if (planets) {
                        bool found = false;
                        for (const auto& p : *planets) {
                            if (p.SpiceID == out.bodyID) { found = true; break; }
                        }
                        if (!found) {
                            lastError = "Spice ID " + std::to_string(out.bodyID) + " not found in current project.";
                            return false;
                        }
                    }
                }
            } catch (...) {
                lastError = "Invalid numeric ID: " + inner;
                return false;
            }
        }

        size_t dot = e.find('.', cp);
        if (dot == std::string::npos) { lastError = "Missing .attr: " + e; return false; }
        out.attr = trim(e.substr(dot + 1));
        out.source = isCraft ? AxisSource::CraftAttr : AxisSource::BodyAttr;
        return true;
    }
    lastError = "Unknown channel expr: " + e;
    return false;
}

inline glm::vec4 OutputScript::parseColor(const std::string& s) {
    std::string n = trim(s);
    // Quotes must be handled by the caller or validated here
    if (n.size() < 2 || (n.front() != '"' && n.front() != '\'') || n.back() != n.front())
        return glm::vec4(0, 0, 0, -2); // Signal invalid string error

    n = n.substr(1, n.size() - 2);
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    
    if (n == "red")     return glm::vec4(1, 0.2f, 0.2f, 1);
    if (n == "blue")    return glm::vec4(0.2f, 0.6f, 1, 1);
    if (n == "green")   return glm::vec4(0.2f, 1.0f, 0.2f, 1);
    if (n == "yellow")  return glm::vec4(1, 1, 0.2f, 1);
    if (n == "cyan")    return glm::vec4(0, 1, 1, 1);
    if (n == "magenta") return glm::vec4(1, 0, 1, 1);
    if (n == "black")   return glm::vec4(0, 0, 0, 1);
    if (n == "white")   return glm::vec4(1, 1, 1, 1);
    if (n == "orange")  return glm::vec4(1, 0.65f, 0, 1);
    if (n == "gray")    return glm::vec4(0.5f, 0.5f, 0.5f, 1);
    if (n == "purple")  return glm::vec4(0.6f, 0.2f, 0.8f, 1);
    if (n == "lime")    return glm::vec4(0.75f, 1, 0, 1);
    return glm::vec4(0, 0, 0, -1); // Auto
}

inline bool OutputScript::parse(const std::string& scriptText, const std::vector<CelestialBody>* planets, const std::vector<std::shared_ptr<Spacecraft>>* spacecrafts) {
    plots.clear(); reports.clear(); lastError.clear();
    std::istringstream ss(scriptText);
    std::string line;
    std::vector<std::string> clean;
    while (std::getline(ss, line)) {
        size_t s = line.find_first_not_of(" \t\r\n");
        if (s == std::string::npos) continue;
        line = line.substr(s);
        size_t h = line.find('#');
        if (h != std::string::npos) line = line.substr(0, h);
        size_t e = line.find_last_not_of(" \t\r\n");
        if (e == std::string::npos) continue;
        line = line.substr(0, e + 1);
        if (!line.empty()) clean.push_back(line);
    }
    std::vector<std::string> stmts;
    std::string cur; int depth = 0;
    for (auto& l : clean) {
        if (!cur.empty()) cur += ' ';
        cur += l;
        for (char c : l) { if (c == '(') depth++; else if (c == ')') depth--; }
        if (depth <= 0) { stmts.push_back(cur); cur.clear(); depth = 0; }
    }
    if (!cur.empty()) stmts.push_back(cur);

    std::string currentFigureTitle = "Plot";
    AxisChannel currentX; currentX.source = AxisSource::SystemDay;

    for (auto& stmt : stmts) {
        std::string s = trim(stmt);
        if (s.empty()) continue;
        size_t pp = s.find('(');
        std::string fn = (pp != std::string::npos) ? s.substr(0, pp) : s;

        if (fn == "figure") {
            std::string title;
            std::vector<std::pair<std::string,std::string>> kv;
            if (!parseFuncArgs(s, title, kv)) { 
                if (lastError.empty()) lastError = "Failed to parse figure()"; 
                return false; 
            }
            currentFigureTitle = title;
            PlotDef pd;
            pd.title = currentFigureTitle;
            pd.showExtrema = true; // Default
            for (auto& [key, val] : kv) {
                if (key == "x") { if (!parseChannel(val, currentX, planets, spacecrafts)) return false; }
                else if (key == "extrema") {
                    if (val == "false") pd.showExtrema = false;
                    else if (val == "true") pd.showExtrema = true;
                } else if (key == "xlabel") {
                    if (val.size() < 2 || (val.front() != '"' && val.front() != '\'') || val.back() != val.front()) {
                        lastError = "xlabel must be a strictly quoted string. Found: " + val;
                        return false;
                    }
                    pd.xlabel = val.substr(1, val.size() - 2);
                } else if (key == "ylabel") {
                    if (val.size() < 2 || (val.front() != '"' && val.front() != '\'') || val.back() != val.front()) {
                        lastError = "ylabel must be a strictly quoted string. Found: " + val;
                        return false;
                    }
                    pd.ylabel = val.substr(1, val.size() - 2);
                }
            }
            auto it = std::find_if(plots.begin(), plots.end(), [&](const PlotDef& p){ return p.title == currentFigureTitle; });
            if (it == plots.end()) {
                pd.xAxis = currentX;
                plots.push_back(std::move(pd));
            } else {
                it->xAxis = currentX;
                it->showExtrema = pd.showExtrema;
                it->xlabel = pd.xlabel;
                it->ylabel = pd.ylabel;
            }
        } else if (fn == "plot") {
            std::string titleArg;
            std::vector<std::pair<std::string,std::string>> kv;
            if (!parseFuncArgs(s, titleArg, kv)) { 
                if (lastError.empty()) lastError = "Failed to parse plot()"; 
                return false; 
            }
            
            if (titleArg != "Plot") {
                lastError = "Legacy syntax detected: plot(\"" + titleArg + "\", ...). Please use figure(\"" + titleArg + "\") instead.";
                return false;
            }

            auto it = std::find_if(plots.begin(), plots.end(), [&](const PlotDef& p){ return p.title == currentFigureTitle; });
            if (it == plots.end()) {
                PlotDef pd; pd.title = currentFigureTitle; pd.xAxis = currentX;
                plots.push_back(std::move(pd));
                it = plots.end() - 1;
            }
            LineDef ld;
            for (auto& [key, val] : kv) {
                if (key == "y") { if (!parseChannel(val, ld.yAxis, planets, spacecrafts)) return false; }
                else if (key == "color") {
                    ld.color = parseColor(val);
                    if (ld.color.a == -2) {
                        lastError = "Color must be a strictly quoted string (e.g. \"orange\"). Found: " + val;
                        return false;
                    }
                }
                else if (key == "label") {
                    if (val.size() < 2 || (val.front() != '"' && val.front() != '\'') || val.back() != val.front()) {
                        lastError = "Label must be a strictly quoted string. Found: " + val;
                        return false;
                    }
                    ld.label = val.substr(1, val.size() - 2);
                }
            }
            it->lines.push_back(std::move(ld));
        } else if (fn == "report") {
            std::string fname;
            std::vector<std::pair<std::string,std::string>> kv;
            if (!parseFuncArgs(s, fname, kv)) { lastError = "Failed to parse report()"; return false; }
            ReportDef rd; rd.filename = fname;
            for (auto& [key, val] : kv) {
                if (key == "columns") {
                    std::string inner = val;
                    if (!inner.empty() && inner.front() == '[') inner = inner.substr(1);
                    if (!inner.empty() && inner.back()  == ']') inner.pop_back();
                    for (auto& ce : splitTopLevel(inner, ',')) {
                        AxisChannel ac; if (!parseChannel(trim(ce), ac, planets, spacecrafts)) return false;
                        rd.columns.push_back(ac);
                    }
                }
            }
            reports.push_back(std::move(rd));
        }
    }
    return true;
}

inline void OutputScript::initForSim(const std::string& projectFolder) {
    std::string outputsDir = projectFolder.empty() ? "Outputs" : projectFolder + "\\Outputs";
    if (!projectFolder.empty()) {
        std::filesystem::create_directories(outputsDir);
    }
    for (auto& pd : plots) {
        pd.initData();
        pd.headerWritten = false;
        if (pd.plotFile.is_open()) pd.plotFile.close();
        std::string fileName = pd.title + ".esmatplot";
        std::string path = projectFolder.empty() ? fileName : outputsDir + "\\" + fileName;
        pd.plotFile.open(path);
        if (pd.plotFile.is_open()) {
            pd.plotFile << pd.xAxis.legend();
            for (auto& l : pd.lines) pd.plotFile << "," << (l.label.empty() ? l.yAxis.legend() : l.label);
            pd.plotFile << "\n";
            pd.headerWritten = true;
        }
    }
    for (auto& rd : reports) {
        rd.headerWritten = false;
        if (rd.fileStream.is_open()) rd.fileStream.close();
        std::string path = projectFolder.empty() ? rd.filename : outputsDir + "\\" + rd.filename;
        rd.fileStream.open(path);
        if (rd.fileStream.is_open() && !rd.columns.empty()) {
            for (size_t i = 0; i < rd.columns.size(); ++i) {
                if (i > 0) rd.fileStream << ",";
                rd.fileStream << rd.columns[i].legend();
            }
            rd.fileStream << "\n";
            rd.headerWritten = true;
        }
    }
}

inline double OutputScript::getBodyAttr(const CelestialBody& body, const std::string& attr) {
    const double AU = 149597870.7;
    if (attr == "pos_x")        return body.RelativePositionKM.x;
    if (attr == "pos_y")        return body.RelativePositionKM.y;
    if (attr == "pos_z")        return body.RelativePositionKM.z;
    if (attr == "dist_km")      return glm::length(body.RelativePositionKM);
    if (attr == "dist_au")      return glm::length(body.RelativePositionKM) / AU;
    if (attr == "dist_sun_km")  return glm::length(body.PositionKM);
    if (attr == "dist_sun_au")  return glm::length(body.PositionKM) / AU;
    if (attr == "vel")          return glm::length(body.RelativeVelocityKMS);
    if (attr == "vel_sun")      return glm::length(body.VelocityKMS);
    if (attr == "period_days")  return body.orbitPeriodSeconds / 86400.0;
    return 0.0;
}

inline double OutputScript::getCraftAttr(const Spacecraft& sc, const std::string& attr, const std::vector<CelestialBody>& planets) {
    const double AU = 149597870.7;
    
    // Relative to Center Body
    if (attr == "pos_x")        return sc.PositionKM.x;
    if (attr == "pos_y")        return sc.PositionKM.y;
    if (attr == "pos_z")        return sc.PositionKM.z;
    if (attr == "dist_parent_km") return glm::length(sc.PositionKM);
    if (attr == "vel_parent")     return glm::length(sc.VelocityKMS);
    
    // Calculate Absolute (Sun-centered) values
    glm::dvec3 centerPosObs(0.0);
    glm::dvec3 centerVelObs(0.0);
    for (const auto& planet : planets) {
        if (planet.SpiceID == sc.centerBodySpiceID) {
            centerPosObs = planet.PositionKM;
            centerVelObs = planet.VelocityKMS;
            break;
        }
    }
    
    glm::dvec3 absPos = centerPosObs + sc.PositionKM;
    glm::dvec3 absVel = centerVelObs + sc.VelocityKMS;

    if (attr == "dist_sun_km")  return glm::length(absPos);
    if (attr == "dist_sun_au")  return glm::length(absPos) / AU;
    if (attr == "vel_sun")      return glm::length(absVel);
    
    return 0.0;
}

inline bool OutputScript::getChannelValue(const AxisChannel& ch, double et,
                                           const std::vector<CelestialBody>& planets,
                                           const std::vector<std::shared_ptr<Spacecraft>>& spacecrafts,
                                           double& val) const {
    double day = (et - simStartET) / 86400.0;
    static constexpr double J2000_UNIX = 946728000.0;
    switch (ch.source) {
        case AxisSource::SystemDay:  val = day;              return true;
        case AxisSource::SystemDate: val = et + J2000_UNIX;  return true;
        case AxisSource::SystemTime: val = et;               return true;
        case AxisSource::BodyAttr:
            for (const auto& p : planets) {
                if (ch.bodyID != -1) {
                    if (p.SpiceID == ch.bodyID) { val = getBodyAttr(p, ch.attr); return true; }
                } else {
                    std::string up = p.Name;
                    std::transform(up.begin(), up.end(), up.begin(), ::toupper);
                    if (up == ch.bodyName) { val = getBodyAttr(p, ch.attr); return true; }
                }
            }
            return false;
        case AxisSource::CraftAttr:
            for (const auto& sc : spacecrafts) {
                if (sc->ID == ch.bodyID) {
                    val = getCraftAttr(*sc, ch.attr, planets);
                    return true;
                }
            }
            return false;
    }
    return false;
}

inline void OutputScript::record(double et, const std::vector<CelestialBody>& planets, const std::vector<std::shared_ptr<Spacecraft>>& spacecrafts) {
    for (auto& pd : plots) {
        if (pd.isFull()) continue;
        double xv; if (!getChannelValue(pd.xAxis, et, planets, spacecrafts, xv)) continue;
        pd.xValues.push_back(xv);
        if (pd.plotFile.is_open()) pd.plotFile << xv;
        for (auto& l : pd.lines) {
            double yv;
            if (!getChannelValue(l.yAxis, et, planets, spacecrafts, yv)) yv = 0.0;
            l.yValues.push_back(yv);
            if (pd.plotFile.is_open()) pd.plotFile << "," << yv;
        }
        if (pd.plotFile.is_open()) pd.plotFile << "\n";
    }
    for (auto& rd : reports) {
        if (!rd.fileStream.is_open()) continue;
        for (size_t i = 0; i < rd.columns.size(); ++i) {
            double v; if (!getChannelValue(rd.columns[i], et, planets, spacecrafts, v)) v = 0.0;
            if (i > 0) rd.fileStream << ",";
            rd.fileStream << v;
        }
        rd.fileStream << "\n";
    }
}

inline void OutputScript::clear() {
    for (auto& pd : plots) pd.clear();
    closeOutputs();
}

inline void OutputScript::closeOutputs() {
    for (auto& rd : reports) if (rd.fileStream.is_open()) rd.fileStream.close();
    for (auto& pd : plots) if (pd.plotFile.is_open()) pd.plotFile.close();
}

inline std::string OutputScript::defaultTemplate() {
    return "# ESMAT Output Script\n"
           "# 1. Define a figure with a title and shared x-axis\n"
           "figure(\"Solar Distance\", x=esmat.date)\n"
           "\n"
           "# 2. Add individual lines (MUST use numeric Spice IDs)\n"
           "# Earth (399), Mars (499)\n"
           "plot(y=ESMATObject(399).dist_sun_au, color=\"cyan\", label=\"Earth\")\n"
           "plot(y=ESMATObject(499).dist_sun_au, color=\"orange\", label=\"Mars\")\n"
           "\n"
           "# 3. Optional report\n"
           "report(\"data.csv\", columns=[esmat.date, ESMATObject(399).dist_sun_au])\n";
}
