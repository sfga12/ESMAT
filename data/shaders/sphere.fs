#version 330 core
out vec4 FragColor;

in vec3 Normal;
in vec3 FragPos;
in vec2 TexCoords;

uniform vec3 objectColor;
uniform vec3 lightColor;
uniform vec3 lightPos;
uniform vec3 viewPos;

uniform sampler2D texture1;
uniform int useTexture;
uniform bool isSun;

void main()
{
    // Make ambient very low to simulate true space darkness on the night side
    float ambientStrength = 0.02;
    vec3 ambient = ambientStrength * lightColor;
    
    // diffuse 
    vec3 norm = normalize(Normal);
    vec3 lightDir = normalize(lightPos - FragPos);
    float diff = max(dot(norm, lightDir), 0.0);
    vec3 diffuse = diff * lightColor;
    
    // specular (Very low for realistic matte planets, mostly rocks/dirt)
    float specularStrength = 0.05;
    vec3 viewDir = normalize(viewPos - FragPos);
    vec3 reflectDir = reflect(-lightDir, norm);  
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), 4);
    vec3 specular = specularStrength * spec * lightColor;  
    
    vec3 baseColor = objectColor;
    if (useTexture == 1) {
        vec4 texColor = texture(texture1, TexCoords);
        baseColor = baseColor * texColor.rgb;
    }
    
    vec3 result = (ambient + diffuse + specular) * baseColor;
    
    // If it's the sun (at 0,0,0 emitting light), make it purely emissive/unlit
    float alpha = 1.0;
    if (isSun) {
        result = baseColor * 1.5; // bloom it up a bit
    } else {
        // For planets, if the camera is extremely far away, make them slightly transparent/glowy 
        // to look better at vast distances instead of tiny hard dots.
        float distToCam = length(viewPos - FragPos);
        if (distToCam > 1000.0) {
            float fadeAmt = clamp((distToCam - 1000.0) / 10000.0, 0.0, 0.4);
            alpha = 1.0 - fadeAmt;
            result += baseColor * fadeAmt * 2.0; // Add a bit of artificial glow
        }
    }

    FragColor = vec4(result, alpha);
}
