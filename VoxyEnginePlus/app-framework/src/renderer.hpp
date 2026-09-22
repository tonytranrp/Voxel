#pragma once
#include <daxa/daxa.hpp>
using namespace daxa::types;
#include "image.hpp"
#include "profiler.hpp"
#include "terminal-colors.hpp"
#include "window.hpp"
#include <chrono>
#include <daxa/utils/imgui.hpp>
#include <daxa/utils/pipeline_manager.hpp>
#include <daxa/utils/task_graph.hpp>
#include <daxa/utils/task_graph_types.hpp>
#include <functional>
#include <imgui_impl_glfw.h>
#include <iostream>
#include <string>
#include <vector>

struct InlineTask
{
    std::vector<daxa::TaskAttachmentInfo> attachments = {};
    std::function<void(daxa::TaskInterface)> task = {};
    std::string_view name = "unnamed";

    InlineTask(std::string_view name)
        : name(name)
    {
    }

    // make builder interface
    InlineTask &AddAttachment(daxa::TaskAttachmentInfo attachment)
    {
        attachments.push_back(attachment);
        return *this;
    }

    InlineTask &AddAttachment(daxa::TaskImageAccess access, daxa::TaskImageView view)
    {
        attachments.push_back(daxa::inl_attachment(access, view));
        return *this;
    }

    InlineTask &AddAttachment(daxa::TaskBufferAccess access, daxa::TaskBuffer buffer)
    {
        attachments.push_back(daxa::inl_attachment(access, buffer));
        return *this;
    }

    InlineTask &SetTask(std::function<void(daxa::TaskInterface)> task)
    {
        this->task = task;
        return *this;
    }

    InlineTask &AddAllAttachments(daxa::TaskImageAccess access, std::vector<daxa::TaskImage> images)
    {
        for (auto image : images)
        {
            AddAttachment(access, image);
        }
        return *this;
    }

    InlineTask &AddAllAttachments(daxa::TaskBufferAccess access, std::vector<daxa::TaskBuffer> buffers)
    {
        for (auto buffer : buffers)
        {
            AddAttachment(access, buffer);
        }
        return *this;
    }

  private:
    friend class Renderer;
    daxa::InlineTaskInfo build()
    {
        return {.attachments = attachments, .task = task, .name = name};
    }
};

class Renderer
{
  public:
    u32 surface_width = 0;
    u32 surface_height = 0;
    daxa::Instance daxa_instance;
    daxa::Device device;
    daxa::Swapchain swapchain;
    daxa::PipelineManager pipeline_manager;
    daxa::TaskImage task_swap_image;
    daxa::ImGuiRenderer imgui_renderer;
    daxa::TaskGraph render_loop_graph;
    bool task_graph_complete = false;

    /// Hot-reloading shaders stats every shader file on disk; doing that per frame costs
    /// milliseconds of CPU time and is a major source of frame time spikes. Poll rarely instead.
    static constexpr double SHADER_RELOAD_POLL_SECONDS = 0.5;

    /// @brief The renderer sets up a simple render loop which can be extended by adding tasks to the pipeline.
    /// @param window
    /// @param shaderDirectories Directories containing shaders. Those shaders can then be referenced via a relative path from the shader directory.
    Renderer(std::shared_ptr<Window> window, std::vector<std::filesystem::path> shaderDirectories)
        : window(window)
    {
        daxa_instance = daxa::create_instance({});
        device = daxa_instance.create_device_2(ChooseBestDevice(daxa_instance));
        InitProfiler();
        swapchain = CreateSwapchain(device, window, "swapchain");
        task_swap_image = CreateSwapchainImage(swapchain, "swapchain_image");
        pipeline_manager = CreatePipelineManager(device, shaderDirectories, "pipeline_manager");
        imgui_renderer = CreateImguiRenderer();

        Resize(window->GetWidth(), window->GetHeight());
        InitializeGraph();
    }

    ~Renderer()
    {
        device.wait_idle();
        device.collect_garbage();
        ImGui_ImplGlfw_Shutdown();
    }

