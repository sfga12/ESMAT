#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <unordered_map>

struct TrailPoint {
    double et;
    glm::dvec3 posRelative;
    int centerBodySpiceID;
    bool isManeuver = false;
};

class LineRenderer {
public:
    unsigned int VAO, VBO;
    std::vector<TrailPoint> pointsKM;
    double lastTrailEt = 0.0; // Time tracker for decoupled trail drawing

    LineRenderer() {
        glGenVertexArrays(1, &VAO);
        glGenBuffers(1, &VBO);
    }

    ~LineRenderer() {
        glDeleteVertexArrays(1, &VAO);
        glDeleteBuffers(1, &VBO);
    }

    void updatePoints(const std::vector<TrailPoint>& newPoints) {
        pointsKM = newPoints;
    }

    void addPoint(double currentEt, glm::dvec3 relativePt, int cbID, size_t maxPoints = 500000) {
        if (!pointsKM.empty()) {
            const auto& lastPt = pointsKM.back();
            if (lastPt.centerBodySpiceID == cbID && !lastPt.isManeuver) {
                double dist = glm::length(relativePt - lastPt.posRelative);
                double r = std::min(glm::length(lastPt.posRelative), glm::length(relativePt));
                
                // Adaptive threshold: 1% of the distance to the center body, but minimum 10 km
                // This ensures high-res curves during close flybys, but sparse points in deep space
                double threshold = std::max(10.0, r * 0.01);
                bool timeThresholdMet = (currentEt - lastPt.et) > (86400.0 * 5.0); // max 5 days without a point
                
                if (dist < threshold && !timeThresholdMet) {
                    return; // Skip this point, visually identical line
                }
            }
        }
        
        pointsKM.push_back({currentEt, relativePt, cbID, false});
        if (pointsKM.size() > maxPoints) {
            pointsKM.erase(pointsKM.begin());
        }
    }
    
    void markLastPointAsManeuver() {
        if (!pointsKM.empty()) {
            pointsKM.back().isManeuver = true;
        }
    }
    
    void erasePointsAfter(double targetEt) {
        while (!pointsKM.empty() && pointsKM.back().et > targetEt) {
            pointsKM.pop_back();
        }
    }

    void draw(double currentEt, glm::dvec3 worldOriginKM, double scaleFactor, glm::dvec3 currentAbsolutePos, const std::vector<CelestialBody>& planets) {
        if(pointsKM.empty()) return;
        
        std::unordered_map<int, glm::dvec3> planetPosCache;
        for (const auto& planet : planets) {
            planetPosCache[planet.SpiceID] = planet.PositionKM;
        }
        
        std::vector<glm::vec3> renderPoints;
        renderPoints.reserve(pointsKM.size() + 1);
        for(const auto& p : pointsKM) {
            if (p.et > currentEt) break; // Don't draw trailing path points from the physics buffer that sit in the future
            
            // Find current absolute position of the body this point was recorded relative to (O(1) lookup now)
            glm::dvec3 centerPosAbs(0.0);
            auto it = planetPosCache.find(p.centerBodySpiceID);
            if (it != planetPosCache.end()) {
                centerPosAbs = it->second;
            }
            
            glm::dvec3 pointAbs = centerPosAbs + p.posRelative;
            renderPoints.push_back(glm::vec3((pointAbs - worldOriginKM) * scaleFactor));
        }
        
        // Re-add dynamic visual gap fill: Trail strictly terminates at exact interpolated frame-position
        renderPoints.push_back(glm::vec3((currentAbsolutePos - worldOriginKM) * scaleFactor));

        glBindVertexArray(VAO);
        glBindBuffer(GL_ARRAY_BUFFER, VBO);
        glBufferData(GL_ARRAY_BUFFER, renderPoints.size() * sizeof(glm::vec3), renderPoints.data(), GL_DYNAMIC_DRAW);

        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)0);
        glEnableVertexAttribArray(0);

        // Make the line more prominent
        glLineWidth(2.5f);
        
        int segmentStart = 0;
        int validPointsCount = renderPoints.size() - 1; // Exclude the dynamic gap fill point
        for (int i = 1; i < validPointsCount; ++i) {
            if (pointsKM[i].centerBodySpiceID != pointsKM[i-1].centerBodySpiceID) {
                // Draw the segment up to the point where the reference body changes
                glDrawArrays(GL_LINE_STRIP, segmentStart, i - segmentStart);
                segmentStart = i; // Start new segment
            }
        }
        // Draw the remaining segment including the dynamic visual gap fill
        glDrawArrays(GL_LINE_STRIP, segmentStart, renderPoints.size() - segmentStart);
        
        glLineWidth(1.0f);
    }
};
