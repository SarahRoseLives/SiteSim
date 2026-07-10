#include "App.hpp"
#include "ui/SitePanel.hpp"
#include "ui/CCPanel.hpp"
#include "ui/LogPanel.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include <cstdio>
#include <cmath>
#include <memory>

App::App() : m_cc(m_tx), m_rx(m_cc, [this](const std::string&) {}) {}

App::~App() { shutdown(); }

bool App::init() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init error: %s\n", SDL_GetError());
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    m_window = SDL_CreateWindow(
        "SiteSim - P25 Control Channel",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 800,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );
    if (!m_window) {
        fprintf(stderr, "SDL_CreateWindow error: %s\n", SDL_GetError());
        return false;
    }

    m_glCtx = SDL_GL_CreateContext(m_window);
    SDL_GL_MakeCurrent(m_window, m_glCtx);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding   = 3.0f;
    style.FrameRounding    = 2.0f;
    style.GrabRounding     = 2.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize  = 0.0f;

    auto& colors = style.Colors;
    colors[ImGuiCol_WindowBg]       = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    colors[ImGuiCol_ChildBg]        = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_Header]         = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_HeaderHovered]  = ImVec4(0.25f, 0.45f, 0.80f, 1.00f);
    colors[ImGuiCol_Button]         = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    colors[ImGuiCol_ButtonHovered]  = ImVec4(0.29f, 0.44f, 0.62f, 1.00f);
    colors[ImGuiCol_ButtonActive]   = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    colors[ImGuiCol_FrameBg]        = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_TitleBg]        = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    colors[ImGuiCol_TitleBgActive]  = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    colors[ImGuiCol_Tab]            = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    colors[ImGuiCol_TabActive]      = ImVec4(0.20f, 0.41f, 0.68f, 1.00f);
    colors[ImGuiCol_TabHovered]     = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    colors[ImGuiCol_DockingPreview] = ImVec4(0.29f, 0.54f, 0.90f, 0.70f);

    ImGui_ImplSDL2_InitForOpenGL(m_window, m_glCtx);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    m_deviceList = m_tx.listDevices();

    // Auto-open first real device found
    bool opened = false;
    for (const auto& dev : m_deviceList) {
        if (dev.find("(no device") == std::string::npos) {
            if (m_tx.open(dev)) {
                m_statusMsg = "Device ready: " + dev;
                opened = true;
            } else {
                m_statusMsg = "Device open failed: " + m_tx.lastError();
            }
            break;
        }
    }
    if (!opened && m_tx.lastError().empty())
        m_statusMsg = "No SDR device found - simulation mode";

    return true;
}

void App::run() {
    // Panels live here so they capture references correctly
    SitePanel sitePanel(m_cc, m_tx);
    CCPanel   ccPanel(m_cc, m_tx, m_rx);
    LogPanel  logPanel(m_cc);

    // Wire RxPipeline log output to the RX log tab
    m_rx.setLogCallback([&logPanel](const std::string& msg) {
        logPanel.addRxEntry(msg);
    });

    while (m_running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) m_running = false;
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_CLOSE) m_running = false;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        setupDocking();
        renderMenuBar();
        renderStatusBar();

        sitePanel.render();
        ccPanel.render(sitePanel.config());
        logPanel.render();

        ImGui::Render();
        ImGuiIO& io = ImGui::GetIO();
        glViewport(0, 0, static_cast<int>(io.DisplaySize.x), static_cast<int>(io.DisplaySize.y));
        glClearColor(0.08f, 0.08f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(m_window);
    }
}

void App::setupDocking() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float menuH = ImGui::GetFrameHeight();
    float statH = 22.0f;

    ImVec2 pos  = { vp->Pos.x, vp->Pos.y + menuH };
    ImVec2 size = { vp->Size.x, vp->Size.y - menuH - statH };

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::SetNextWindowViewport(vp->ID);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize   | ImGuiWindowFlags_NoMove     |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoDocking  | ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##DockSpace", nullptr, flags);
    ImGui::PopStyleVar();

    ImGuiID dockId = ImGui::GetID("MainDock");
    ImGui::DockSpace(dockId, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);

    if (!m_dockLayoutDone) {
        m_dockLayoutDone = true;

        ImGui::DockBuilderRemoveNode(dockId);
        ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockId, size);

        ImGuiID left, right;
        ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Left, 0.22f, &left, &right);
        ImGuiID rightTop, rightBottom;
        ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.40f, &rightBottom, &rightTop);

        ImGui::DockBuilderDockWindow("Site Config",     left);
        ImGui::DockBuilderDockWindow("Control Channel", rightTop);
        ImGui::DockBuilderDockWindow("Activity Log",    rightBottom);

        ImGui::DockBuilderFinish(dockId);
    }

    ImGui::End();
}

void App::renderMenuBar() {
    if (ImGui::BeginMainMenuBar()) {
        ImGui::TextColored(ImVec4(0.29f, 0.62f, 1.0f, 1.0f), "SiteSim");
        ImGui::Spacing();
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Quit", "Alt+F4")) m_running = false;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Device")) {
            if (ImGui::MenuItem("Refresh Device List")) {
                m_deviceList = m_tx.listDevices();
                m_statusMsg = "Device list refreshed.";
            }
            ImGui::Separator();
            for (size_t i = 0; i < m_deviceList.size(); i++) {
                bool isCurrent = m_tx.isOpen() && (i == 0); // simple: first = active
                if (ImGui::MenuItem(m_deviceList[i].c_str(), nullptr, isCurrent)) {
                    m_tx.close();
                    if (m_tx.open(m_deviceList[i]))
                        m_statusMsg = "Opened: " + m_deviceList[i];
                    else
                        m_statusMsg = "Failed: " + m_tx.lastError();
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

void App::renderStatusBar() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float statH = 22.0f;

    ImGui::SetNextWindowPos({ vp->Pos.x, vp->Pos.y + vp->Size.y - statH });
    ImGui::SetNextWindowSize({ vp->Size.x, statH });

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar   | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoMove       | ImGuiWindowFlags_NoResize    |
        ImGuiWindowFlags_NoCollapse   | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 3.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.10f, 0.10f, 1.0f));
    ImGui::Begin("##StatusBar", nullptr, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    if (!m_statusMsg.empty())
        ImGui::TextUnformatted(m_statusMsg.c_str());
    else
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "SiteSim v0.1  |  P25 Phase 1 TSCC Emulator");

    // Device indicator dot - right-aligned
    ImGui::SameLine();
    bool devOpen = m_tx.isOpen();
    ImVec4 dotCol = devOpen ? ImVec4(0.2f, 1.0f, 0.4f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
    float dotW = ImGui::CalcTextSize("[*] HackRF").x + 12.0f;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - dotW);
    ImGui::TextColored(dotCol, devOpen ? "[*] HackRF" : "[ ] No Device");

    ImGui::End();
}

void App::shutdown() {
    if (m_rx.isRunning()) m_rx.stop();
    if (m_cc.isRunning()) m_cc.stop();
    m_tx.stopTx();
    m_tx.close();

    if (m_glCtx) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        SDL_GL_DeleteContext(m_glCtx);
        m_glCtx = nullptr;
    }
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
    SDL_Quit();
}
