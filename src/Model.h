#pragma once
#ifndef MODEL_GLTF_H
#define MODEL_GLTF_H

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>
#include <string>
#include <iostream>

#include <tiny_gltf.h>

#include "Shader.h"

struct PrimitiveGLTF {
    GLuint vao;
    GLuint vboPosition;
    GLuint vboNormal;
    GLuint vboTexCoord;
    GLuint ebo;
    int indexCount;
    int materialIndex;
    int mode; // GL_TRIANGLES etc.
    bool hasIndices;
    glm::vec4 baseColor;
    int diffuseTextureIndex;
};

struct MeshGLTF {
    std::vector<PrimitiveGLTF> primitives;
};

class ModelGLTF {
public:
    tinygltf::Model gltfModel;
    std::vector<MeshGLTF> meshes;
    std::vector<GLuint> textureIDs;
    bool loaded = false;
    float maxAbsCoord = 0.0f; // Used to normalize scale

    ModelGLTF(const std::string& path) {
        loadModel(path);
    }

    ModelGLTF() {}

    void Draw(Shader& shader, const glm::mat4& baseTransform) {
        if (!loaded) return;

        const tinygltf::Scene& scene = gltfModel.scenes[gltfModel.defaultScene > -1 ? gltfModel.defaultScene : 0];
        for (int i = 0; i < scene.nodes.size(); ++i) {
            drawNode(shader, gltfModel.nodes[scene.nodes[i]], baseTransform);
        }
        shader.setInt("useTexture", 0); // Restore state
    }

private:
    void drawNode(Shader& shader, const tinygltf::Node& node, const glm::mat4& parentMatrix) {
        glm::mat4 localMatrix(1.0f);
        
        if (node.matrix.size() == 16) {
            for(int c=0; c<4; ++c) {
                for(int r=0; r<4; ++r) {
                    localMatrix[c][r] = (float)node.matrix[c * 4 + r];
                }
            }
        } else {
            glm::mat4 translation(1.0f);
            glm::mat4 rotation(1.0f);
            glm::mat4 scale(1.0f);
            
            if (node.translation.size() == 3) {
                translation = glm::translate(translation, glm::vec3(node.translation[0], node.translation[1], node.translation[2]));
            }
            if (node.rotation.size() == 4) {
                glm::quat q((float)node.rotation[3], (float)node.rotation[0], (float)node.rotation[1], (float)node.rotation[2]); // w, x, y, z
                rotation = glm::mat4_cast(q);
            }
            if (node.scale.size() == 3) {
                scale = glm::scale(scale, glm::vec3(node.scale[0], node.scale[1], node.scale[2]));
            }
            localMatrix = translation * rotation * scale;
        }

        glm::mat4 globalMatrix = parentMatrix * localMatrix;

        if (node.mesh >= 0 && node.mesh < meshes.size()) {
            drawMesh(shader, meshes[node.mesh], globalMatrix);
        }

        for (int i = 0; i < node.children.size(); ++i) {
            drawNode(shader, gltfModel.nodes[node.children[i]], globalMatrix);
        }
    }

