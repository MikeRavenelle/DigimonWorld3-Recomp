#include "wizard_state.h"

namespace dw3setup {

void WizardState::set_disc_path(std::string path) {
    if (path == disc_path_) return;
    disc_path_ = std::move(path);
    check_ = {};
    check_done_ = false;
}

void WizardState::finish_verify(const DiscCheck& result) {
    if (step_ != Step::Verify) return;
    check_ = result;
    check_done_ = true;
}

bool WizardState::can_continue() const {
    switch (step_) {
        case Step::Welcome: return true;
        case Step::ChooseDisc: return !disc_path_.empty();
        case Step::Verify: return check_done_ && check_.ok();
        case Step::HdOffer:
        case Step::HdWork:
        case Step::Done: return false;  // these have their own buttons
    }
    return false;
}

bool WizardState::can_go_back() const {
    switch (step_) {
        case Step::ChooseDisc: return true;
        case Step::Verify: return check_done_;
        case Step::HdOffer: return true;
        default: return false;
    }
}

void WizardState::next() {
    if (!can_continue()) return;
    switch (step_) {
        case Step::Welcome: step_ = Step::ChooseDisc; break;
        case Step::ChooseDisc:
            check_done_ = false;
            step_ = Step::Verify;
            break;
        case Step::Verify: step_ = Step::HdOffer; break;
        default: break;
    }
}

void WizardState::back() {
    if (!can_go_back()) return;
    switch (step_) {
        case Step::ChooseDisc: step_ = Step::Welcome; break;
        case Step::Verify:
        case Step::HdOffer: step_ = Step::ChooseDisc; break;
        default: break;
    }
}

void WizardState::accept_hd() {
    if (step_ != Step::HdOffer) return;
    step_ = Step::HdWork;
    hd_phase_ = HdPhase::Preparing;
    hd_error_.clear();
}

void WizardState::skip_hd() {
    const bool failed_work = step_ == Step::HdWork && hd_phase_ == HdPhase::Failed;
    if (step_ != Step::HdOffer && !failed_work) return;
    hd_installed_ = false;
    step_ = Step::Done;
}

void WizardState::set_hd_phase(HdPhase phase) {
    if (step_ != Step::HdWork || phase == HdPhase::Failed) return;
    hd_phase_ = phase;
}

void WizardState::fail_hd(std::string message) {
    if (step_ != Step::HdWork) return;
    hd_phase_ = HdPhase::Failed;
    hd_error_ = std::move(message);
}

void WizardState::retry_hd() {
    if (step_ != Step::HdWork || hd_phase_ != HdPhase::Failed) return;
    hd_phase_ = HdPhase::Preparing;
    hd_error_.clear();
}

void WizardState::finish_hd() {
    if (step_ != Step::HdWork || hd_phase_ == HdPhase::Failed) return;
    hd_installed_ = true;
    step_ = Step::Done;
}

void WizardState::press_finish() {
    if (step_ == Step::Done) finish_pressed_ = true;
}

WizardOutcome WizardState::outcome() const {
    WizardOutcome o;
    o.completed = finished();
    o.disc_path = disc_path_;
    o.hd_backgrounds = hd_installed_;
    return o;
}

}  // namespace dw3setup
