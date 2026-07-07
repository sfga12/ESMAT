#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>

struct ESailVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec3 color;
};

class ESailMesh {
public:
    unsigned int tethersVAO = 0, tethersVBO = 0;
    unsigned int bodyVAO = 0, bodyVBO = 0, bodyEBO = 0;
    unsigned int axesVAO = 0, axesVBO = 0;
    unsigned int axesTriVAO = 0, axesTriVBO = 0;
    
    int currentTetherCount = 0;
    float currentConeAngle = 0.0f;
    int indexCount = 0;
    int tetherVertexCount = 0;

    ESailMesh() {
        glGenVertexArrays(1, &tethersVAO);
        glGenBuffers(1, &tethersVBO);
        
        glGenVertexArrays(1, &bodyVAO);
        glGenBuffers(1, &bodyVBO);
        glGenBuffers(1, &bodyEBO);
        
        buildStaticBodyMesh();
        buildAxesMesh();
    }

    ~ESailMesh() {
        if(tethersVAO) glDeleteVertexArrays(1, &tethersVAO);
        if(tethersVBO) glDeleteBuffers(1, &tethersVBO);
        if(bodyVAO) glDeleteVertexArrays(1, &bodyVAO);
        if(bodyVBO) glDeleteBuffers(1, &bodyVBO);
        if(bodyEBO) glDeleteBuffers(1, &bodyEBO);
        if(axesVAO) glDeleteVertexArrays(1, &axesVAO);
        if(axesVBO) glDeleteBuffers(1, &axesVBO);
        if(axesTriVAO) glDeleteVertexArrays(1, &axesTriVAO);
        if(axesTriVBO) glDeleteBuffers(1, &axesTriVBO);
    }