    void drawMesh(Shader& shader, const MeshGLTF& mesh, const glm::mat4& matrix) {
        shader.setMat4("model", matrix);

        for (const auto& primitive : mesh.primitives) {
            // Pass the material color to the shader
            shader.setVec3("objectColor", glm::vec3(primitive.baseColor));
            
            if (primitive.diffuseTextureIndex >= 0 && primitive.diffuseTextureIndex < textureIDs.size()) {
                shader.setInt("useTexture", 1);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, textureIDs[primitive.diffuseTextureIndex]);
                shader.setInt("texture1", 0);
            } else {
                shader.setInt("useTexture", 0);
            }

            glBindVertexArray(primitive.vao);

            if (primitive.hasIndices) {
                glDrawElements(primitive.mode, primitive.indexCount, GL_UNSIGNED_SHORT, 0); // Assuming 16-bit indices
            } else {
                glDrawArrays(primitive.mode, 0, primitive.indexCount);
            }

            glBindVertexArray(0);
        }
    }
    void loadTextures() {
        textureIDs.resize(gltfModel.images.size());
        for (size_t i = 0; i < gltfModel.images.size(); ++i) {
            const tinygltf::Image& image = gltfModel.images[i];
            
            GLuint tex;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            
            GLenum format = GL_RGBA;
            if (image.component == 1) format = GL_RED;
            else if (image.component == 2) format = GL_RG;
            else if (image.component == 3) format = GL_RGB;
            else if (image.component == 4) format = GL_RGBA;
            
            GLenum type = GL_UNSIGNED_BYTE;
            if (image.pixel_type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) type = GL_UNSIGNED_SHORT;
            
            // Fix alignment for RGB images that are not multiple of 4 bytes wide
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, format, image.width, image.height, 0, format, type, &image.image.at(0));
            glGenerateMipmap(GL_TEXTURE_2D);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4); // Restore default
            
            textureIDs[i] = tex;
        }
    }
    void loadModel(const std::string& path) {
        tinygltf::TinyGLTF loader;
        std::string err;
        std::string warn;

        bool ret = loader.LoadBinaryFromFile(&gltfModel, &err, &warn, path);

        if (!warn.empty()) {
            std::cout << "TinyGLTF Warning: " << warn << std::endl;
        }

        if (!err.empty()) {
            std::cerr << "TinyGLTF Error: " << err << std::endl;
        }

        if (!ret) {
            std::cerr << "Failed to load glTF: " << path << std::endl;
            return;
        }

        loadTextures();
        buildMeshes();
        calculateBounds();
        loaded = true;
    }

    glm::vec3 globalMin;
    glm::vec3 globalMax;

    void calculateBounds() {
        globalMin = glm::vec3(FLT_MAX);
        globalMax = glm::vec3(-FLT_MAX);
        const tinygltf::Scene& scene = gltfModel.scenes[gltfModel.defaultScene > -1 ? gltfModel.defaultScene : 0];
        for (int i = 0; i < scene.nodes.size(); ++i) {
            calculateNodeBounds(gltfModel.nodes[scene.nodes[i]], glm::mat4(1.0f));
        }
        
        // Find max absolute coordinate
        maxAbsCoord = 0.0001f;
        for (int c = 0; c < 3; ++c) {
            if (std::abs(globalMin[c]) > maxAbsCoord) maxAbsCoord = std::abs(globalMin[c]);
            if (std::abs(globalMax[c]) > maxAbsCoord) maxAbsCoord = std::abs(globalMax[c]);
        }
    }

    void calculateNodeBounds(const tinygltf::Node& node, const glm::mat4& parentMatrix) {
        glm::mat4 localMatrix(1.0f);
        if (node.matrix.size() == 16) {
            for(int c=0; c<4; ++c) {
                for(int r=0; r<4; ++r) {
                    localMatrix[c][r] = (float)node.matrix[c * 4 + r];
                }
            }
        } else {
            glm::mat4 translation(1.0f);
            glm::mat4 rotation(1.0f);
            glm::mat4 scale(1.0f);
            
            if (node.translation.size() == 3) {
                translation = glm::translate(translation, glm::vec3(node.translation[0], node.translation[1], node.translation[2]));
            }
            if (node.rotation.size() == 4) {
                glm::quat q((float)node.rotation[3], (float)node.rotation[0], (float)node.rotation[1], (float)node.rotation[2]); // w, x, y, z
                rotation = glm::mat4_cast(q);
            }
            if (node.scale.size() == 3) {
                scale = glm::scale(scale, glm::vec3(node.scale[0], node.scale[1], node.scale[2]));
            }
            localMatrix = translation * rotation * scale;
        }

        glm::mat4 globalMatrix = parentMatrix * localMatrix;

        if (node.mesh >= 0 && node.mesh < gltfModel.meshes.size()) {
            const tinygltf::Mesh& mesh = gltfModel.meshes[node.mesh];
            for (const auto& primitive : mesh.primitives) {
                if (primitive.attributes.find("POSITION") != primitive.attributes.end()) {
                    int accIndex = primitive.attributes.at("POSITION");
                    const tinygltf::Accessor& accessor = gltfModel.accessors[accIndex];
                    if (accessor.minValues.size() == 3 && accessor.maxValues.size() == 3) {
                        glm::vec3 minV(accessor.minValues[0], accessor.minValues[1], accessor.minValues[2]);
                        glm::vec3 maxV(accessor.maxValues[0], accessor.maxValues[1], accessor.maxValues[2]);
                        
                        // 8 corners of the AABB
                        glm::vec3 corners[8] = {
                            glm::vec3(minV.x, minV.y, minV.z),
                            glm::vec3(maxV.x, minV.y, minV.z),
                            glm::vec3(minV.x, maxV.y, minV.z),
                            glm::vec3(maxV.x, maxV.y, minV.z),
                            glm::vec3(minV.x, minV.y, maxV.z),
                            glm::vec3(maxV.x, minV.y, maxV.z),
                            glm::vec3(minV.x, maxV.y, maxV.z),
                            glm::vec3(maxV.x, maxV.y, maxV.z)
                        };
                        for (int k = 0; k < 8; ++k) {
                            glm::vec3 transformed = glm::vec3(globalMatrix * glm::vec4(corners[k], 1.0f));
                            for (int c = 0; c < 3; ++c) {
                                if (transformed[c] < globalMin[c]) globalMin[c] = transformed[c];
                                if (transformed[c] > globalMax[c]) globalMax[c] = transformed[c];
                            }
                        }
                    }
                }
            }
        }

        for (int i = 0; i < node.children.size(); ++i) {
            calculateNodeBounds(gltfModel.nodes[node.children[i]], globalMatrix);
        }
    }

    void buildMeshes() {
        meshes.resize(gltfModel.meshes.size());
        maxAbsCoord = 0.0001f; // Prevent division by zero

        for (size_t i = 0; i < gltfModel.meshes.size(); ++i) {
            const tinygltf::Mesh& gltfMesh = gltfModel.meshes[i];
            MeshGLTF& mesh = meshes[i];
            mesh.primitives.resize(gltfMesh.primitives.size());

            for (size_t j = 0; j < gltfMesh.primitives.size(); ++j) {
                const tinygltf::Primitive& gltfPrimitive = gltfMesh.primitives[j];
                PrimitiveGLTF& primitive = mesh.primitives[j];
                
                primitive.mode = gltfPrimitive.mode;
                primitive.materialIndex = gltfPrimitive.material;
                primitive.hasIndices = (gltfPrimitive.indices >= 0);

                // Initialize default color
                primitive.baseColor = glm::vec4(1.0f); // Default white
                primitive.diffuseTextureIndex = -1;

                if (primitive.materialIndex >= 0 && primitive.materialIndex < gltfModel.materials.size()) {
                    const tinygltf::Material& mat = gltfModel.materials[primitive.materialIndex];
                    if (mat.pbrMetallicRoughness.baseColorFactor.size() == 4) {
                        primitive.baseColor = glm::vec4(
                            (float)mat.pbrMetallicRoughness.baseColorFactor[0],
                            (float)mat.pbrMetallicRoughness.baseColorFactor[1],
                            (float)mat.pbrMetallicRoughness.baseColorFactor[2],
                            (float)mat.pbrMetallicRoughness.baseColorFactor[3]
                        );
                    }
                    if (mat.pbrMetallicRoughness.baseColorTexture.index >= 0) {
                        int texIndex = mat.pbrMetallicRoughness.baseColorTexture.index;
                        if (texIndex >= 0 && texIndex < gltfModel.textures.size()) {
                            primitive.diffuseTextureIndex = gltfModel.textures[texIndex].source;
                        }
                    }
                }

                glGenVertexArrays(1, &primitive.vao);
                glBindVertexArray(primitive.vao);

                // POSITION
                if (gltfPrimitive.attributes.find("POSITION") != gltfPrimitive.attributes.end()) {
                    const tinygltf::Accessor& accessor = gltfModel.accessors[gltfPrimitive.attributes.at("POSITION")];
                    const tinygltf::BufferView& bufferView = gltfModel.bufferViews[accessor.bufferView];
                    const tinygltf::Buffer& buffer = gltfModel.buffers[bufferView.buffer];

                    // Position parsing
                    glGenBuffers(1, &primitive.vboPosition);
                    glBindBuffer(GL_ARRAY_BUFFER, primitive.vboPosition);
                    glBufferData(GL_ARRAY_BUFFER, bufferView.byteLength, &buffer.data.at(0) + bufferView.byteOffset, GL_STATIC_DRAW);

                    int byteStride = accessor.ByteStride(bufferView);
                    if (byteStride == -1) {
                         byteStride = tinygltf::GetComponentSizeInBytes(accessor.componentType) * tinygltf::GetNumComponentsInType(accessor.type);
                    }

                    glEnableVertexAttribArray(0); // 0 corresponds to aPos
                    glVertexAttribPointer(0, 3, accessor.componentType, accessor.normalized ? GL_TRUE : GL_FALSE, byteStride, (void*)accessor.byteOffset);

                    if (!primitive.hasIndices) {
                        primitive.indexCount = accessor.count;
                    }
                }

                // NORMAL
                if (gltfPrimitive.attributes.find("NORMAL") != gltfPrimitive.attributes.end()) {
                    const tinygltf::Accessor& accessor = gltfModel.accessors[gltfPrimitive.attributes.at("NORMAL")];
                    const tinygltf::BufferView& bufferView = gltfModel.bufferViews[accessor.bufferView];
                    const tinygltf::Buffer& buffer = gltfModel.buffers[bufferView.buffer];

                    glGenBuffers(1, &primitive.vboNormal);
                    glBindBuffer(GL_ARRAY_BUFFER, primitive.vboNormal);
                    glBufferData(GL_ARRAY_BUFFER, bufferView.byteLength, &buffer.data.at(0) + bufferView.byteOffset, GL_STATIC_DRAW);

                    int byteStride = accessor.ByteStride(bufferView);
                    if (byteStride == -1) {
                         byteStride = tinygltf::GetComponentSizeInBytes(accessor.componentType) * tinygltf::GetNumComponentsInType(accessor.type);
                    }

                    glEnableVertexAttribArray(1); // 1 corresponds to aNormal
                    glVertexAttribPointer(1, 3, accessor.componentType, accessor.normalized ? GL_TRUE : GL_FALSE, byteStride, (void*)accessor.byteOffset);
                } else {
                    // Default up-normal if model lacks normals
                    glVertexAttrib3f(1, 0.0f, 1.0f, 0.0f);
                }

                // TEXCOORD_0
                if (gltfPrimitive.attributes.find("TEXCOORD_0") != gltfPrimitive.attributes.end()) {
                    const tinygltf::Accessor& accessor = gltfModel.accessors[gltfPrimitive.attributes.at("TEXCOORD_0")];
                    const tinygltf::BufferView& bufferView = gltfModel.bufferViews[accessor.bufferView];
                    const tinygltf::Buffer& buffer = gltfModel.buffers[bufferView.buffer];

                    glGenBuffers(1, &primitive.vboTexCoord);
                    glBindBuffer(GL_ARRAY_BUFFER, primitive.vboTexCoord);
                    glBufferData(GL_ARRAY_BUFFER, bufferView.byteLength, &buffer.data.at(0) + bufferView.byteOffset, GL_STATIC_DRAW);

                    int byteStride = accessor.ByteStride(bufferView);
                    if (byteStride == -1) {
                         byteStride = tinygltf::GetComponentSizeInBytes(accessor.componentType) * tinygltf::GetNumComponentsInType(accessor.type);
                    }

                    glEnableVertexAttribArray(2); // 2 corresponds to aTexCoords
                    glVertexAttribPointer(2, 2, accessor.componentType, accessor.normalized ? GL_TRUE : GL_FALSE, byteStride, (void*)accessor.byteOffset);
                }

                // Indices
                if (primitive.hasIndices) {
                    const tinygltf::Accessor& accessor = gltfModel.accessors[gltfPrimitive.indices];
                    const tinygltf::BufferView& bufferView = gltfModel.bufferViews[accessor.bufferView];
                    const tinygltf::Buffer& buffer = gltfModel.buffers[bufferView.buffer];

                    glGenBuffers(1, &primitive.ebo);
                    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, primitive.ebo);
                    glBufferData(GL_ELEMENT_ARRAY_BUFFER, bufferView.byteLength, &buffer.data.at(0) + bufferView.byteOffset, GL_STATIC_DRAW);

                    primitive.indexCount = accessor.count;
                }

                glBindVertexArray(0);
            }
        }
    }
};

#endif
