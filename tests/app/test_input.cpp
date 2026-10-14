#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "analog.h"
#include "bindings.h"
#include "glyphs.h"

using namespace dw3app::input;

namespace {

bool key_set(const std::set<std::string>& held, std::string_view key) { return held.count(std::string(key)) > 0; }

uint16_t buttons(const Bindings& b, std::set<std::string> keys = {}, std::set<PadInput> pad = {}) {
    return console_buttons(
        b, [&](std::string_view k) { return key_set(keys, k); }, [&](PadInput p) { return pad.count(p) > 0; });
}

}  // namespace

// ---- Binding text ----

TEST(Bindings, ParsesKeyAndButton) {
    const auto b = parse_binding("Z|South");
    ASSERT_TRUE(b);
    EXPECT_EQ(b->key, "Z");
    EXPECT_EQ(b->pad, PadInput::South);
}

TEST(Bindings, EitherHalfMayBeEmpty) {
    EXPECT_EQ(*parse_binding("F9|"), (Binding{"F9", PadInput::None}));
    EXPECT_EQ(*parse_binding("|LeftStick"), (Binding{"", PadInput::LeftStick}));
    EXPECT_EQ(*parse_binding("|"), Binding{});
}

TEST(Bindings, ButtonNamesIgnoreCaseAndSpaces) {
    EXPECT_EQ(parse_binding("Tab | rightstick ")->pad, PadInput::RightStick);
    EXPECT_EQ(parse_binding(" Left Shift |DPADUP")->key, "Left Shift");
}

TEST(Bindings, RejectsTextThatIsNotABinding) {
    EXPECT_FALSE(parse_binding("Z"));
    EXPECT_FALSE(parse_binding("Z|Cross"));
    EXPECT_FALSE(parse_binding(""));
}

TEST(Bindings, FormatRoundTrips) {
    for (std::size_t i = 0; i < kActionCount; ++i) {
        const Binding b = default_binding(action_at(i));
        EXPECT_EQ(*parse_binding(format_binding(b)), b) << action_id(action_at(i));
    }
    EXPECT_EQ(format_binding({"Left Shift", PadInput::DPadUp}), "Left Shift|DPadUp");
}

TEST(Bindings, EveryPadInputHasANameThatParsesBack) {
    for (int i = 1; i < static_cast<int>(PadInput::Count); ++i) {
        const auto p = static_cast<PadInput>(i);
        EXPECT_EQ(pad_input_from_name(pad_input_name(p)), p);
    }
}

TEST(Bindings, ActionIdsAreUniqueSettingsKeys) {
    std::set<std::string_view> ids;
    for (std::size_t i = 0; i < kActionCount; ++i) EXPECT_TRUE(ids.insert(action_id(action_at(i))).second);
}

TEST(Bindings, DefaultsKeepTheOriginalLayout) {
    const Bindings b = default_bindings();
    EXPECT_EQ(b[index_of(Action::Cross)], (Binding{"Z", PadInput::South}));
    EXPECT_EQ(b[index_of(Action::Circle)], (Binding{"X", PadInput::East}));
    EXPECT_EQ(b[index_of(Action::Start)], (Binding{"Return", PadInput::Start}));
    EXPECT_EQ(b[index_of(Action::Select)], (Binding{"Backspace", PadInput::Back}));
    EXPECT_EQ(b[index_of(Action::L2)].pad, PadInput::LeftTrigger);
    EXPECT_EQ(b[index_of(Action::FastForward)], (Binding{"Tab", PadInput::RightStick}));
    EXPECT_EQ(b[index_of(Action::FastForwardLatch)], (Binding{"F10", PadInput::LeftStick}));
    EXPECT_EQ(b[index_of(Action::GameSpeed)], (Binding{"F4", PadInput::Guide}));
    EXPECT_EQ(b[index_of(Action::Compare)], (Binding{"F9", PadInput::None}));
}

TEST(Bindings, UnreadableStoredTextFallsBackToTheDefault) {
    auto text = default_binding_texts();
    text[index_of(Action::Cross)] = "garbage";
    text[index_of(Action::Circle)] = "Q|North";
    const Bindings b = bindings_from_text(text);
    EXPECT_EQ(b[index_of(Action::Cross)], default_binding(Action::Cross));
    EXPECT_EQ(b[index_of(Action::Circle)], (Binding{"Q", PadInput::North}));
}

