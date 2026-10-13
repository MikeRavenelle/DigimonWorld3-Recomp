#include "wizard_ui.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#include "assets.h"
#include "disc_check.h"
#include "dw3/platform.h"
#include "dw3/texrep/pack_job.h"
#include "hd_install.h"
#include "process_runner.h"
#include "roboto_medium.h"

namespace dw3setup {

namespace {

namespace fs = std::filesystem;

constexpr ImU32 kAccent = IM_COL32(94, 160, 255, 255);
constexpr ImU32 kGood = IM_COL32(110, 205, 140, 255);
constexpr ImU32 kBad = IM_COL32(255, 125, 115, 255);
constexpr ImU32 kWarn = IM_COL32(240, 190, 90, 255);
constexpr ImU32 kMuted = IM_COL32(150, 158, 175, 255);

void apply_theme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 6.0f;
    style.ChildRounding = 8.0f;
    style.GrabRounding = 6.0f;
    style.FramePadding = ImVec2(14.0f, 8.0f);
    style.ItemSpacing = ImVec2(10.0f, 10.0f);
    style.WindowPadding = ImVec2(28.0f, 22.0f);
    style.FrameBorderSize = 0.0f;
    style.ScrollbarRounding = 6.0f;
    ImVec4* c = style.Colors;
    const ImVec4 bg(0.075f, 0.082f, 0.102f, 1.0f);
    const ImVec4 panel(0.115f, 0.125f, 0.152f, 1.0f);
    const ImVec4 raised(0.17f, 0.185f, 0.225f, 1.0f);
    const ImVec4 accent(0.37f, 0.63f, 1.0f, 1.0f);
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = panel;
    c[ImGuiCol_PopupBg] = panel;
    c[ImGuiCol_Text] = ImVec4(0.92f, 0.94f, 0.97f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.48f, 0.55f, 1.0f);
    c[ImGuiCol_Border] = ImVec4(0.22f, 0.24f, 0.29f, 1.0f);
    c[ImGuiCol_FrameBg] = raised;
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.21f, 0.23f, 0.28f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.26f, 0.32f, 1.0f);
    c[ImGuiCol_Button] = raised;
    c[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.30f, 0.38f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.30f, 0.36f, 0.46f, 1.0f);
    c[ImGuiCol_Header] = raised;
    c[ImGuiCol_HeaderHovered] = ImVec4(0.21f, 0.23f, 0.28f, 1.0f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.24f, 0.26f, 0.32f, 1.0f);
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_NavCursor] = accent;
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.37f, 0.63f, 1.0f, 0.35f);
    c[ImGuiCol_ScrollbarBg] = panel;
    c[ImGuiCol_ScrollbarGrab] = raised;
}

bool primary_button(const char* label, bool enabled = true) {
    ImGui::BeginDisabled(!enabled);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.45f, 0.82f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.52f, 0.92f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.33f, 0.58f, 0.98f, 1.0f));
    const bool pressed = ImGui::Button(label, ImVec2(0, 0));
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    return pressed;
}

