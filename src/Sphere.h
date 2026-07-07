#pragma once
#include <vector>
#include <glm/glm.hpp>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// We need a Vertex struct
struct Vertex {
    glm::vec3 Position;
    glm::vec3 Normal;
    glm::vec2 TexCoords;
};

class Sphere {
public:
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;

    Sphere(float radius = 1.0f, unsigned int sectorCount = 36, unsigned int stackCount = 18) {
        generate(radius, sectorCount, stackCount);
    }

private:
    void generate(float radius, unsigned int sectorCount, unsigned int stackCount) {
        float x, y, z, xy;                              // vertex position
        float nx, ny, nz, lengthInv = 1.0f / radius;    // vertex normal

        float sectorStep = 2 * M_PI / sectorCount;
        float stackStep = M_PI / stackCount;
        float sectorAngle, stackAngle;

        for(unsigned int i = 0; i <= stackCount; ++i) {
            stackAngle = M_PI / 2 - i * stackStep;        // starting from pi/2 to -pi/2
            xy = radius * cosf(stackAngle);             // r * cos(u)
            z = radius * sinf(stackAngle);              // r * sin(u)

            for(unsigned int j = 0; j <= sectorCount; ++j) {
                sectorAngle = j * sectorStep + M_PI;           // Offset by PI so u=0.5 (Prime Meridian) aligns with X-axis

                x = xy * cosf(sectorAngle);             // r * cos(u) * cos(v)
                y = xy * sinf(sectorAngle);             // r * cos(u) * sin(v)

                nx = x * lengthInv;
                ny = y * lengthInv;
                nz = z * lengthInv;

                float u = (float)j / sectorCount;
                float v = (float)i / stackCount;

                Vertex vertex;
                vertex.Position = glm::vec3(x, y, z);
                vertex.Normal = glm::vec3(nx, ny, nz);
                vertex.TexCoords = glm::vec2(u, v);
                vertices.push_back(vertex);
            }
        }

        // generate indices
        unsigned int k1, k2;
        for(unsigned int i = 0; i < stackCount; ++i) {
            k1 = i * (sectorCount + 1);     // beginning of current stack
            k2 = k1 + sectorCount + 1;      // beginning of next stack

            for(unsigned int j = 0; j < sectorCount; ++j, ++k1, ++k2) {
                if(i != 0) {
                    indices.push_back(k1);
                    indices.push_back(k2);
                    indices.push_back(k1 + 1);
                }

                if(i != (stackCount - 1)) {
                    indices.push_back(k1 + 1);
                    indices.push_back(k2);
                    indices.push_back(k2 + 1);
                }
            }
        }
    }
};