    void updateTethers(int tetherCount, float coneAngleDeg) {
        if (tetherCount == currentTetherCount && coneAngleDeg == currentConeAngle) return;
        
        currentTetherCount = tetherCount;
        currentConeAngle = coneAngleDeg;
        
        std::vector<glm::vec3> tetherData;
        
        float coneAngleRad = glm::radians(coneAngleDeg);
        
        // Base lengths (normalized relative to standard rendering block)
        float L = 1.0f; 
        
        for (int i = 0; i < tetherCount; i++) {
            float theta = glm::radians(i * (360.0f / tetherCount));
            
            // X and Y lie in the spin plane
            float x = L * std::cos(coneAngleRad) * std::cos(theta);
            float y = L * std::cos(coneAngleRad) * std::sin(theta);
            
            // Z bends away from the sun (Sun is assumed +Z, so cone opens negatively)
            float z = -L * std::sin(coneAngleRad);
            
            tetherData.push_back(glm::vec3(0.0f, 0.0f, 0.0f)); // Start at center
            tetherData.push_back(glm::vec3(x, y, z)); // Extend to edge
        }
        

        tetherVertexCount = tetherData.size();

        glBindVertexArray(tethersVAO);
        glBindBuffer(GL_ARRAY_BUFFER, tethersVBO);
        glBufferData(GL_ARRAY_BUFFER, tetherVertexCount * sizeof(glm::vec3), tetherData.data(), GL_STATIC_DRAW);
        
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)0);
        glEnableVertexAttribArray(0);
        
        glBindVertexArray(0);
    }

    void buildAxesMesh() {
        std::vector<glm::vec3> axesData;
        std::vector<glm::vec3> axesTriData;
        
        auto buildArrow = [&](glm::vec3 axisDir) {
            float L = 1.3f;
            glm::vec3 start(0.0f);
            glm::vec3 end = axisDir * L;
            
            // Central line (Full Shaft)
            axesData.push_back(start); axesData.push_back(end);
            
            // Arrowheads removed for testing simplified look
        };
        
        buildArrow(glm::vec3(0, 0, 1.0f)); // Z
        buildArrow(glm::vec3(1.0f, 0, 0)); // X
        buildArrow(glm::vec3(0, 1.0f, 0)); // Y

        // Line Buffer
        glGenVertexArrays(1, &axesVAO);
        glGenBuffers(1, &axesVBO);
        glBindVertexArray(axesVAO);
        glBindBuffer(GL_ARRAY_BUFFER, axesVBO);
        glBufferData(GL_ARRAY_BUFFER, axesData.size() * sizeof(glm::vec3), axesData.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)0);
        glEnableVertexAttribArray(0);

        // Triangle Buffer
        glGenVertexArrays(1, &axesTriVAO);
        glGenBuffers(1, &axesTriVBO);
        glBindVertexArray(axesTriVAO);
        glBindBuffer(GL_ARRAY_BUFFER, axesTriVBO);
        glBufferData(GL_ARRAY_BUFFER, axesTriData.size() * sizeof(glm::vec3), axesTriData.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)0);
        glEnableVertexAttribArray(0);
        
        glBindVertexArray(0);
    }

    void buildStaticBodyMesh() {
        // Build a minimalist central body with Solar Panels.
        std::vector<ESailVertex> vertices;
        std::vector<unsigned int> indices;
        
        glm::vec3 colBody(0.8f, 0.8f, 0.8f); // Silver body
        glm::vec3 colPanel(0.1f, 0.2f, 0.8f); // Blue solar panels
        
        auto addTri = [&](int v0, int v1, int v2) {
            indices.push_back(v0); indices.push_back(v1); indices.push_back(v2);
        };
        auto addQuad = [&](glm::vec3 p0, glm::vec3 p1, glm::vec3 p2, glm::vec3 p3, glm::vec3 n, glm::vec3 c) {
            int off = vertices.size();
            vertices.push_back({p0, n, c}); vertices.push_back({p1, n, c});
            vertices.push_back({p2, n, c}); vertices.push_back({p3, n, c});
            addTri(off, off+1, off+2); addTri(off, off+2, off+3);
        };
        auto addBox = [&](glm::vec3 minP, glm::vec3 maxP, glm::vec3 color) {
            // Front (-Z)
            addQuad(glm::vec3(minP.x, minP.y, minP.z), glm::vec3(maxP.x, minP.y, minP.z), glm::vec3(maxP.x, maxP.y, minP.z), glm::vec3(minP.x, maxP.y, minP.z), glm::vec3(0,0,-1), color);
            // Back (+Z)
            addQuad(glm::vec3(maxP.x, minP.y, maxP.z), glm::vec3(minP.x, minP.y, maxP.z), glm::vec3(minP.x, maxP.y, maxP.z), glm::vec3(maxP.x, maxP.y, maxP.z), glm::vec3(0,0,1), color);
            // Left (-X)
            addQuad(glm::vec3(minP.x, minP.y, maxP.z), glm::vec3(minP.x, minP.y, minP.z), glm::vec3(minP.x, maxP.y, minP.z), glm::vec3(minP.x, maxP.y, maxP.z), glm::vec3(-1,0,0), color);
            // Right (+X)
            addQuad(glm::vec3(maxP.x, minP.y, minP.z), glm::vec3(maxP.x, minP.y, maxP.z), glm::vec3(maxP.x, maxP.y, maxP.z), glm::vec3(maxP.x, maxP.y, minP.z), glm::vec3(1,0,0), color);
            // Bottom (-Y)
            addQuad(glm::vec3(minP.x, minP.y, maxP.z), glm::vec3(maxP.x, minP.y, maxP.z), glm::vec3(maxP.x, minP.y, minP.z), glm::vec3(minP.x, minP.y, minP.z), glm::vec3(0,-1,0), color);
            // Top (+Y)
            addQuad(glm::vec3(minP.x, maxP.y, minP.z), glm::vec3(maxP.x, maxP.y, minP.z), glm::vec3(maxP.x, maxP.y, maxP.z), glm::vec3(minP.x, maxP.y, maxP.z), glm::vec3(0,1,0), color);
        };

        float bf = 0.05f; // half size of central body
        addBox(glm::vec3(-bf, -bf, -bf), glm::vec3(bf, bf, bf), colBody);
        
        // Solar panel 1 (+X)
        addBox(glm::vec3(bf, -0.01f, -bf), glm::vec3(0.3f, 0.01f, bf), colPanel);
        // Solar panel 2 (-X)
        addBox(glm::vec3(-0.3f, -0.01f, -bf), glm::vec3(-bf, 0.01f, bf), colPanel);

        indexCount = indices.size();

        glBindVertexArray(bodyVAO);
        glBindBuffer(GL_ARRAY_BUFFER, bodyVBO);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(ESailVertex), vertices.data(), GL_STATIC_DRAW);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bodyEBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);

        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(ESailVertex), (void*)0);
        glEnableVertexAttribArray(0);
        
        // Passing normal anyway
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(ESailVertex), (void*)offsetof(ESailVertex, normal));
        glEnableVertexAttribArray(1);
        
        // We can pass color into layout 2 for future use
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(ESailVertex), (void*)offsetof(ESailVertex, color));
        glEnableVertexAttribArray(2);
        
        glBindVertexArray(0);
    }
    
    // Shader notes: sphereShader handles 'color' (or 'objectColor' for sun material fallback). 
    // It's recommended to just bind the VAO, loop index, override shader's setVec3("objectColor"...).
    // Or we just rely on standard shader state holding steady while glDrawElements runs.
};
