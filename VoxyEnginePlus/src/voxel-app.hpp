#pragma once
#include "user-interface\root.hpp"
#include "vox/shaders/shared.inl"

#include "vox/vox-camera.hpp"
#include "vox/vox-renderer.hpp"
#include <application.hpp>
#include <benchmark.hpp>
#include <input.hpp>
#include <stdio.h>

struct VoxyApp : public Application
{
  public:
    VoxelRenderer voxelRenderer;
    UIData uiState;
    daxa::ImageId render_image;
    daxa::TaskImage task_render_image;

    VoxyApp()
        // Both shader roots are passed, searched in order. "resources/shaders" is the copy CMake
        // drops next to the executable, so a packaged build works standing on its own; the source
        // tree path keeps hot-reload working when running from the build directory.
        : Application("Voxy", {"resources/shaders", "../../../../src/vox/shaders"}),
          voxelRenderer(renderer),
          uiState({.renderData = &voxelRenderer.stateData})
    {
        render_image = renderer->CreateRenderImage("game_render_image", &task_render_image);
        voxelRenderer.camera.speed = 6.0f;
        voxelRenderer.camera.sensitivity = 0.05f;
    }

    ~VoxyApp()
    {
        renderer->DestroyImage(render_image);

        ClayState::FreeAllStrings();
    }

  protected:
    void OnStart() override
    {
        voxelRenderer.InitializeTasks(renderer->render_loop_graph, &task_render_image, &render_image);
        renderer->Complete();
        Benchmark::Init();
    }

    /// Scripted camera path + sampling for VOXY_BENCH runs. Keeps the measured workload identical
    /// between builds so before/after numbers actually mean something.
    void RunBenchmarkFrame(float dt)
    {
        float const t = Benchmark::PathTime();

        float x, y, z, yaw, pitch;
        Benchmark::SampleCameraPath(t, x, y, z, yaw, pitch);
        voxelRenderer.camera.setPosition(Vector3(x, y, z));
        voxelRenderer.camera.setOrientation(yaw, pitch);

        // Feed the shaders the virtual timestep too, so temporal accumulation behaves identically
        // regardless of how fast the build renders.
        voxelRenderer.stateData.dt = Benchmark::virtual_dt;
        voxelRenderer.stateData.time = t;

        Benchmark::Record(dt * 1000.0f);

        // Still run the UI pass so the measured frame matches what the game actually does.
        uiState.time = t;
        UIInputs input =
            {
                .screenWidth = (int)window->GetWidth(),
                .screenHeight = (int)window->GetHeight(),
                .deltaTime = Benchmark::virtual_dt,
            };
        ClayUI::Update<UIData>(uiState, input, draw_root_ui);

        if (Benchmark::Finished())
        {
            Benchmark::Report();
            window->RequestClose();
        }

        voxelRenderer.Update();
    }

    void OnUpdate(float dt) override
    {
        // renderer state
        RenderData &renderData = voxelRenderer.stateData;
        renderData.dt = dt;
        renderData.time = GetTime();
        renderData.frame++;

        if (Benchmark::active)
        {
            RunBenchmarkFrame(dt);
            return;
        }

        // ui state
        uiState.time = GetTime();

        fflush(stdout);

        UIInputs input =
            {
                .screenWidth = (int)window->GetWidth(),
                .screenHeight = (int)window->GetHeight(),
                .pointerX = Input::GetMousePosition().x,
                .pointerY = Input::GetMousePosition().y,
                .pointerDown = Input::IsMouseButtonPressed(MouseButton::Left),
                .scrollDeltaX = Input::GetMouseScrollDelta().x,
                .scrollDeltaY = Input::GetMouseScrollDelta().y,
                .deltaTime = dt,
            };

        if (Input::WasKeyPressed(Key::F3))
        {
            Profiler::enabled = !Profiler::enabled;
        }

        if (Input::WasKeyPressed(Key::F4))
        {
            ClayUI::DebugMode(!ClayUI::GetDebugMode());
        }

        // if pressing L, sun dir is locked to camera
        if (Input::IsKeyPressed(Key::L))
        {
            renderData.sunDir = (-voxelRenderer.camera.getForward()).toDaxa();
        }

        // mouse capture
        if (Input::WasMouseButtonPressed(MouseButton::Left) && !ImGui::GetIO().WantCaptureMouse && !uiState.mouseIsActive)
        {
            Input::CaptureMouseResetDelta(true);
        }

        if (Input::WasKeyPressed(Key::Escape))
        {
            Input::CaptureMouseResetDelta(!Input::IsMouseCaptured()); // use escape to toggle mouse capture
        }

        // if mouse is captured, update camera
        if (Input::IsMouseCaptured())
        {
            auto delta = Input::GetMouseDelta();
            voxelRenderer.camera.processMouseMovement(delta, true);
            voxelRenderer.camera.processKeyboard(dt);
        }

        uiState.mouseIsActive = !Input::IsMouseCaptured(); // assuming that the mouse is active when it is not captured (so it is visible and can interact with UI)
        bool menuOpenBefore = uiState.mouseIsActive;
        ClayUI::Update<UIData>(uiState, input, draw_root_ui);

        if (menuOpenBefore && !uiState.mouseIsActive)
        {
            Input::CaptureMouseResetDelta(true);
        }

        voxelRenderer.Update();
    }

    void OnAfterRender() override
    {
        if (Benchmark::ShouldCapture())
        {
            renderer->CaptureImageRaw(render_image, Benchmark::CapturePath());
        }
    }

    void OnResize(u32 sx, u32 sy) override
    {
        voxelRenderer.Resize(sx, sy);
        renderer->DestroyImage(render_image);
        render_image = renderer->CreateRenderImage("game_render_image");
        task_render_image.set_images({.images = std::array{render_image}});
    }
};