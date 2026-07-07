#pragma once
#include <string>
#include <vector>
#include <glm/glm.hpp>

class CelestialBody {
public:
    std::string Name;
    int SpiceID;
    float RadiusKM;        
    glm::vec3 Color;
    bool showBody;
    bool showOrbit;
    bool HasTexture;
    unsigned int TextureID;
    std::string TexturePath;
    double J2;             
    double GM;             
    
    // Rotation mapping
    glm::mat4 currentRotationMatrix;
    
    int ParentID;          
    glm::dvec3 PositionKM;  
    glm::dvec3 ParentPositionKM;
    glm::dvec3 VelocityKMS; 
    
    glm::dvec3 RelativePositionKM;
    glm::dvec3 RelativeVelocityKMS;
    
    double lastOrbitUpdateET;
    double orbitPeriodSeconds;
    
    std::vector<glm::dvec3> OrbitPathKM; 
    class LineRenderer* orbitRenderer;

    CelestialBody(std::string name, int spiceID, float radiusKM, glm::vec3 color, int parentID = 10) 
        : Name(name), SpiceID(spiceID), RadiusKM(radiusKM), Color(color), ParentID(parentID), orbitRenderer(nullptr), showBody(true), showOrbit(true), HasTexture(false), TextureID(0), TexturePath(""), currentRotationMatrix(1.0f), J2(0.0), GM(0.0) {
        PositionKM = glm::dvec3(0.0);
        ParentPositionKM = glm::dvec3(0.0);
        VelocityKMS = glm::dvec3(0.0);
        RelativePositionKM = glm::dvec3(0.0);
        RelativeVelocityKMS = glm::dvec3(0.0);
        lastOrbitUpdateET = 0.0;
        orbitPeriodSeconds = 31557600.0; // 1 year fallback
    }
};
