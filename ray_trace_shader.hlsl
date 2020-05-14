static const float PI = 3.141592f;
static const float PI2 = PI * 2.0f;

RWTexture2D<float4> tex: register(u0);

cbuffer ConfigBuffer : register(b0)
{
    float3 camera_pos;
    int step;
    int screen_width;
    int screen_height;
    float ambient_light_intensity;
    float sphere_lights_intensity;
    float metal_roughness;
    float refractive_index;
    float dof_radius;
    float dof_focal_plane;
}

static const int SPHERES_COUNT = DEFINE_SPHERES_COUNT;

cbuffer geometry_buffer : register(b1)
{
    float4 spheres[SPHERES_COUNT];
    float4 mats[SPHERES_COUNT];
};

/* Helper functions */

float3x3 get_view_matrix(float3 cam_pos) {
	float3 y = float3(0,1,0);
	float3 z = normalize(cam_pos);
	float3 x = normalize(cross(y, z));
	y = normalize(cross(z, x));

	return float3x3(
		x.x, y.x, z.x,
		x.y, y.y, z.y,
		x.z, y.z, z.z);
}

uint wang_hash(uint seed) {
    seed = (seed ^ 61) ^ (seed >> 16);
    seed *= 9;
    seed = seed ^ (seed >> 4);
    seed *= 0x27d4eb2d;
    seed = seed ^ (seed >> 15);
    return seed;
}

float random(int random_seed) {
    return float(wang_hash(random_seed) % 10000) / 10000.0f;
}

float3 uniform_unit_sphere(int random_seed) {
    float azimuth = random(random_seed * 33) * PI2;
	float polar = acos(2 * random(random_seed * 37 + 3) - 1);
	float r = pow(random(random_seed * 11 - 7), 1.0f / 3.0f);
	
	float3 result = float3(
        r * cos(azimuth) * sin(polar),
        r * cos(polar),
        r * sin(azimuth) * sin(polar)
    );
	return result;
}

float ray_sphere_intersection(float3 rd, float3 rs, float3 s, float r) {
    float3 os = rs - s;
    float a = dot(rd, rd);
    float b = 2.0 * dot(os, rd);
    float c = dot(os, os) - r * r;
    float discriminant = b * b - 4 * a * c;
        
    if(discriminant > 0) {
        float t = (-b - sqrt(discriminant)) / (2.0 * a);
        if(t > 0.001f) {
            return t;
        }
        t = (-b + sqrt(discriminant)) / (2.0 * a);
        if(t > 0.001f) {
            return t;
        }
    }

    return -1;
};

float3 reflect(float3 v, float3 n) {
    return v - 2 * n * dot(v, n);
}

float3 refract(float3 rd, float3 n, float ri) {
    n = -n; // Normal now points in the same direction as ray.
    float3 r_perp = ri * (rd - n * dot(rd, n));
    float3 r_parallel = sqrt(1 - dot(r_perp, r_perp)) * n;
    return r_perp + r_parallel;
}

float schlick(float c, float ri) {
    float r0 = (1 - ri) / (1 + ri);
    r0 = r0 * r0;
    return r0 + (1 - r0) * pow((1 - c), 5);
}

/* Ray tracing logic */

struct RayHitResult {
    float3 normal;
    float t;
    float3 color;
    int material;
    float2 uv;
};

RayHitResult hit_geometry(float3 rd, float3 rs) {
    static const float t_min = 0.001f;

    RayHitResult r;
    r.t = -1; // Initialize current ray hit distance to -1 (no hit)
    for (int i = 0; i < SPHERES_COUNT; ++i) {
        float4 sphere = spheres[i];
        float t = ray_sphere_intersection(rd, rs, sphere.xyz, sphere.w);
        
        // We consider it the closest hit if it's in the positive direction of ray
        // and it's either the first hit (r.t < 0.0) or closer than previously closest hit.
        if(t > t_min && (t < r.t || r.t < 0)) {
            // Calculate normal on the sphere's surface.
            float3 p = rs + rd * t;
            float3 n = normalize(p - sphere.xyz);

            // Store values into result structure.
            r.normal = n;
            r.t = t;
            r.color = mats[i].xyz;
            r.material = round(mats[i].w);
            // UV coordinates on a sphere.
            r.uv.x = 0.5 + atan2(n.x, n.z) / PI2;
            r.uv.y = 0.5 - asin(n.y) / PI;
        }
    }
    return r;
}

// Materials definitions
#define LAMBERT 0
#define LAMBERT_CHECKERBOARD 1
#define METAL 2
#define DIELECTRIC 3
#define LIGHT 4