TEST(Bindings, EachConsoleButtonHasItsOwnBit) {
    uint16_t seen = 0;
    for (std::size_t i = 0; i < kActionCount; ++i) {
        const Action a = action_at(i);
        if (!is_console_button(a)) {
            EXPECT_EQ(console_bit(a), 0) << action_id(a);
            continue;
        }
        EXPECT_NE(console_bit(a), 0) << action_id(a);
        EXPECT_EQ(seen & console_bit(a), 0) << action_id(a);
        seen |= console_bit(a);
    }
    EXPECT_EQ(seen, 0xFFFF);
}

// ---- Rebinding ----

TEST(Rebinding, AssigningAKeyTakesItFromTheOldOwner) {
    const Bindings b = with_key(default_bindings(), Action::Cross, "X");
    EXPECT_EQ(b[index_of(Action::Cross)].key, "X");
    EXPECT_EQ(b[index_of(Action::Circle)].key, "");
    EXPECT_EQ(b[index_of(Action::Circle)].pad, PadInput::East);  // its button is untouched
}

TEST(Rebinding, AssigningAButtonTakesItFromTheOldOwner) {
    const Bindings b = with_pad(default_bindings(), Action::Square, PadInput::South);
    EXPECT_EQ(b[index_of(Action::Square)].pad, PadInput::South);
    EXPECT_EQ(b[index_of(Action::Cross)].pad, PadInput::None);
}

TEST(Rebinding, KeyMatchingIgnoresCase) {
    const Bindings b = with_key(default_bindings(), Action::L3, "z");
    EXPECT_EQ(b[index_of(Action::Cross)].key, "");
}

TEST(Rebinding, ReservedKeysCannotMoveToAnotherAction) {
    const Bindings before = default_bindings();
    EXPECT_EQ(with_key(before, Action::FastForward, "Return"), before);
    EXPECT_EQ(with_key(before, Action::Cross, "Escape"), before);
    EXPECT_FALSE(key_assignable(Action::Square, "Space"));
    EXPECT_TRUE(key_assignable(Action::Cross, "Space"));  // its own
    EXPECT_TRUE(key_assignable(Action::Square, "Q"));
}

TEST(Rebinding, ReservedKeysCoverMenuAccess) {
    EXPECT_EQ(reserved_key_action("Return"), Action::Start);
    EXPECT_EQ(reserved_key_action("backspace"), Action::Select);
    EXPECT_EQ(reserved_key_action("Space"), Action::Cross);
    EXPECT_EQ(reserved_key_action("Escape"), Action::Circle);
    for (const char* arrow : {"Up", "Down", "Left", "Right"}) EXPECT_TRUE(reserved_key_action(arrow)) << arrow;
    EXPECT_FALSE(reserved_key_action("Z"));
}

TEST(Rebinding, MenuStaysReachableWithEverythingUnbound) {
    Bindings none;
    EXPECT_EQ(buttons(none, {"Return"}), console_bit(Action::Start));
    EXPECT_EQ(buttons(none, {"Backspace"}), console_bit(Action::Select));
    EXPECT_EQ(buttons(none, {"Space"}), console_bit(Action::Cross));
    EXPECT_EQ(buttons(none, {"Escape"}), console_bit(Action::Circle));
    EXPECT_EQ(buttons(none, {"Up", "Left"}), console_bit(Action::Up) | console_bit(Action::Left));
}

TEST(Rebinding, ConsoleButtonsFollowTheBindings) {
    Bindings b = with_key(default_bindings(), Action::Cross, "C");
    EXPECT_EQ(buttons(b, {"C"}), console_bit(Action::Cross));
    EXPECT_EQ(buttons(b, {"Z"}), 0);
    EXPECT_EQ(buttons(b, {}, {PadInput::South}), console_bit(Action::Cross));
    EXPECT_EQ(buttons(b, {"C"}, {PadInput::East}), console_bit(Action::Cross) | console_bit(Action::Circle));
}

TEST(Rebinding, HotkeysAreNotConsoleButtons) {
    EXPECT_EQ(buttons(default_bindings(), {"Tab", "F4", "F9"}, {PadInput::RightStick, PadInput::Guide}), 0);
}

TEST(Rebinding, ShoulderAndTriggerButtonsReachTheConsole) {
    EXPECT_EQ(buttons(default_bindings(), {}, {PadInput::LeftTrigger, PadInput::RightShoulder}),
              console_bit(Action::L2) | console_bit(Action::R1));
}

// ---- Left stick ----

