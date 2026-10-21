// The wizard's flow as a state machine, free of UI, files and processes: the UI reports what happened (the disc was
// checked, a download finished) and asks what it may do (can the player continue, go back).
#pragma once
#include <string>

#include "disc_check.h"

namespace dw3setup {

enum class Step { Welcome, ChooseDisc, Verify, PrepareWork, HdOffer, HdWork, Done };
enum class HdPhase { Preparing, Downloading, Unpacking, Generating, Failed };

struct WizardOutcome {
    bool completed = false;  // false when the window was closed before the end
    std::string disc_path;
    bool hd_backgrounds = false;
    bool prepared = false;  // the game module was built by this run
};

class WizardState {
public:
    Step step() const { return step_; }

    // Disc
    const std::string& disc_path() const { return disc_path_; }
    void set_disc_path(std::string path);  // changing the path discards an earlier check
    bool verify_pending() const { return step_ == Step::Verify && !check_done_; }
    void finish_verify(const DiscCheck& result);
    const DiscCheck& check() const { return check_; }

    // Navigation
    bool can_continue() const;
    bool can_go_back() const;
    void next();
    void back();

    // Preparing the game from the disc (only when the program has no game code of its own): after the disc is verified
    // the work starts by itself; it ends in HdOffer, or in a failure that can be retried.
    void require_prepare(bool required) { prepare_required_ = required; }
    bool prepare_required() const { return prepare_required_; }
    void resume_at_prepare(std::string verified_disc);  // the disc was checked already: start with the work
    bool prepare_failed() const { return prepare_failed_; }
    const std::string& prepare_error() const { return prepare_error_; }
    void fail_prepare(std::string message);  // PrepareWork only
    void retry_prepare();                    // a failed PrepareWork starts over
    void finish_prepare();                   // PrepareWork -> HdOffer

    // Optional HD backgrounds
    void accept_hd();  // HdOffer -> HdWork
    void skip_hd();    // HdOffer or a failed HdWork -> Done without HD backgrounds
    HdPhase hd_phase() const { return hd_phase_; }
    const std::string& hd_error() const { return hd_error_; }
    void set_hd_phase(HdPhase phase);        // HdWork only; not Failed (use fail_hd)
    void fail_hd(std::string message);       // HdWork only
    void retry_hd();                         // a failed HdWork starts over
    void finish_hd();                        // HdWork -> Done with HD backgrounds

    // End
    void quit() { quit_ = true; }
    bool quit_requested() const { return quit_; }
    bool finished() const { return step_ == Step::Done && finish_pressed_; }
    void press_finish();  // Done only
    WizardOutcome outcome() const;

private:
    Step step_ = Step::Welcome;
    std::string disc_path_;
    DiscCheck check_;
    bool check_done_ = false;
    HdPhase hd_phase_ = HdPhase::Preparing;
    std::string hd_error_;
    bool hd_installed_ = false;
    bool prepare_required_ = false;
    bool prepare_failed_ = false;
    bool prepared_ = false;
    std::string prepare_error_;
    bool finish_pressed_ = false;
    bool quit_ = false;
};

}  // namespace dw3setup