void colored_wrapped(ImU32 color, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

// The file dialog answers on another thread (or later, from the event loop): park the answer for the UI to pick up.
struct DialogResult {
    std::mutex mutex;
    std::string path;
    bool has_path = false;
    bool failed = false;
};

void SDLCALL on_file_chosen(void* userdata, const char* const* files, int) {
    auto* result = static_cast<DialogResult*>(userdata);
    std::lock_guard lock(result->mutex);
    if (!files) result->failed = true;
    else if (files[0]) {
        result->path = files[0];
        result->has_path = true;
    }
}

class SetupUi {
public:
    SetupUi(SDL_Window* window, const WizardLaunch& launch) : window_(window), launch_(launch) {
        state_.set_disc_path(launch.initial_disc);
        std::snprintf(path_buffer_, sizeof path_buffer_, "%s", launch.initial_disc.c_str());
        plan_ = plan_for(dw3::platform::host_os(), dw3::platform::host_arch());
    }
    ~SetupUi() { stop_worker(); }

    WizardState& state() { return state_; }

    // Builds one frame of the window.
    void frame(float width, float height) {
        pump();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(width, height));
        ImGui::Begin("##setup", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        header();
        const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().WindowPadding.y + 6.0f;
        ImGui::BeginChild("##content", ImVec2(0, -footer), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        switch (state_.step()) {
            case Step::Welcome: welcome(); break;
            case Step::ChooseDisc: choose_disc(); break;
            case Step::Verify: verify(); break;
            case Step::HdOffer: hd_offer(); break;
            case Step::HdWork: hd_work(); break;
            case Step::Done: done(); break;
        }
        ImGui::EndChild();
        footer_buttons();
        ImGui::End();
        shortcuts();
        step_changed_ = state_.step() != shown_step_;
        // ImGui picks its own first item to focus when a window starts navigating; the primary button gets it back for two frames.
        focus_frames_ = step_changed_ ? 2 : std::max(0, focus_frames_ - 1);
        shown_step_ = state_.step();
    }

    bool step_changed() const { return step_changed_; }
    bool verify_waiting() const { return state_.verify_pending(); }
    bool worker_running() const { return worker_.joinable(); }
    HdSnapshot hd_snapshot() const { return installer_ ? installer_->snapshot() : HdSnapshot{}; }
    void request_stop() { if (installer_) installer_->cancel(); }

    // Scripted access for the unattended run.
    void demo_type_path(const std::string& path) {
        state_.set_disc_path(path);
        std::snprintf(path_buffer_, sizeof path_buffer_, "%s", path.c_str());
    }
    void demo_accept_hd() { begin_hd(); }

private:
    void header() {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.45f);
        ImGui::TextUnformatted("Digimon World 3 Recomp setup");
        ImGui::PopFont();
        const char* labels[] = {"Welcome", "Disc", "Verify", "HD backgrounds", "Done"};
        int current = 0;
        switch (state_.step()) {
            case Step::Welcome: current = 0; break;
            case Step::ChooseDisc: current = 1; break;
            case Step::Verify: current = 2; break;
            case Step::HdOffer:
            case Step::HdWork: current = 3; break;
            case Step::Done: current = 4; break;
        }
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImVec2 at = ImGui::GetCursorScreenPos();
        const float font = ImGui::GetFontSize();
        float x = at.x;
        for (int i = 0; i < 5; ++i) {
            const bool active = i == current;
            const ImU32 color = active ? kAccent : (i < current ? kGood : kMuted);
            const float radius = font * 0.42f;
            const ImVec2 center(x + radius, at.y + font * 0.55f);
            if (active || i < current) draw->AddCircleFilled(center, radius, color);
            else draw->AddCircle(center, radius, color, 0, 2.0f);
            const std::string number = std::to_string(i + 1);
            const ImVec2 size = ImGui::CalcTextSize(number.c_str());
            if (active || i < current)
                draw->AddText(ImVec2(center.x - size.x * 0.5f, center.y - size.y * 0.5f), IM_COL32(15, 18, 26, 255), number.c_str());
            else
                draw->AddText(ImVec2(center.x - size.x * 0.5f, center.y - size.y * 0.5f), color, number.c_str());
            const ImVec2 label_size = ImGui::CalcTextSize(labels[i]);
            draw->AddText(ImVec2(x + radius * 2 + 8.0f, at.y + font * 0.55f - label_size.y * 0.5f), active ? IM_COL32(235, 240, 250, 255) : color,
                          labels[i]);
            x += radius * 2 + 8.0f + label_size.x + 34.0f;
        }
        ImGui::Dummy(ImVec2(0, font * 1.4f));
    }

    void heading(const char* text) {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25f);
        ImGui::TextUnformatted(text);
        ImGui::PopFont();
        ImGui::Spacing();
    }

    void welcome() {
        heading("Welcome");
        ImGui::TextWrapped(
            "This program plays Digimon World 3 on your computer. It contains no game data: it needs a disc image of your own "
            "copy of the game, the USA release.");
        ImGui::Spacing();
        ImGui::TextWrapped("Setup takes a minute. You will:");
        ImGui::BulletText("choose your disc image (.cue, .bin or .iso)");
        ImGui::BulletText("let the program check that it is the right disc");
        ImGui::BulletText("optionally create HD versions of the field backgrounds on your own computer");
        ImGui::Spacing();
        colored_wrapped(kMuted, "Everything else (video, audio, controls, cheats) is in the game's own settings menu. Run with --setup to open this window again.");
    }

    void choose_disc() {
        heading("Choose your disc image");
        ImGui::TextWrapped("Select the .cue file of your Digimon World 3 (USA) dump, or type its path. A .bin or .iso with 2352-byte sectors also works.");
        ImGui::Spacing();
        const float browse_width = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SetNextItemWidth(-(browse_width + ImGui::GetStyle().ItemSpacing.x));
        if (ImGui::InputTextWithHint("##path", "path to the disc image", path_buffer_, sizeof path_buffer_)) state_.set_disc_path(path_buffer_);
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) open_dialog();
        {
            std::lock_guard lock(dialog_.mutex);
            if (dialog_.failed) colored_wrapped(kWarn, "The file picker could not be opened on this system. Type the path instead.");
        }
        ImGui::Spacing();
        colored_wrapped(kMuted, "The image is only read, never changed or copied. Only its location is saved.");
    }

    void verify() {
        heading("Checking the disc");
        ImGui::TextWrapped("%s", state_.disc_path().c_str());
        ImGui::Spacing();
        if (state_.verify_pending()) {
            if (verify_frames_++ >= 1) state_.finish_verify(check_disc(dw3::platform::path_from_utf8(state_.disc_path())));
            ImGui::TextUnformatted("Reading the disc...");
            return;
        }
        verify_frames_ = 0;
        const DiscCheck& result = state_.check();
        if (result.ok()) {
            colored_wrapped(kGood, "This is Digimon World 3 (USA), serial " + result.serial + ".");
            ImGui::Spacing();
            ImGui::TextWrapped("The game executable matches the retail release. You can continue.");
        } else {
            colored_wrapped(kBad, "This disc cannot be used.");
            ImGui::Spacing();
            ImGui::TextWrapped("%s", result.message.c_str());
            ImGui::Spacing();
            colored_wrapped(kMuted, "Go back and choose another image.");
        }
    }

    void hd_offer() {
        heading("Optional: HD backgrounds");
        ImGui::TextWrapped(
            "The game's field maps are pre-rendered pictures with a low resolution. A local AI upscaler (Real-ESRGAN) can enlarge them four "
            "times. It runs on your own computer, on your own disc: nothing from your disc is uploaded anywhere.");
        ImGui::Spacing();
        ImGui::TextWrapped("It needs a graphics card with Vulkan support and takes some time to run once. You can skip this now; the HD backgrounds can be set up later with --setup.");
        ImGui::Spacing();
        bool all_known = false;
        const uint64_t total = total_download_bytes(plan_, all_known);
        std::string size = plan_.unsupported.empty() ? format_bytes(total) : "not available";
        if (plan_.unsupported.empty() && !all_known) size += " plus the model files (a few MB)";
        ImGui::Text("Download size: %s", size.c_str());
        if (const std::string problem = plan_problem(plan_); !problem.empty()) colored_wrapped(kWarn, "Not ready in this build: " + problem);
        ImGui::Spacing();
        ImGui::TextWrapped("Licences: the upscaler (realesrgan-ncnn-vulkan) is MIT; the model weights (Real-ESRGAN) are BSD 3-Clause.");
        ImGui::Spacing();
        if (ImGui::CollapsingHeader("What is downloaded, and from where")) {
            ImGui::TextWrapped("Downloads use the curl program that comes with your system.");
            for (const Asset& a : plan_.assets) {
                ImGui::Spacing();
                ImGui::TextWrapped("%s", a.file_name.c_str());
                colored_wrapped(kMuted, a.url);
            }
            ImGui::Spacing();
            ImGui::TextWrapped("Every file is checked against a fixed SHA-256 before use. Choosing Skip downloads nothing.");
        }
    }

    void hd_work() {
        const HdSnapshot snap = hd_snapshot();
        heading(state_.hd_phase() == HdPhase::Failed ? "HD backgrounds: stopped" : "Creating HD backgrounds");
        if (state_.hd_phase() == HdPhase::Failed) {
            colored_wrapped(kBad, state_.hd_error());
            if (manual_download_) {
                ImGui::Spacing();
                ImGui::TextWrapped("You can download the files yourself with a web browser and put them in this folder, then press Try again:");
                const std::string folder = dw3::platform::path_to_utf8(downloads_dir(launch_.user_data));
                std::string folder_copy = folder;
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##folder", folder_copy.data(), folder_copy.size() + 1, ImGuiInputTextFlags_ReadOnly);
                for (const Asset& a : plan_.assets) colored_wrapped(kMuted, a.url);
            }
            return;
        }
        const char* phase_names[] = {"Preparing", "Downloading", "Unpacking", "Upscaling", "Stopped"};
        ImGui::Text("Step: %s", phase_names[static_cast<int>(state_.hd_phase())]);
        ImGui::TextWrapped("%s", snap.status.c_str());
        ImGui::Spacing();
        ImGui::ProgressBar(snap.fraction < 0 ? -1.0f * static_cast<float>(ImGui::GetTime()) : static_cast<float>(snap.fraction), ImVec2(-1, 0));
        ImGui::Spacing();
        colored_wrapped(kMuted, "Upscaling every field map takes a while. You can cancel; the next run continues where this one stopped.");
    }

    void done() {
        heading("All set");
        colored_wrapped(kGood, "Digimon World 3 is ready to play.");
        ImGui::Spacing();
        ImGui::TextWrapped("Disc: %s", state_.disc_path().c_str());
        ImGui::TextWrapped("HD backgrounds: %s", hd_done_ ? "installed and turned on" : "not set up");
        ImGui::Spacing();
        colored_wrapped(kMuted, "Video, audio and gameplay options are in the settings menu of the game. Run with --setup to change the disc or set up HD backgrounds later.");
    }

    void footer_buttons() {
        const ImGuiStyle& style = ImGui::GetStyle();
        std::vector<std::pair<std::string, int>> buttons;  // right to left: label, id
        auto width_of = [&](const char* label) { return ImGui::CalcTextSize(label).x + style.FramePadding.x * 2; };
        switch (state_.step()) {
            case Step::Welcome: buttons = {{"Next", 1}, {"Quit", 9}}; break;
            case Step::ChooseDisc: buttons = {{"Next", 1}, {"Back", 2}}; break;
            case Step::Verify: buttons = {{"Next", 1}, {"Back", 2}}; break;
            case Step::HdOffer: buttons = {{"Accept and download", 3}, {"Skip", 4}, {"Back", 2}}; break;
            case Step::HdWork:
                if (state_.hd_phase() == HdPhase::Failed) buttons = {{"Try again", 5}, {"Skip", 4}};
                else buttons = {{"Cancel", 6}};
                break;
            case Step::Done: buttons = {{"Finish", 7}}; break;
        }
        float total = 0;
        for (const auto& b : buttons) total += width_of(b.first.c_str()) + style.ItemSpacing.x;
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - style.WindowPadding.x - total + style.ItemSpacing.x);
        for (auto it = buttons.rbegin(); it != buttons.rend(); ++it) {
            const int id = it->second;
            const bool primary = (it == buttons.rend() - 1);
            const bool enabled = id == 1 ? state_.can_continue() : (id == 2 ? state_.can_go_back() : true);
            bool pressed;
            if (primary) {
                pressed = primary_button(it->first.c_str(), enabled);
            } else {
                ImGui::BeginDisabled(!enabled);
                pressed = ImGui::Button(it->first.c_str());
                ImGui::EndDisabled();
            }
            if (primary && enabled && focus_frames_ > 0) {
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                ImGui::SetNavCursorVisible(true);  // until the mouse moves
            }
            ImGui::SameLine();
            if (pressed) act(id);
        }
    }

    void act(int id) {
        switch (id) {
            case 1: state_.next(); break;
            case 2: state_.back(); break;
            case 3: begin_hd(); break;
            case 4: state_.skip_hd(); break;
            case 5: begin_hd(); break;
            case 6: request_stop(); break;
            case 7: state_.press_finish(); break;
            case 9: state_.quit(); break;
        }
    }

    void shortcuts() {
        if (ImGui::IsAnyItemActive()) return;  // typing in the path box
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
            if (state_.step() == Step::HdWork && state_.hd_phase() != HdPhase::Failed) request_stop();
            else state_.back();
        }
    }

    void open_dialog() {
        {
            std::lock_guard lock(dialog_.mutex);
            dialog_.failed = false;
        }
        static const SDL_DialogFileFilter filters[] = {{"Disc images", "cue;bin;iso"}, {"All files", "*"}};
        SDL_ShowOpenFileDialog(on_file_chosen, &dialog_, window_, filters, 2, nullptr, false);
    }

    // Per-frame housekeeping: the file picker's answer and the worker's progress.
    void pump() {
        {
            std::lock_guard lock(dialog_.mutex);
            if (dialog_.has_path) {
                demo_type_path(dialog_.path);
                dialog_.has_path = false;
            }
        }
        if (state_.step() != Step::HdWork) return;
        if (worker_done_ && worker_.joinable()) {
            worker_.join();
            if (hd_result_.ok) {
                hd_done_ = true;
                state_.finish_hd();
            } else {
                manual_download_ = hd_result_.manual_download;
                state_.fail_hd(hd_result_.error);
            }
        } else if (worker_.joinable()) {
            state_.set_hd_phase(hd_snapshot().phase);
        }
    }

    void begin_hd() {
        if (state_.step() == Step::HdOffer) state_.accept_hd();
        else state_.retry_hd();
        stop_worker();
        manual_download_ = false;
        worker_done_ = false;
        HdConfig config;
        config.user_data = launch_.user_data;
        config.disc = dw3::platform::path_from_utf8(state_.disc_path());
        config.plan = plan_;
        config.run = [](const std::vector<std::string>& command, const std::atomic<bool>& cancel) { return run_process(command, cancel); };
        config.generate = [disc = config.disc](const fs::path& upscaler, const fs::path& models, const fs::path& pack_dir,
                                               const std::function<void(std::size_t, std::size_t)>& progress,
                                               const std::atomic<bool>& cancel, std::string& error) {
            dw3::texrep::PackJob job;
            job.disc = disc;
            job.out = pack_dir;
            job.name = kPackName;
            job.upscaler = upscaler;
            job.models = models;
            job.model = kModelName;
            job.run = [&cancel](const std::vector<std::string>& command) { return run_process(command, cancel); };
            job.progress = progress;
            dw3::texrep::GenerateReport report;
            return dw3::texrep::run_pack_job(job, report, error);
        };
        installer_ = std::make_unique<HdInstaller>(std::move(config));
        worker_ = std::thread([this] {
            hd_result_ = installer_->run();
            worker_done_ = true;
        });
    }

    void stop_worker() {
        if (worker_.joinable()) {
            if (installer_) installer_->cancel();
            worker_.join();
        }
    }

    SDL_Window* window_;
    WizardLaunch launch_;
    WizardState state_;
    DownloadPlan plan_;
    char path_buffer_[1024]{};
    DialogResult dialog_;
    Step shown_step_ = Step::Welcome;
    bool step_changed_ = true;
    int focus_frames_ = 2;
    int verify_frames_ = 0;
    std::unique_ptr<HdInstaller> installer_;
    std::thread worker_;
    std::atomic<bool> worker_done_{false};
    HdResult hd_result_;
    bool manual_download_ = false;
    bool hd_done_ = false;
};