TEST(Stick, CentreAndSmallLeansDoNothing) {
    EXPECT_EQ(stick_dpad_bits(0, 0, 25), 0);
    EXPECT_EQ(stick_dpad_bits(5000, -5000, 25), 0);  // 21% from centre
    EXPECT_EQ(stick_dpad_bits(8000, 0, 25), 0);      // 24%
}

TEST(Stick, PastTheDeadzoneItPressesTheAxis) {
    EXPECT_EQ(stick_dpad_bits(9000, 0, 25), dw3::kPadRight);  // 27%
    EXPECT_EQ(stick_dpad_bits(-32768, 0, 25), dw3::kPadLeft);
    EXPECT_EQ(stick_dpad_bits(0, -20000, 25), dw3::kPadUp);
    EXPECT_EQ(stick_dpad_bits(0, 32767, 25), dw3::kPadDown);  // SDL's y grows downward
}

TEST(Stick, DeadzoneSizeIsTheSetting) {
    EXPECT_EQ(stick_dpad_bits(9000, 0, 10), dw3::kPadRight);
    EXPECT_EQ(stick_dpad_bits(9000, 0, 30), 0);
    EXPECT_EQ(stick_dpad_bits(16000, 0, 50), 0);  // 49%
    EXPECT_EQ(stick_dpad_bits(17000, 0, 50), dw3::kPadRight);
}

TEST(Stick, DiagonalsPressBothAxes) {
    EXPECT_EQ(stick_dpad_bits(23000, 23000, 25), dw3::kPadRight | dw3::kPadDown);
    EXPECT_EQ(stick_dpad_bits(-23000, -23000, 25), dw3::kPadLeft | dw3::kPadUp);
}

TEST(Stick, ANearlyHorizontalLeanIsNotADiagonal) {
    EXPECT_EQ(stick_dpad_bits(30000, 6000, 25), dw3::kPadRight);   // 11 degrees off the axis
    EXPECT_EQ(stick_dpad_bits(30000, 14000, 25), dw3::kPadRight | dw3::kPadDown);  // 25 degrees
}

TEST(Stick, TheDeadzoneIsRoundNotSquare) {
    // Both axes are inside 25% but the stick is 28% from centre: it moves.
    EXPECT_NE(stick_dpad_bits(6600, 6600, 25), 0);
    EXPECT_EQ(stick_dpad_bits(5500, 5500, 25), 0);
}

TEST(Stick, TriggersCountPastAQuarterTravel) {
    EXPECT_FALSE(trigger_pressed(0));
    EXPECT_FALSE(trigger_pressed(8000));
    EXPECT_TRUE(trigger_pressed(9000));
}

// ---- Which pad drives port 1 ----

TEST(PadChoice, NothingConnectedChoosesNothing) {
    EXPECT_FALSE(choose_pad<int>({}, 0, std::nullopt));
    EXPECT_FALSE(choose_pad<int>({}, 2, 7));
}

TEST(PadChoice, AutoTakesTheFirstPad) {
    const std::vector<int> pads{10, 11};
    EXPECT_EQ(choose_pad<int>(pads, 0, std::nullopt), 10);
}

TEST(PadChoice, AutoKeepsThePadInUseWhenAnotherAppears) {
    const std::vector<int> pads{10, 11};
    EXPECT_EQ(choose_pad<int>(pads, 0, 11), 11);
}

TEST(PadChoice, AutoFallsBackToTheNextPadWhenTheCurrentOneIsRemoved) {
    const std::vector<int> pads{10};
    EXPECT_EQ(choose_pad<int>(pads, 0, 11), 10);
}

TEST(PadChoice, APositionPicksThatPad) {
    const std::vector<int> pads{10, 11, 12};
    EXPECT_EQ(choose_pad<int>(pads, 1, 12), 10);
    EXPECT_EQ(choose_pad<int>(pads, 3, 10), 12);
}

TEST(PadChoice, APositionBeyondTheConnectedPadsActsAsAuto) {
    const std::vector<int> pads{10, 11};
    EXPECT_EQ(choose_pad<int>(pads, 3, std::nullopt), 10);
    EXPECT_EQ(choose_pad<int>(pads, 4, 11), 11);
}

TEST(PadChoice, ReplugRestoresTheChosenPosition) {
    const std::vector<int> only_first{10};
    EXPECT_EQ(choose_pad<int>(only_first, 2, std::nullopt), 10);
    const std::vector<int> both{10, 11};
    EXPECT_EQ(choose_pad<int>(both, 2, 10), 11);
}

