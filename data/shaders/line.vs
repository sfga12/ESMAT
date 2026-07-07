#version 330 core
layout (location = 0) in vec3 aPos;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform float farPlane;

void main()
{
    gl_Position = projection * view * model * vec4(aPos, 1.0);
    
    // Logarithmic Depth Buffer
    float Fcoef = 2.0 / log2(farPlane + 1.0);
    gl_Position.z = log2(max(1e-6, 1.0 + gl_Position.w)) * Fcoef - 1.0;
    gl_Position.z *= gl_Position.w;
}