void save_frame(SDL_Renderer* renderer, const std::string& path) {
    SDL_Surface* surface = SDL_RenderReadPixels(renderer, nullptr);
    if (!surface) return;
    SDL_SaveBMP(surface, path.c_str());
    SDL_DestroySurface(surface);
}

// The unattended walk-through used to produce pictures of every step: a wrong file first, then the real disc, the HD
// offer, a failed HD attempt and the final page. Each page is shown for a few frames before it is captured.
class ScriptedRun {
public:
    ScriptedRun(std::string dir, std::string disc) : dir_(std::move(dir)), disc_(std::move(disc)) {}

    // Returns true when the run is over.
    bool advance(SetupUi& ui, SDL_Renderer* renderer) {
        if (++frames_ < 12) return false;
        const Step step = ui.state().step();
        auto shot = [&](const char* name) { save_frame(renderer, dir_ + "/" + std::to_string(++shots_) + "-" + name + ".bmp"); };
        switch (stage_) {
            case 0:
                shot("welcome");
                press_key(SDL_GetWindowID(SDL_GetRenderWindow(renderer)), SDLK_RETURN, SDL_SCANCODE_RETURN);  // the focused Next button, as a keyboard user would
                break;
            case 1:
                if (step == Step::Welcome) {
                    SDL_Log("setup: pressing Enter did not activate the focused button (nav active %d, id %u, keys %d)", ImGui::GetIO().NavActive, ImGui::GetCurrentContext()->NavId, ImGui::IsKeyDown(ImGuiKey_Enter));
                    ui.state().next();
                    return false;
                }
                shot("choose-disc-empty"); ui.demo_type_path(bogus_path()); ui.state().next(); break;
            case 2:
                if (ui.verify_waiting()) return false;
                shot("verify-wrong-file");
                ui.state().back();
                ui.demo_type_path(disc_);
                break;
            case 3: shot("choose-disc"); ui.state().next(); break;
            case 4:
                if (ui.verify_waiting()) return false;
                shot("verify-ok");
                ui.state().next();
                break;
            case 5: shot("hd-offer"); ui.demo_accept_hd(); break;
            case 6:
                if (step == Step::HdWork && ui.state().hd_phase() != HdPhase::Failed) return false;
                shot("hd-failed");
                ui.state().skip_hd();
                break;
            case 7: shot("done"); return true;
            default: return true;
        }
        ++stage_;
        frames_ = 0;
        return false;
    }

private:
    static void press_key(SDL_WindowID window_id, SDL_Keycode key, SDL_Scancode scancode) {
        for (bool down : {true, false}) {
            SDL_Event e{};
            e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            e.key.windowID = window_id;
            e.key.key = key;
            e.key.scancode = scancode;
            e.key.down = down;
            SDL_PushEvent(&e);
        }
    }
    static std::string bogus_path() {
        return dw3::platform::path_to_utf8(fs::temp_directory_path() / "not-a-disc.cue");
    }
    std::string dir_, disc_;
    int stage_ = 0;
    int frames_ = 0;
    int shots_ = 0;
};

}  // namespace