    void Render()
    {
        if (!task_graph_complete)
        {
            printf(RED "Task graph not completed before frame start!!\n" RESET);
            Complete();
        }

        PollShaderReload();
        ResolveProfilerQueries();

        auto acquire_start = std::chrono::high_resolution_clock::now();
        auto swapchain_image = swapchain.acquire_next_image();
        Profiler::cpu_wait_ms = std::chrono::duration<float, std::milli>(
                                    std::chrono::high_resolution_clock::now() - acquire_start)
                                    .count();

        task_swap_image.set_images({.images = std::array{swapchain_image}});
        if (swapchain_image.is_empty())
        {
            std::cout << "Failed to acquire next image\n";
            return;
        }

        render_loop_graph.execute({});
        profile_slot = (profile_slot + 1) % PROFILE_SLOTS;
    }

    /// Hot reload is only useful while iterating on shaders, and it is not free: poll it on a timer.
    void PollShaderReload()
    {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - last_reload_poll).count() < SHADER_RELOAD_POLL_SECONDS)
            return;
        last_reload_poll = now;

        auto reloaded_result = pipeline_manager.reload_all();
        if (auto reload_err = daxa::get_if<daxa::PipelineReloadError>(&reloaded_result))
            std::cout << "Failed to reload " << reload_err->message << '\n';
        if (daxa::get_if<daxa::PipelineReloadSuccess>(&reloaded_result))
            std::cout << "Successfully reloaded!\n";
    }

    void Resize(u32 width, u32 height)
    {
        swapchain.resize();
        surface_width = swapchain.get_surface_extent().x;
        surface_height = swapchain.get_surface_extent().y;
    }

    inline void Complete()
    {

        auto imgui_task_info = daxa::InlineTaskInfo{
            .attachments = {daxa::inl_attachment(daxa::TaskImageAccess::COLOR_ATTACHMENT, task_swap_image)},
            .task = [this](daxa::TaskInterface const &ti)
            {
                imgui_renderer.record_commands(ImGui::GetDrawData(), ti.recorder, ti.get(task_swap_image).ids[0], surface_width, surface_height);
            },
            .name = "ImGui Task",
        };

        render_loop_graph.add_task(InstrumentTask(imgui_task_info));

        render_loop_graph.submit({});
        render_loop_graph.present({});
        render_loop_graph.complete({});
        task_graph_complete = true;
    }

    /// @brief Add a task to the render loop
    /// @param task The task to add. This can be a new insteance of InlineTask.
    /// @param defer_completion If true, the task graph will not be rebuilt (but will be reinitialized). This only matters if the graph was already completed before this task was added.
    inline void AddTask(InlineTask task, bool defer_completion = false)
    {
        AddTask(task.build(), defer_completion);
    }

    /// @brief Add a task to the render loop
    /// @param task The task to add as an InlineTaskInfo.
    /// @param defer_completion If true, the task graph will not be rebuilt (but will be reinitialized). This only matters if the graph was already completed before this task was added.
    inline void AddTask(daxa::InlineTaskInfo task, bool defer_completion = false)
    {
        if (task_graph_complete)
        {
            InitializeGraph();
        }

        render_loop_graph.add_task(InstrumentTask(std::move(task)));

        if (!defer_completion && task_graph_complete)
        {
            Complete();
        }
    }

    /// Dump an image to a raw RGBA file. Used by the benchmark harness to prove that an
    /// optimisation did not change what is on screen: two builds should produce identical bytes.
    /// Stalls the GPU, so it is only ever called from capture runs.
    void CaptureImageRaw(daxa::ImageId image, std::string const &path)
    {
        auto info = device.image_info(image).value();
        u32 const w = info.size.x;
        u32 const h = info.size.y;
        usize const bytes = usize(w) * usize(h) * 4u;

        auto staging = device.create_buffer({
            .size = bytes,
            .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
            .name = "capture_staging",
        });

        auto recorder = device.create_command_recorder({});
        recorder.pipeline_barrier_image_transition({
            .src_access = {.stages = daxa::PipelineStageFlagBits::ALL_COMMANDS, .type = daxa::AccessTypeFlagBits::READ_WRITE},
            .dst_access = {.stages = daxa::PipelineStageFlagBits::TRANSFER, .type = daxa::AccessTypeFlagBits::READ},
            .src_layout = daxa::ImageLayout::GENERAL,
            .dst_layout = daxa::ImageLayout::TRANSFER_SRC_OPTIMAL,
            .image_id = image,
        });
        recorder.copy_image_to_buffer({
            .image = image,
            .image_layout = daxa::ImageLayout::TRANSFER_SRC_OPTIMAL,
            .image_slice = {0, 0},
            .image_offset = {0, 0, 0},
            .image_extent = {w, h, 1},
            .buffer = staging,
        });
        recorder.pipeline_barrier_image_transition({
            .src_access = {.stages = daxa::PipelineStageFlagBits::TRANSFER, .type = daxa::AccessTypeFlagBits::READ},
            .dst_access = {.stages = daxa::PipelineStageFlagBits::ALL_COMMANDS, .type = daxa::AccessTypeFlagBits::READ_WRITE},
            .src_layout = daxa::ImageLayout::TRANSFER_SRC_OPTIMAL,
            .dst_layout = daxa::ImageLayout::GENERAL,
            .image_id = image,
        });

        auto commands = recorder.complete_current_commands();
        recorder.~CommandRecorder();
        device.submit_commands({.command_lists = std::array{commands}});
        device.wait_idle();

        auto *src = device.get_host_address_as<std::byte>(staging).value();
        std::FILE *f = std::fopen(path.c_str(), "wb");
        if (f != nullptr)
        {
            u32 header[2] = {w, h};
            std::fwrite(header, sizeof(u32), 2, f);
            std::fwrite(src, 1, bytes, f);
            std::fclose(f);
            std::cout << "[capture] wrote " << path << " (" << w << "x" << h << ")" << std::endl;
        }

        device.destroy_buffer(staging);
        device.collect_garbage();
    }

    inline void DestroyImage(daxa::ImageId image)
    {
        device.destroy_image(image);
    }

    inline void DestroyBuffer(daxa::BufferId buffer)
    {
        device.destroy_buffer(buffer);
    }

    inline daxa::ImageId CreateImage(daxa::ImageInfo info)
    {
        return device.create_image(info);
    }

    /// Upload several CPU images in one submit.
    ///
    /// The single-image CreateImage below does its own submit + wait_idle + collect_garbage, which
    /// is fine once but costs a full GPU round trip per image. Loading the 64 blue-noise textures
    /// that way was several seconds of startup; batched it is one round trip.
    inline std::vector<daxa::ImageId> CreateImages(std::vector<std::pair<std::string, Image *>> const &images)
    {
        std::vector<daxa::ImageId> ids;
        std::vector<daxa::BufferId> staging;
        ids.reserve(images.size());
        staging.reserve(images.size());

        auto recorder = device.create_command_recorder({});

        for (auto const &[name, image] : images)
        {
            auto id = CreateImage(daxa::ImageInfo{
                .format = daxa::Format::R8G8B8A8_UNORM,
                .size = {(unsigned int)image->width(), (unsigned int)image->height(), 1},
                .usage = daxa::ImageUsageFlagBits::TRANSFER_DST | daxa::ImageUsageFlagBits::SHADER_STORAGE,
                .name = name,
            });

            auto buffer = device.create_buffer({
                .size = image->size(),
                .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_SEQUENTIAL_WRITE,
            });
            std::memcpy(device.get_host_address_as<std::byte>(buffer).value(), image->data(), image->size());

            recorder.pipeline_barrier_image_transition({
                .src_access = {.stages = daxa::PipelineStageFlagBits::NONE, .type = daxa::AccessTypeFlagBits::READ},
                .dst_access = {.stages = daxa::PipelineStageFlagBits::TRANSFER, .type = daxa::AccessTypeFlagBits::WRITE},
                .src_layout = daxa::ImageLayout::UNDEFINED,
                .dst_layout = daxa::ImageLayout::TRANSFER_DST_OPTIMAL,
                .image_id = id,
            });
            recorder.copy_buffer_to_image({
                .buffer = buffer,
                .image = id,
                .image_layout = daxa::ImageLayout::TRANSFER_DST_OPTIMAL,
                .image_slice = {0, 0},
                .image_offset = {0, 0, 0},
                .image_extent = {(unsigned int)image->width(), (unsigned int)image->height(), 1},
            });
            recorder.pipeline_barrier_image_transition({
                .src_access = {.stages = daxa::PipelineStageFlagBits::TRANSFER, .type = daxa::AccessTypeFlagBits::WRITE},
                .dst_access = {.stages = daxa::PipelineStageFlagBits::NONE, .type = daxa::AccessTypeFlagBits::READ},
                .src_layout = daxa::ImageLayout::TRANSFER_DST_OPTIMAL,
                .dst_layout = daxa::ImageLayout::READ_ONLY_OPTIMAL,
                .image_id = id,
            });

            ids.push_back(id);
            staging.push_back(buffer);
        }

        auto executable_commands = recorder.complete_current_commands();
        recorder.~CommandRecorder();
        device.submit_commands({.command_lists = std::array{executable_commands}});
        device.wait_idle();
        device.collect_garbage();

        for (auto buffer : staging)
            device.destroy_buffer(buffer);

        return ids;
    }

    inline daxa::ImageId CreateImage(std::string name, Image &image)
    {
        auto id = CreateImage(daxa::ImageInfo{
            .format = daxa::Format::R8G8B8A8_UNORM,
            .size = {(unsigned int)image.width(), (unsigned int)image.height(), 1},
            .usage = daxa::ImageUsageFlagBits::TRANSFER_DST | daxa::ImageUsageFlagBits::SHADER_STORAGE,
            .name = name,
        });

        // create temp buffer
        auto buffer = device.create_buffer({
            .size = image.size(),
            .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_SEQUENTIAL_WRITE,
        });

        // copy image data to buffer
        auto data = device.get_host_address_as<std::byte>(buffer).value();
        std::memcpy(data, image.data(), image.size());

        // execute a task to copy the image data to the gpu
        auto recorder = device.create_command_recorder({});

        recorder.pipeline_barrier_image_transition({
            .src_access =
                {
                    .stages = daxa::PipelineStageFlagBits::NONE,
                    .type = daxa::AccessTypeFlagBits::READ},
            .dst_access =
                {
                    .stages = daxa::PipelineStageFlagBits::TRANSFER,
                    .type = daxa::AccessTypeFlagBits::WRITE},
            .src_layout = daxa::ImageLayout::UNDEFINED,
            .dst_layout = daxa::ImageLayout::TRANSFER_DST_OPTIMAL,
            .image_id = id,
        });

        recorder.copy_buffer_to_image({
            .buffer = buffer,
            .image = id,
            .image_layout = daxa::ImageLayout::TRANSFER_DST_OPTIMAL,
            .image_slice = {0, 0},
            .image_offset = {0, 0, 0},
            .image_extent = {(unsigned int)image.width(), (unsigned int)image.height(), 1},
        });

        recorder.pipeline_barrier_image_transition({
            .src_access =
                {
                    .stages = daxa::PipelineStageFlagBits::TRANSFER,
                    .type = daxa::AccessTypeFlagBits::WRITE},
            .dst_access =
                {
                    .stages = daxa::PipelineStageFlagBits::NONE,
                    .type = daxa::AccessTypeFlagBits::READ},
            .src_layout = daxa::ImageLayout::TRANSFER_DST_OPTIMAL,
            .dst_layout = daxa::ImageLayout::READ_ONLY_OPTIMAL,
            .image_id = id,
        });

        auto executable_commands = recorder.complete_current_commands();
        recorder.~CommandRecorder();
        device.submit_commands({.command_lists = std::array{executable_commands}});
        device.wait_idle();
        device.collect_garbage();

        device.destroy_buffer(buffer);

        return id;
    }

    static constexpr daxa::ImageUsageFlags color_image_flags = daxa::ImageUsageFlagBits::SHADER_STORAGE | daxa::ImageUsageFlagBits::COLOR_ATTACHMENT | daxa::ImageUsageFlagBits::TRANSFER_SRC | daxa::ImageUsageFlagBits::TRANSFER_DST;
    static constexpr daxa::ImageUsageFlags depth_image_flags = daxa::ImageUsageFlagBits::SHADER_STORAGE | daxa::ImageUsageFlagBits::DEPTH_STENCIL_ATTACHMENT | daxa::ImageUsageFlagBits::TRANSFER_SRC | daxa::ImageUsageFlagBits::TRANSFER_DST;
    static constexpr daxa::ImageUsageFlags transfer_image_flags = daxa::ImageUsageFlagBits::SHADER_STORAGE | daxa::ImageUsageFlagBits::TRANSFER_SRC | daxa::ImageUsageFlagBits::TRANSFER_DST;

    inline daxa::ImageId CreateRenderImage(std::string name, daxa::ImageUsageFlags flags = color_image_flags)
    {
        return CreateImage({
            .format = swapchain.get_format(),
            .size = {surface_width, surface_height, 1},
            .usage = flags,
            .name = name,
        });
    }

    inline daxa::ImageId CreateRenderImage(std::string name, daxa::Format format, float scale = 1.0, daxa::ImageUsageFlags flags = color_image_flags)
    {
        return CreateImage({
            .format = format,
            .size = {static_cast<u32>(surface_width * scale), static_cast<u32>(surface_height * scale), 1},
            .usage = flags,
            .name = name,
        });
    }

    inline void SetTaskImage(std::string name, daxa::ImageId image, daxa::TaskImage *out_task_image)
    {
        if (out_task_image->is_valid())
        {
            out_task_image->set_images({.images = std::array{image}});
        }
        else
        {
            *out_task_image = daxa::TaskImage({.initial_images = {.images = std::array{image}}, .name = ("task_" + name).c_str()});
        }
    }

    inline daxa::ImageId CreateRenderImage(std::string name, daxa::TaskImage *out_task_image, daxa::Format format, float scale = 1.0, daxa::ImageUsageFlags flags = color_image_flags)
    {
        auto id = CreateRenderImage(name, format, scale, flags);
        SetTaskImage(name, id, out_task_image);
        return id;
    }

    inline daxa::ImageId CreateImage(daxa::ImageInfo info, daxa::TaskImage *out_task_image)
    {
        auto id = CreateImage(info);
        SetTaskImage(std::string(info.name.view()), id, out_task_image);
        return id;
    }

    inline daxa::ImageId CreateRenderImage(std::string name, daxa::TaskImage *out_task_image, daxa::ImageUsageFlags flags = color_image_flags)
    {
        auto id = CreateRenderImage(name, flags);
        SetTaskImage(name, id, out_task_image);
        return id;
    }

    template <typename PushConstant>
    inline std::shared_ptr<daxa::ComputePipeline> AddComputePipeline(std::string name, std::filesystem::path shaderPath)
    {
        auto result = pipeline_manager.add_compute_pipeline({
            .shader_info = {.source = daxa::ShaderFile{shaderPath}},
            .push_constant_size = sizeof(PushConstant),
            .name = name,
        });

        if (!result.is_ok())
        {
            std::cout << RED << "Failed to compile compute pipeline '" << name << "' (" << shaderPath.string() << "):\n"
                      << result.message() << RESET << std::endl;
            throw std::runtime_error("shader compilation failed: " + name);
        }

        return result.value();
    }

    inline void CreateBuffer(std::string name, usize bytes, daxa::BufferId &out_buffer, daxa::TaskBuffer &out_task_buffer, daxa::MemoryFlags flags = daxa::MemoryFlagBits::NONE)
    {
        out_buffer = device.create_buffer({
            .size = bytes,
            .allocate_info = flags,
        });

        out_task_buffer = daxa::TaskBuffer({.initial_buffers = {.buffers = std::array{out_buffer}}, .name = ("task_" + name).c_str()});
    }

    template <typename T>
    inline void CreateBuffer(std::string name, daxa::BufferId &out_buffer, daxa::TaskBuffer &out_task_buffer, daxa::MemoryFlags flags = daxa::MemoryFlagBits::NONE)
    {
        CreateBuffer(name, sizeof(T), out_buffer, out_task_buffer, flags);
    }

    template <typename T>
    inline T *MapBufferAs(daxa::BufferId buffer)
    {
        return device.get_host_address_as<T>(buffer).value();
    }

    inline daxa::InlineTaskInfo CreateBlitTask(daxa::TaskImage src, daxa::TaskImage dst)
    {
        return {
            .attachments = {
                {daxa::inl_attachment(daxa::TaskImageAccess::TRANSFER_READ, src)},
                {daxa::inl_attachment(daxa::TaskImageAccess::TRANSFER_WRITE, dst)},
            },
            .task = [src, dst, this](daxa::TaskInterface task)
            {
                task.recorder.blit_image_to_image({
                    .src_image = task.get(src).ids[0],
                    .src_image_layout = daxa::ImageLayout::TRANSFER_SRC_OPTIMAL,
                    .dst_image = task.get(dst).ids[0],
                    .dst_image_layout = daxa::ImageLayout::TRANSFER_DST_OPTIMAL,
                    .src_offsets = {{{0, 0, 0}, {(i32)(surface_width), (i32)(surface_height), 1}}},
                    .dst_offsets = {{{0, 0, 0}, {(i32)(surface_width), (i32)(surface_height), 1}}},
                });
            },
            .name = "Blit " + std::string(src.info().name) + " -> " + std::string(dst.info().name),
        };
    }

    inline daxa::InlineTaskInfo CreateSwapchainBlitTask(daxa::TaskImage src)
    {
        return CreateBlitTask(src, task_swap_image);
    }

  private:
    std::shared_ptr<Window> window;
    std::chrono::steady_clock::time_point last_reload_poll = {};

    // --- GPU profiling ---------------------------------------------------
    // Each pass gets a begin/end timestamp. Queries are ring-buffered over a few
    // frames so reading results never stalls the CPU on in-flight work.
    static constexpr u32 MAX_PROFILED_PASSES = 32;
    static constexpr u32 PROFILE_SLOTS = 3;
    static constexpr u32 QUERIES_PER_SLOT = MAX_PROFILED_PASSES * 2;

    daxa::TimelineQueryPool timeline_query_pool = {};
    std::vector<std::string> pass_names = {};
    u32 profile_slot = 0;
    f32 timestamp_period_ns = 1.0f;

    void InitProfiler()
    {
        timestamp_period_ns = device.properties().limits.timestamp_period;
        timeline_query_pool = device.create_timeline_query_pool({
            .query_count = QUERIES_PER_SLOT * PROFILE_SLOTS,
            .name = "gpu_profiler",
        });
        Profiler::device_name = std::string(reinterpret_cast<char const *>(device.properties().device_name));
    }

    /// Wrap a task so it brackets its own GPU work with timestamps.
    daxa::InlineTaskInfo InstrumentTask(daxa::InlineTaskInfo task)
    {
        if (pass_names.size() >= MAX_PROFILED_PASSES)
            return task;

        u32 const pass_index = static_cast<u32>(pass_names.size());
        pass_names.push_back(std::string(task.name));

        auto inner = task.task;
        task.task = [this, pass_index, inner](daxa::TaskInterface ti)
        {
            u32 const base = profile_slot * QUERIES_PER_SLOT + pass_index * 2;
            ti.recorder.reset_timestamps({.query_pool = timeline_query_pool, .start_index = base, .count = 2});
            ti.recorder.write_timestamp({
                .query_pool = timeline_query_pool,
                .pipeline_stage = daxa::PipelineStageFlagBits::TOP_OF_PIPE,
                .query_index = base,
            });

            if (inner)
                inner(ti);

            ti.recorder.write_timestamp({
                .query_pool = timeline_query_pool,
                .pipeline_stage = daxa::PipelineStageFlagBits::BOTTOM_OF_PIPE,
                .query_index = base + 1,
            });
        };

        return task;
    }

    /// Read back the oldest in-flight slot. Anything not yet available is simply skipped.
    void ResolveProfilerQueries()
    {
        if (pass_names.empty())
            return;

        if (Profiler::passes.size() != pass_names.size())
        {
            Profiler::passes.resize(pass_names.size());
            for (size_t i = 0; i < pass_names.size(); i++)
                Profiler::passes[i].name = pass_names[i];
        }

        u32 const oldest = (profile_slot + 1) % PROFILE_SLOTS;
        u32 const base = oldest * QUERIES_PER_SLOT;
        auto results = timeline_query_pool.get_query_results(base, static_cast<u32>(pass_names.size()) * 2);

        float total = 0.0f;
        for (size_t i = 0; i < pass_names.size(); i++)
        {
            u64 const begin = results[i * 4 + 0];
            u64 const begin_ready = results[i * 4 + 1];
            u64 const end = results[i * 4 + 2];
            u64 const end_ready = results[i * 4 + 3];

            if (begin_ready == 0 || end_ready == 0 || end < begin)
                continue;

            float const ms = static_cast<float>(end - begin) * timestamp_period_ns * 1e-6f;
            Profiler::Smooth(Profiler::passes[i], ms);
            total += ms;
        }

        if (total > 0.0f)
        {
            Profiler::gpu_total_raw_ms = total;
            Profiler::gpu_total_ms = Profiler::gpu_total_ms * 0.92f + total * 0.08f;
        }
    }

    /// daxa's choose_device takes the first device that satisfies the requested features,
    /// which on a laptop is usually the integrated GPU. Prefer a discrete GPU explicitly.
    static daxa::DeviceInfo2 ChooseBestDevice(daxa::Instance &instance)
    {
        auto devices = instance.list_devices_properties();

        int best_index = -1;
        i32 best_score = -1;
        std::cout << "Vulkan devices:\n";
        for (u32 i = 0; i < devices.size(); i++)
        {
            auto const &d = devices[i];
            char const *type_name = "other";
            i32 score = 0;
            switch (d.device_type)
            {
            case daxa::DeviceType::DISCRETE_GPU:
                type_name = "discrete";
                score = 1000;
                break;
            case daxa::DeviceType::VIRTUAL_GPU:
                type_name = "virtual";
                score = 500;
                break;
            case daxa::DeviceType::INTEGRATED_GPU:
                type_name = "integrated";
                score = 100;
                break;
            case daxa::DeviceType::CPU:
                type_name = "cpu";
                score = 1;
                break;
            default:
                break;
            }

            std::cout << "  [" << i << "] " << reinterpret_cast<char const *>(d.device_name)
                      << " (" << type_name << ")\n";

            if (score > best_score)
            {
                best_score = score;
                best_index = static_cast<int>(i);
            }
        }

        if (best_index < 0)
            return instance.choose_device({}, {});

        std::cout << "  -> using [" << best_index << "] "
                  << reinterpret_cast<char const *>(devices[best_index].device_name) << "\n";

        return daxa::DeviceInfo2{
            .physical_device_index = static_cast<u32>(best_index),
            .name = "device",
        };
    }

    void InitializeGraph()
    {
        task_graph_complete = false;
        pass_names.clear();
        Profiler::passes.clear();
        render_loop_graph = daxa::TaskGraph({
            .device = device,
            .swapchain = swapchain,
            // Split barriers let independent passes overlap instead of fully serialising.
            .use_split_barriers = true,
            // Debug recording allocates and formats strings for every task, every frame.
            .record_debug_information = false,
            .name = "render_loop",
        });

        render_loop_graph.use_persistent_image(task_swap_image);
    }

    inline daxa::ImGuiRenderer CreateImguiRenderer()
    {
        auto ctx = ImGui::CreateContext();

        FontManager::LoadDefaults();

        ImGui_ImplGlfw_InitForVulkan(window->GetGlfwWindow(), true);
        return daxa::ImGuiRenderer(
            {.device = device,
             .format = swapchain.get_format(),
             .context = ctx});
    }

  public:
    template <typename T>
    static inline void CopyToBuffer(daxa::TaskInterface ti, T src, daxa::TaskBufferAttachmentInfo dst, u32 dst_offset = 0)
    {
        auto alloc = ti.allocator->allocate_fill(src).value();
        ti.recorder.copy_buffer_to_buffer({
            .src_buffer = ti.allocator->buffer(),
            .dst_buffer = dst.ids[0],
            .src_offset = alloc.buffer_offset,
            .dst_offset = dst_offset,
            .size = sizeof(T),
        });
    }

    static inline void CopyBuffer(daxa::TaskInterface ti, daxa::TaskBuffer src, daxa::TaskBuffer dst, u32 dst_offset = 0)
    {
        ti.recorder.copy_buffer_to_buffer({
            .src_buffer = ti.get(src).ids[0],
            .dst_buffer = ti.get(dst).ids[0],
            .src_offset = 0,
            .dst_offset = dst_offset,
            .size = ti.device.buffer_info(ti.get(src).ids[0]).value().size,
        });
    }

    static inline void ClearBuffer(daxa::TaskInterface ti, daxa::TaskBufferAttachmentInfo buffer, u32 value = 0)
    {
        ti.recorder.clear_buffer(
            {.buffer = buffer.ids[0],
             .offset = 0,
             .size = ti.device.buffer_info(buffer.ids[0]).value().size,
             .clear_value = value});
    }

    static inline void ClearBuffer(daxa::TaskInterface ti, daxa::TaskBuffer buffer, u32 value = 0)
    {
        ClearBuffer(ti, ti.get(buffer), value);
    }

    static inline void CopyImage(daxa::TaskInterface ti, daxa::TaskImage src, daxa::TaskImage dst)
    {
        ti.recorder.copy_image_to_image({.src_image = ti.get(src).ids[0],
                                         .dst_image = ti.get(dst).ids[0]});
    }

    static inline DeviceAddress GetDeviceAddress(daxa::TaskInterface ti, daxa::TaskBuffer buffer, usize bufferIndex = 0)
    {
        return ti.device.buffer_device_address(ti.get(buffer).ids[bufferIndex]).value();
    }

    static inline daxa::Swapchain CreateSwapchain(daxa::Device &device, std::shared_ptr<Window> window, std::string name)
    {
        // FIFO blocks the CPU until vblank, so a frame that misses the refresh interval costs a
        // whole extra interval - that is what reads as stutter. MAILBOX presents the newest frame
        // without blocking and without tearing; fall back to FIFO when the driver lacks it.
        auto supported = device.get_supported_present_modes(window->GetNativeHandle(), window->GetNativePlatform());
        auto present_mode = daxa::PresentMode::FIFO;
        for (auto mode : supported)
        {
            if (mode == daxa::PresentMode::MAILBOX)
            {
                present_mode = daxa::PresentMode::MAILBOX;
                break;
            }
        }

        return device.create_swapchain(
            {.native_window = window->GetNativeHandle(),
             .native_window_platform = window->GetNativePlatform(),
             .present_mode = present_mode,
             .image_usage = daxa::ImageUsageFlagBits::TRANSFER_DST,
             .name = name});
    }

    static inline daxa::TaskImage CreateSwapchainImage(daxa::Swapchain &swapchain, std::string name)
    {
        return daxa::TaskImage{{.swapchain_image = true, .name = name}};
    }

    static inline daxa::PipelineManager CreatePipelineManager(daxa::Device &device, std::vector<std::filesystem::path> shaderDirectories, std::string name)
    {
        shaderDirectories.push_back(DAXA_SHADER_INCLUDE_DIR);
        return daxa::PipelineManager({
            .device = device,
            .shader_compile_options = {
                .root_paths = shaderDirectories,
                .language = daxa::ShaderLanguage::SLANG,
                .enable_debug_info = true,
            },
            .name = name,
        });
    }
};