float3 get_ray_color(float3 rd, float3 rs, int depth, int random_seed) {
    static const int NUM_BOUNCES = 10;

    float3 color = float3(1,1,1);
    for(int i = 0; i < NUM_BOUNCES; ++i) {
        RayHitResult result = hit_geometry(rd, rs);

        // No hit - ambient lighting.
        if(result.t <= 0.0f) {
            color *= float3(1.0f, 1.0f, 1.0f) * ambient_light_intensity;
            return color;
        }

        // Get ray hit's position and normal vector at that point.
        float3 n = result.normal;
        float3 p = result.t * rd + rs;

        // Calculate color update and next ray based on material hit.
        if (result.material == LAMBERT || result.material == LAMBERT_CHECKERBOARD) {
            // Sample next ray direction.
            // Sampling points in a sphere above surface in direction of surface normal
            // should follow cosine distribution - lambertian surface.
            float3 r = uniform_unit_sphere(random_seed * 31 * (i + 1)) + p + n;
            
            // Update next ray's position and direction.
            rd = normalize(r - p);
            rs = p;

            // Update color.
            float s = 1.0f;
            if (result.material == LAMBERT_CHECKERBOARD) {
                // Checkerboard pattern.
                s = sin(result.uv.x * PI2 * 25.0f) * sin(result.uv.y * PI * 25.0f);
                s = sign(s) * 0.5f + 0.5f;
            }
            color *= result.color * s;
        } else if(result.material == METAL) {
            // Reflect incident ray and add some noise to make the metallic surface more diffuse.
            float3 r = reflect(rd, n) + uniform_unit_sphere(random_seed * 19 * (i + 1)) * metal_roughness;

            // Update next ray's position and direction.
            rd = normalize(r);
            rs = p;

            // Update color.
            color *= result.color;
        } else if(result.material == DIELECTRIC) {
            float ri = 1.0f / refractive_index;

            // Normal pointing in the same direction as ray means we hit a sphere from the inside.
            // That means we have to reflect the normal and invert the refractive index.
            if(dot(rd, n) > 0) {
                n = -n;
                ri = 1.0f / ri;
            }

            float3 r;
            // Reflect or refract based on Fresnel equation (approximated).
            float reflect_prob = schlick(dot(rd, -n), ri);
            if (random(random_seed * 31) < reflect_prob) {
                r = reflect(rd, n);
            } else {
                // Compute sine of angle between normal and ray.
                float sin_theta = sqrt(1.0f - dot(rd, -n) * dot(rd, -n));
                // Check for total internal reflection.
                if(sin_theta * ri >= 1.0f) {
                    r = reflect(rd, n);
                } else {
                    r = refract(rd, n, ri);
                }
            }

            // Update next ray's position and direction.
            rd = r;
            rs = p;

            // Update color.
            color *= result.color;
        } else if(result.material == LIGHT) {
            // In case we hit a light source, we're ending ray tracing and just updating the accumulated color.
            color *= result.color * sphere_lights_intensity;
            break;
        }
    }

    return color;
};

[numthreads(GROUP_SIZE_X,GROUP_SIZE_Y,1)]
void main(uint3 threadIDInGroup : SV_GroupThreadID, uint3 groupID : SV_GroupID,
          uint3 dispatchThreadId : SV_DispatchThreadID){
    uint2 p = dispatchThreadId.xy;

    static const int NUM_SAMPLES = 32;
    float3 final_color = float3(0,0,0);
    for (int i = 0; i < NUM_SAMPLES; ++i) {
        // Used for random number generator.
        int random_seed = p.x * 317 * p.y * 911 * (step * NUM_SAMPLES + i);

        // Compute x and y ray directions in "neutral" camera position.
        float aspect_ratio = float(screen_width) / float(screen_height);
        float rx = float(p.x + random(random_seed * 11)) / float(screen_width) * 2.0f - 1.0f;
        float ry = float(p.y + random(random_seed * 17)) / float(screen_height) * 2.0f - 1.0f;
        ry /= aspect_ratio;
        
        // Compute depth of field ray origin offset.
        float r = random(random_seed * 19) * PI2;
        float3 dof_offset = float3(sin(r), cos(r), 0) * dof_radius;

        // Ray's target position on a focal plane.
        // Note that we have to first rotate according to camera position and then
        // make that position relative to camera position.
        float3 rt = float3(rx, ry, -1.0f) * dof_focal_plane;
        rt = mul(get_view_matrix(camera_pos), rt);
        rt += camera_pos;

        // Ray start and direction.
        float3 rs = camera_pos + dof_offset;
        float3 rd = normalize(rt - rs);

        // Get current ray's color.
        float3 ray_color = get_ray_color(rd, rs, 4, random_seed);
        final_color += ray_color;
    }
    // Average current frame's samples.
    final_color /= NUM_SAMPLES;

    // Reinhard tone mapping
    float l = dot(float3(0.2126, 0.7152, 0.0722), final_color);
    final_color /= l + 1;

    // Average values over time.
    tex[p] = float4(final_color, 1.0f) / float(step) + tex[p] * float(step - 1) / float(step);
}