WizardOutcome run_setup_wizard(const WizardLaunch& launch) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
        SDL_Log("setup: cannot start SDL: %s", SDL_GetError());
        return {};
    }
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("Digimon World 3 Recomp - Setup", 960, 660, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &window,
                                     &renderer)) {
        SDL_Log("setup: cannot open a window: %s", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD);
        return {};
    }
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetWindowMinimumSize(window, 720, 520);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<uint8_t*>(dw3::wizard::roboto_medium), static_cast<int>(dw3::wizard::roboto_medium_size), 20.0f,
                                   &font_config);
    apply_theme();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    WizardOutcome outcome;
    {
        SetupUi ui(window, launch);
        std::unique_ptr<ScriptedRun> script;
        if (!launch.screenshot_dir.empty()) script = std::make_unique<ScriptedRun>(launch.screenshot_dir, launch.initial_disc);
        bool running = true;
        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) ui.state().quit();
            }
            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            ui.frame(io.DisplaySize.x, io.DisplaySize.y);
            ImGui::Render();
            SDL_SetRenderDrawColor(renderer, 19, 21, 26, 255);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
            if (script && script->advance(ui, renderer)) running = false;
            SDL_RenderPresent(renderer);
            if (ui.state().finished() || ui.state().quit_requested()) running = false;
            SDL_Delay(8);
        }
        outcome = ui.state().outcome();
        std::error_code ec;
        const fs::path absolute = fs::absolute(dw3::platform::path_from_utf8(outcome.disc_path), ec);  // saved paths must not depend on the working folder
        if (!ec && !outcome.disc_path.empty()) outcome.disc_path = dw3::platform::path_to_utf8(absolute);
        if (script) outcome.completed = false;  // a scripted run configures nothing
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD);
    return outcome;
}

}  // namespace dw3setup