// ---- Button prompts ----

TEST(Glyphs, AutoFollowsTheConnectedPad) {
    EXPECT_EQ(resolve_style(PromptStyle::Auto, PadFamily::PlayStation), PromptStyle::PlayStation);
    EXPECT_EQ(resolve_style(PromptStyle::Auto, PadFamily::Nintendo), PromptStyle::Nintendo);
    EXPECT_EQ(resolve_style(PromptStyle::Auto, PadFamily::Xbox), PromptStyle::Xbox);
    EXPECT_EQ(resolve_style(PromptStyle::Auto, PadFamily::Unknown), PromptStyle::Xbox);
}

TEST(Glyphs, AChoiceOverridesThePad) {
    EXPECT_EQ(resolve_style(PromptStyle::Nintendo, PadFamily::PlayStation), PromptStyle::Nintendo);
    EXPECT_EQ(resolve_style(PromptStyle::PlayStation, PadFamily::Unknown), PromptStyle::PlayStation);
}

TEST(Glyphs, FaceButtonsAreNamedByStyle) {
    EXPECT_EQ(pad_label(PadInput::South, PromptStyle::Xbox), "A");
    EXPECT_EQ(pad_label(PadInput::South, PromptStyle::PlayStation), "Cross");
    EXPECT_EQ(pad_label(PadInput::South, PromptStyle::Nintendo), "B");
    EXPECT_EQ(pad_label(PadInput::East, PromptStyle::Nintendo), "A");
    EXPECT_EQ(pad_label(PadInput::North, PromptStyle::Xbox), "Y");
    EXPECT_EQ(pad_label(PadInput::Back, PromptStyle::Xbox), "View");
    EXPECT_EQ(pad_label(PadInput::RightShoulder, PromptStyle::PlayStation), "R1");
    EXPECT_EQ(pad_label(PadInput::LeftTrigger, PromptStyle::Nintendo), "ZL");
}

TEST(Glyphs, EveryLabelFitsAMenuRow) {
    for (const PromptStyle style : {PromptStyle::Xbox, PromptStyle::PlayStation, PromptStyle::Nintendo})
        for (int i = 0; i < static_cast<int>(PadInput::Count); ++i)
            EXPECT_LE(pad_label(static_cast<PadInput>(i), style).size(), 5u);
}

TEST(Glyphs, KeyNamesAreShortened) {
    EXPECT_EQ(key_label("Backspace"), "Bksp");
    EXPECT_EQ(key_label("Left Shift"), "LShift");
    EXPECT_EQ(key_label("Z"), "Z");
    EXPECT_EQ(key_label("Keypad Multiply"), "Keypad");
}

TEST(Glyphs, BindingLabelShowsKeyAndButton) {
    EXPECT_EQ(binding_label({"Return", PadInput::Start}, PromptStyle::Xbox), "Enter/Menu");
    EXPECT_EQ(binding_label({"Z", PadInput::South}, PromptStyle::PlayStation), "Z/Cross");
    EXPECT_EQ(binding_label({"Z", PadInput::None}, PromptStyle::Xbox), "Z");
    EXPECT_EQ(binding_label({"", PadInput::East}, PromptStyle::Nintendo), "A");
    EXPECT_EQ(binding_label({}, PromptStyle::Xbox), "None");
}

TEST(Glyphs, SettingsHintNamesTheButtonOnAPad) {
    const Binding select = default_binding(Action::Select);
    EXPECT_EQ(select_hint(select, PromptStyle::Xbox, true), "View");
    EXPECT_EQ(select_hint(select, PromptStyle::Nintendo, true), "Minus");
    EXPECT_EQ(select_hint(select, PromptStyle::Xbox, false), "SELECT");
    EXPECT_EQ(select_hint({"Backspace", PadInput::None}, PromptStyle::Xbox, true), "SELECT");
}

TEST(Glyphs, CloseHintNamesThePadButtonOutsidePlayStationStyle) {
    const Binding close{"S", PadInput::North};
    EXPECT_EQ(close_hint(close, PromptStyle::Xbox, true), "Y");
    EXPECT_EQ(close_hint(close, PromptStyle::Nintendo, true), "X");
    EXPECT_EQ(close_hint(close, PromptStyle::PlayStation, true), "");
    EXPECT_EQ(close_hint(close, PromptStyle::Xbox, false), "");
    EXPECT_EQ(close_hint({"S", PadInput::None}, PromptStyle::Xbox, true), "");
}
