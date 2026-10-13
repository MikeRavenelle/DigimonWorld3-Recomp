#include <gtest/gtest.h>

#include "capture.h"

using namespace dw3app::input;

TEST(Capture, IdleUntilBegun) {
    BindingCapture c;
    EXPECT_FALSE(c.active());
    EXPECT_FALSE(c.key_pressed(default_bindings(), "Q"));
    EXPECT_FALSE(c.pad_pressed(default_bindings(), PadInput::North));
}

TEST(Capture, TheNextKeyIsBound) {
    BindingCapture c;
    c.begin(Action::Square);
    EXPECT_EQ(c.active(), Action::Square);
    const auto bound = c.key_pressed(default_bindings(), "Q");
    ASSERT_TRUE(bound);
    EXPECT_EQ((*bound)[index_of(Action::Square)].key, "Q");
    EXPECT_EQ((*bound)[index_of(Action::L1)].key, "");  // taken from its old owner
}

TEST(Capture, TheNextButtonIsBound) {
    BindingCapture c;
    c.begin(Action::Square);
    const auto bound = c.pad_pressed(default_bindings(), PadInput::LeftShoulder);
    ASSERT_TRUE(bound);
    EXPECT_EQ((*bound)[index_of(Action::Square)].pad, PadInput::LeftShoulder);
    EXPECT_EQ((*bound)[index_of(Action::Square)].key, "A");  // the key half is kept
}

TEST(Capture, OnlyTheFirstPressCounts) {
    BindingCapture c;
    c.begin(Action::Square);
    ASSERT_TRUE(c.key_pressed(default_bindings(), "Q"));
    EXPECT_FALSE(c.waiting());
    EXPECT_FALSE(c.key_pressed(default_bindings(), "W"));
    EXPECT_FALSE(c.pad_pressed(default_bindings(), PadInput::South));
}

TEST(Capture, EscapeCancelsWithoutBinding) {
    BindingCapture c;
    c.begin(Action::Square);
    EXPECT_FALSE(c.key_pressed(default_bindings(), "Escape"));
    EXPECT_FALSE(c.waiting());
    EXPECT_TRUE(c.active());  // until the key is let go
}

TEST(Capture, AReservedKeyOfAnotherActionIsIgnoredAndItKeepsWaiting) {
    BindingCapture c;
    c.begin(Action::FastForward);
    EXPECT_FALSE(c.key_pressed(default_bindings(), "Return"));
    EXPECT_TRUE(c.waiting());
    EXPECT_TRUE(c.key_pressed(default_bindings(), "Tab"));
}

TEST(Capture, AReservedKeyMayBeBoundToItsOwnAction) {
    BindingCapture c;
    c.begin(Action::Start);
    const auto bound = c.key_pressed(with_key(default_bindings(), Action::Start, ""), "Return");
    ASSERT_TRUE(bound);
    EXPECT_EQ((*bound)[index_of(Action::Start)].key, "Return");
}

TEST(Capture, EmptyOrUnbindableInputIsIgnored) {
    BindingCapture c;
    c.begin(Action::Square);
    EXPECT_FALSE(c.key_pressed(default_bindings(), ""));
    EXPECT_FALSE(c.pad_pressed(default_bindings(), PadInput::None));
    EXPECT_TRUE(c.waiting());
}

TEST(Capture, StaysActiveUntilEverythingIsReleased) {
    BindingCapture c;
    c.begin(Action::Square);
    ASSERT_TRUE(c.key_pressed(default_bindings(), "Q"));
    c.tick(true);
    EXPECT_TRUE(c.active());
    c.tick(false);
    EXPECT_FALSE(c.active());
}

TEST(Capture, WaitingEndsAfterTheTimeoutWhateverIsHeld) {
    BindingCapture c;
    c.begin(Action::Square);
    for (int i = 0; i < BindingCapture::kTimeoutFrames - 1; ++i) {
        c.tick(false);
        ASSERT_TRUE(c.waiting()) << i;
    }
    c.tick(false);
    EXPECT_FALSE(c.active());
}

TEST(Capture, AHeldKeyCannotKeepItActiveForever) {
    BindingCapture c;
    c.begin(Action::Square);
    ASSERT_TRUE(c.key_pressed(default_bindings(), "Q"));
    for (int i = 0; i < BindingCapture::kTimeoutFrames; ++i) c.tick(true);
    EXPECT_FALSE(c.active());
}

TEST(Capture, CanBeginAgainAfterwards) {
    BindingCapture c;
    c.begin(Action::Square);
    c.key_pressed(default_bindings(), "Q");
    c.tick(false);
    c.begin(Action::Triangle);
    EXPECT_TRUE(c.waiting());
    EXPECT_EQ(c.active(), Action::Triangle);
}
