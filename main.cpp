#include "platform.h"
#include "graphics.h"
#include "file_system.h"
#include "maths.h"
#include "ui.h"
#include "font.h"
#include "input.h"
#include "colors.h"
#include <cassert>

#define _STR(x) #x
#define STR(x) _STR(x)

#define SPHERES_COUNT 75
#define GROUP_SIZE_X 32
#define GROUP_SIZE_Y 32

int main(int argc, char **argv) {
    // Set up window
    uint32_t window_width = 1280, window_height = 960;
    uint32_t render_target_width = window_width / 2, render_target_height = window_height / 2;
 	Window window = platform::get_window("Ray Tracer", window_width, window_height);
    assert(platform::is_window_valid(&window));

    // Init graphics
    graphics::init();
    graphics::init_swap_chain(&window);

    // Init UI.
    font::init();
    ui::init((float)window_width, (float)window_height);
    ui::set_input_responsive(true);

    // Create window render target
	RenderTarget render_target_window = graphics::get_render_target_window(true);
    assert(graphics::is_ready(&render_target_window));
    graphics::set_render_targets_viewport(&render_target_window);

    // Vertex shader for displaying textures.
    File vertex_shader_file = file_system::read_file("vertex_shader.hlsl"); 
    VertexShader vertex_shader = graphics::get_vertex_shader_from_code((char *)vertex_shader_file.data, vertex_shader_file.size);
    file_system::release_file(vertex_shader_file);
    assert(graphics::is_ready(&vertex_shader));

    // Pixel shader for displaying textures.
    File pixel_shader_file = file_system::read_file("pixel_shader.hlsl"); 
    PixelShader pixel_shader = graphics::get_pixel_shader_from_code((char *)pixel_shader_file.data, pixel_shader_file.size);
    file_system::release_file(pixel_shader_file);
    assert(graphics::is_ready(&pixel_shader));

    // List of macro defines for compute shader.
    char *macro_defines[] = {
        "DEFINE_SPHERES_COUNT", STR(SPHERES_COUNT),
        "GROUP_SIZE_X", STR(GROUP_SIZE_X),
        "GROUP_SIZE_Y", STR(GROUP_SIZE_Y)
    };
    
    // Main raytracing shader
    // Note that the path is relative so it's using the source file.
    // This is needed for hot-reloading.
    char *ray_trace_shader_path = "../ray_trace_shader.hlsl";
    File ray_trace_shader_file = file_system::read_file(ray_trace_shader_path);
    ComputeShader ray_trace_shader = graphics::get_compute_shader_from_code(
        (char *)ray_trace_shader_file.data, ray_trace_shader_file.size, macro_defines, ARRAYSIZE(macro_defines)
    );
    file_system::release_file(ray_trace_shader_file);
    assert(graphics::is_ready(&ray_trace_shader));

    // Simple texture sampler.
    TextureSampler tex_sampler = graphics::get_texture_sampler();
    assert(graphics::is_ready(&tex_sampler));

    // Texture where we'll render the scene.
    Texture2D render_texture = graphics::get_texture2D(NULL, render_target_width, render_target_height, DXGI_FORMAT_R32G32B32A32_FLOAT, 16);
    assert(graphics::is_ready(&render_texture));

    // Quad mesh for rendering the resulting texture.
    Mesh quad_mesh = graphics::get_quad_mesh();

    // Camera setup.
    float azimuth = 0.0f;
    float polar = math::PIHALF * 0.5f;
    float radius = 10.0f;

    // Config buffer.
    struct Config {
        Vector3 camera_pos;
        int step;

        int render_target_width;
        int render_target_height;
        float ambient_light_intensity;
        float sphere_lights_intensity;

        float metal_roughness;
        float refractive_index;
        float dof_radius;
        float dof_focal_plane;
    };
    Config config = {
        Vector3(0,0,0),
        0,

        int(render_target_width),
        int(render_target_height),
        15.0f,
        1.0f,
        
        0.0f,
        1.5f,
        0.0f,
        8.0f,
    };
    ConstantBuffer config_buffer = graphics::get_constant_buffer(sizeof(Config));

    struct SpheresBuffer {
        Vector4 positions[SPHERES_COUNT];
        Vector4 materials[SPHERES_COUNT];
    };
    ConstantBuffer spheres_buffer = graphics::get_constant_buffer(sizeof(SpheresBuffer));

    enum Material {
        LAMBERT = 0,
        LAMBERT_CHECKERBOARD = 1,
        METAL = 2,
        DIELECTRIC = 3,
        LIGHT = 4,
    };

    // Initialize spheres.
    SpheresBuffer spheres;
    // Ground sphere.
    spheres.positions[0] = Vector4(0, -1000, 0, 1000);
    spheres.materials[0] = Vector4(0.15f, 0.15f, 0.15f, LAMBERT);

    // "Sun" sphere.
    // Not used by default. To use it, loop in reset_spheres has to start from 2.
    spheres.positions[1] = Vector4(10, 10, 0, 2);
    spheres.materials[1] = Vector4(800.0f, 800.0f, 800.0f, LIGHT);

    // Function to reset spheres positions/colors/materials.
    auto reset_spheres = [&spheres, &spheres_buffer]() {
        const float SPHERES_CIRCLE_RADIUS = 15.0f;
        for(int i = 1; i < SPHERES_COUNT; ++i) {
            float sphere_size = math::random_uniform(0.5f, 1.0f);
            
            // Generate sphere's position so it doesn't overlap with any other sphere.
            float x, z;
            bool collision = false;
            do {
                // Random position in a circle.
                float a = math::random_uniform(0, math::PI2);
                float r = math::random_uniform() * SPHERES_CIRCLE_RADIUS;
                x = math::sin(a) * r - 6;
                z = math::cos(a) * r;
                Vector2 current_pos = Vector2(x, z);

                // Check for collisions.
                collision = false;
                for(int j = 1; j < i; j++) {
                    Vector4 other_sphere = spheres.positions[j];
                    Vector2 other_sphere_pos = Vector2(other_sphere.x, other_sphere.z);
                    if(math::length(other_sphere_pos - current_pos) < other_sphere.w + sphere_size) {
                        collision = true;
                        break;
                    }
                }
            } while(collision);
            spheres.positions[i] = Vector4(x, sphere_size, z, sphere_size);

            // Map from index (random number) to material.
            // We want lambertian materials to be more probable, so they're represented twice in the map.
            Material index_to_mat[] = {
                LAMBERT,
                LAMBERT,
                LAMBERT_CHECKERBOARD,
                LAMBERT_CHECKERBOARD,
                METAL,
                DIELECTRIC,
                LIGHT
            };
            Material mat = index_to_mat[int(math::random_uniform(0, ARRAYSIZE(index_to_mat)))];

            // Get sphere's color.
            Vector3 color = Vector3(0.9f, 0.9f, 0.9f);
            switch (mat) {
                case LAMBERT:
                case LAMBERT_CHECKERBOARD: {
                    color = colors::hsv_to_rgb(math::random_uniform(180, 360), 0.9f, 1) * 0.2f;
                }
                break;
                case METAL:
                break;
                case DIELECTRIC:
                break;
                case LIGHT: {
                    color = colors::hsv_to_rgb(math::random_uniform(0, 360), 0.2f, 1) * 500.0f;
                }
                break;
            }
            spheres.materials[i] = Vector4(color, float(mat));
        }
        // Update constant buffer with new spheres.
        graphics::update_constant_buffer(&spheres_buffer, &spheres);
    };

    // Function to reset rendering state.
    auto reset_rendering = [&config, &render_texture]() {
        graphics::clear_texture(&render_texture, 0.0f, 0.0f, 0.0f, 0.0f);
        config.step = 1;
    };

    // Initialize spheres for the first time.
    reset_spheres();

    // Render loop
    bool is_running = true;
    bool show_ui = true;
    FILETIME stored_file_time;

    Timer timer = timer::get();
    timer::start(&timer);
    while(is_running) {
        // Compute FPS.
        float dt = timer::checkpoint(&timer);
        int fps = int(1.0f / dt);

        // Update ray tracing step.
        config.step += 1;
    
        // Event loop
        {
            input::reset();
            Event event;
            while(platform::get_event(&event)) {
                input::register_event(&event);

                // Check if close button pressed
                switch(event.type) {
                    case EventType::EXIT: {
                        is_running = false;
                    }
                    break;
                }
            }
        }

        // React to inputs
        if (!ui::is_registering_input()) {
            // Handle key presses.
            if (input::key_pressed(KeyCode::ESC)) is_running = false; 
            if (input::key_pressed(KeyCode::F1)) show_ui = !show_ui; 
            if (input::key_pressed(KeyCode::F2)) {
                reset_rendering();   
                reset_spheres();
            }

            // Handle mouse wheel scrolling.
            float scroll_delta = input::mouse_scroll_delta();
            if(math::abs(scroll_delta) > 0.0f) {
                radius -= input::mouse_scroll_delta() * 0.1f;
                reset_rendering();
            }

            // Handle mouse movement.
            if (input::mouse_left_button_down()) {
                const float MOUSE_SPEED = 0.003f;
                Vector2 dm = input::mouse_delta_position();
                azimuth -= dm.x * MOUSE_SPEED;
                polar -= dm.y * MOUSE_SPEED;
                polar = math::clamp(polar, 0.02f, math::PI);  // Clamp so we cannot look completely along y-axis.

                reset_rendering();   
            }
        }

        // Update camera position.
        {
            Vector3 camera_pos = Vector3(
                math::sin(azimuth) * math::sin(polar),
                math::cos(polar),
                math::cos(azimuth) * math::sin(polar)
            ) * radius;
            config.camera_pos = camera_pos;
        }

        // Shader hot reloading.
        {
            // Get the latest shader file write time.
            char *reload_shader_file = "../ray_trace_shader.hlsl";
            FILETIME current_file_time = file_system::get_last_write_time(reload_shader_file);

            // Check if we've seen this shader before. If not, attempt reload.
            if (CompareFileTime(&current_file_time, &stored_file_time) != 0) {
                // Try to compile the new shader.
                File ray_trace_shader_file = file_system::read_file(reload_shader_file);
                ComputeShader new_ray_trace_shader = graphics::get_compute_shader_from_code(
                    (char *)ray_trace_shader_file.data, ray_trace_shader_file.size, macro_defines, ARRAYSIZE(macro_defines))
                ;
                file_system::release_file(ray_trace_shader_file);
                
                // If the compilation was successful, release the old shader and replace with the new one.
                bool reload_success = graphics::is_ready(&new_ray_trace_shader);
                if(reload_success) {
                    graphics::release(&ray_trace_shader);
                    ray_trace_shader = new_ray_trace_shader;
                    reset_rendering();
                }

                // Remember the current shader's write time.
                stored_file_time = current_file_time;
            }
        }

        // Ray tracing.
        graphics::set_compute_shader(&ray_trace_shader);
        graphics::set_constant_buffer(&spheres_buffer, 1);
        graphics::set_constant_buffer(&config_buffer, 0);
        graphics::update_constant_buffer(&config_buffer, &config);
        graphics::set_texture_compute(&render_texture, 0);
        graphics::run_compute(render_target_width / int(GROUP_SIZE_X), render_target_height / int(GROUP_SIZE_Y), 1);
        graphics::unset_texture_compute(0);

        // Draw texture with ray-traced image.
        graphics::set_render_targets_viewport(&render_target_window);
        graphics::clear_render_target(&render_target_window, 0.0f, 0.0f, 0.0f, 1);
        graphics::set_vertex_shader(&vertex_shader);
        graphics::set_pixel_shader(&pixel_shader);
        graphics::set_texture_sampler(&tex_sampler, 0);
        graphics::set_texture(&render_texture, 0);
        graphics::draw_mesh(&quad_mesh);
        graphics::unset_texture(0);

        // UI rendering.
        if(show_ui) {
            // Set color of text and UI panel background based on ambient lighting.
            float color_modifier = math::clamp(config.ambient_light_intensity, 0.0f, 1.0f);
            ui::set_background_opacity(color_modifier);
            float text_color_base = color_modifier > 0.5f ? 0.0f : 1.0f;
            Vector4 text_color = Vector4(text_color_base, text_color_base, text_color_base, 1.0f);

            // Render FPS and rendering steps counter.
            char text_buffer[100];
            sprintf_s(text_buffer, 100, "FPS %d", fps);
            ui::draw_text(text_buffer, Vector2(10, float(window_height) - 10), text_color, Vector2(0, 1));
            sprintf_s(text_buffer, 100, "STEPS %d", config.step);
            ui::draw_text(text_buffer, Vector2(10, float(window_height) - 30), text_color, Vector2(0, 1));

            // Render controls UI.
            Panel panel = ui::start_panel("", Vector2(10, 10.0f), 410.0f);
            bool changed = ui::add_slider(&panel, "ambient light intensity", &config.ambient_light_intensity, 0.0f, 20.0f);
            changed |= ui::add_slider(&panel, "sphere lights intensity", &config.sphere_lights_intensity, 0.0f, 20.0f);
            changed |= ui::add_slider(&panel, "metal roughness", &config.metal_roughness, 0.0f, 1.0f);
            changed |= ui::add_slider(&panel, "refractive index", &config.refractive_index, 0.5f, 2.0f);
            changed |= ui::add_slider(&panel, "dof radius", &config.dof_radius, 0.0f, .2f);
            changed |= ui::add_slider(&panel, "dof focal plane", &config.dof_focal_plane, 0.0f, 20.0f);
            ui::end_panel(&panel);

            if(changed) {
                reset_rendering();   
            }
        }
        ui::end();

        graphics::swap_frames();
    }

    ui::release();
    graphics::release();

    return 0;
}