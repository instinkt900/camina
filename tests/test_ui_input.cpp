// M10.5 tests for the SDL3 to moth_ui input bridge.
//
// The bridge names no device and opens no window, so every case here drives it
// with frames written by hand. That is the whole reason `platform::InputFrame`
// is a plain struct: `DESIGN.md` section 9 asks for it so a replay can write one
// out, and a test is the first thing that reads one back.
//
// The listener is a recorder rather than a real layout. Building one needs a
// moth_ui::Context, which needs a renderer and both factories, and none of that
// belongs in a test with no GPU. What matters here is which events came out,
// in which order, and what happened to the frame when one was consumed.

#include "check.h"
#include "platform/input.h"
#include "ui/font_factory.h"
#include "ui/image.h"
#include "ui/input_bridge.h"
#include "ui/renderer.h"

#include <moth_ui/context.h>
#include <moth_ui/events/event.h>
#include <moth_ui/events/event_key.h>
#include <moth_ui/events/event_mouse.h>
#include <moth_ui/nodes/group.h>
#include <moth_ui/nodes/node.h>

#include <cstddef>
#include <memory>
#include <vector>

namespace {

    namespace pf = engine::platform;
    using engine::ui::InputBridge;

    /// A listener that keeps every event and consumes what it is told to.
    class Recorder : public moth_ui::IEventListener {
    public:
        bool OnEvent(const moth_ui::Event& event) override {
            events.push_back(event.Clone());
            return consume;
        }

        /// What OnEvent returns. The UI decides this in a real run.
        bool consume = false;
        std::vector<std::unique_ptr<moth_ui::Event>> events;

        [[nodiscard]] std::size_t count() const { return events.size(); }

        /// The event at an index, as a type, or null when it is another type.
        template <typename T>
        [[nodiscard]] const T* at(std::size_t index) const {
            if (index >= events.size()) {
                return nullptr;
            }
            return moth_ui::event_cast<T>(*events.at(index));
        }

        void clear() { events.clear(); }
    };

    /// A frame with the named key down and nothing else.
    [[nodiscard]] pf::InputFrame with_key(pf::Key key) {
        pf::InputFrame frame;
        frame.keys.at(static_cast<std::size_t>(key)) = true;
        return frame;
    }

    /// A frame with the named button down and nothing else.
    [[nodiscard]] pf::InputFrame with_button(pf::MouseButton button) {
        pf::InputFrame frame;
        frame.mouse_buttons.at(static_cast<std::size_t>(button)) = true;
        return frame;
    }

    [[nodiscard]] bool key_down(const pf::InputFrame& frame, pf::Key key) {
        return frame.keys.at(static_cast<std::size_t>(key));
    }

    [[nodiscard]] bool button_down(const pf::InputFrame& frame, pf::MouseButton button) {
        return frame.mouse_buttons.at(static_cast<std::size_t>(button));
    }

    void an_unchanged_frame_sends_nothing() {
        test::section("an unchanged frame sends nothing");

        InputBridge bridge;
        Recorder recorder;

        // This is the offscreen case as well as the idle one. An offscreen run
        // has no devices and feeds a default frame every time, so the bridge
        // must send nothing at all or a capture stops being reproducible.
        pf::InputFrame frame;
        (void)bridge.take(frame, &recorder);
        (void)bridge.take(frame, &recorder);
        (void)bridge.take(frame, &recorder);

        test::check(recorder.count() == 0, "three default frames send no event");
    }

    void a_key_reports_both_edges_and_not_the_hold() {
        test::section("a key reports both edges and not the hold");

        InputBridge bridge;
        Recorder recorder;

        pf::InputFrame down = with_key(pf::Key::W);
        (void)bridge.take(down, &recorder);
        test::check(recorder.count() == 1, "the press sends one event");
        const auto* pressed = recorder.at<moth_ui::EventKey>(0);
        test::check(pressed != nullptr, "the press is a key event");
        if (pressed != nullptr) {
            test::check(pressed->GetAction() == moth_ui::KeyAction::Down, "it is a down");
            test::check(pressed->GetKey() == moth_ui::Key::W, "it names the W key");
        }

        recorder.clear();
        (void)bridge.take(down, &recorder);
        test::check(recorder.count() == 0, "holding the key sends nothing more");

        pf::InputFrame up;
        (void)bridge.take(up, &recorder);
        test::check(recorder.count() == 1, "the release sends one event");
        const auto* released = recorder.at<moth_ui::EventKey>(0);
        test::check(released != nullptr, "the release is a key event");
        if (released != nullptr) {
            test::check(released->GetAction() == moth_ui::KeyAction::Up, "it is an up");
        }
    }

    void a_key_event_carries_the_modifiers_of_its_frame() {
        test::section("a key event carries the modifiers of its frame");

        InputBridge bridge;
        Recorder recorder;

        pf::InputFrame frame = with_key(pf::Key::S);
        frame.keys.at(static_cast<std::size_t>(pf::Key::LeftControl)) = true;
        frame.keys.at(static_cast<std::size_t>(pf::Key::RightShift)) = true;

        (void)bridge.take(frame, &recorder);

        // Three keys went down on the one frame, so three events came out. Each
        // has to carry the modifiers, including the ones for the modifier keys
        // themselves, because moth_ui reads the mask off the event and never
        // asks a device.
        test::check(recorder.count() == 3, "three keys down send three events");
        bool every_event_has_the_mods = recorder.count() == 3;
        for (std::size_t i = 0; i < recorder.count(); ++i) {
            const auto* event = recorder.at<moth_ui::EventKey>(i);
            if (event == nullptr ||
                (event->GetMods() & moth_ui::KeyMod_LeftCtrl) == 0 ||
                (event->GetMods() & moth_ui::KeyMod_RightShift) == 0) {
                every_event_has_the_mods = false;
            }
        }
        test::check(every_event_has_the_mods, "every event names control and shift");

        const auto* event = recorder.at<moth_ui::EventKey>(0);
        test::check(event != nullptr && (event->GetMods() & moth_ui::KeyMod_LeftAlt) == 0,
                    "and it does not name a modifier nobody held");
    }

    void the_pointer_reports_where_it_is_and_how_far_it_moved() {
        test::section("the pointer reports where it is and how far it moved");

        InputBridge bridge;
        Recorder recorder;

        pf::InputFrame frame;
        frame.mouse_position = engine::Vec2{ 320.0F, 240.0F };
        frame.mouse_delta = engine::Vec2{ 12.0F, -4.0F };
        (void)bridge.take(frame, &recorder);

        test::check(recorder.count() == 1, "a move sends one event");
        const auto* move = recorder.at<moth_ui::EventMouseMove>(0);
        test::check(move != nullptr, "it is a move event");
        if (move != nullptr) {
            test::check(move->GetPosition().x == 320 && move->GetPosition().y == 240,
                        "it names the position in window pixels");
            test::check(move->GetDelta().x == 12.0F && move->GetDelta().y == -4.0F,
                        "and it carries the delta");
        }

        recorder.clear();
        (void)bridge.take(frame, &recorder);
        test::check(recorder.count() == 0, "a pointer that did not move sends nothing");
    }

    void a_button_reports_a_press_and_a_release_at_the_pointer() {
        test::section("a button reports a press and a release at the pointer");

        InputBridge bridge;
        Recorder recorder;

        pf::InputFrame down = with_button(pf::MouseButton::Left);
        down.mouse_position = engine::Vec2{ 100.0F, 50.0F };
        (void)bridge.take(down, &recorder);

        // The move comes first, so a widget knows what is under the pointer
        // before it is asked about a press there.
        test::check(recorder.count() == 2, "the first frame sends a move and a press");
        test::check(recorder.at<moth_ui::EventMouseMove>(0) != nullptr, "the move is first");
        const auto* pressed = recorder.at<moth_ui::EventMouseDown>(1);
        test::check(pressed != nullptr, "the press is second");
        if (pressed != nullptr) {
            test::check(pressed->GetButton() == moth_ui::MouseButton::Left,
                        "it names the left button");
            test::check(pressed->GetPosition().x == 100 && pressed->GetPosition().y == 50,
                        "and it happened where the pointer is");
        }

        recorder.clear();
        pf::InputFrame up;
        up.mouse_position = down.mouse_position;
        (void)bridge.take(up, &recorder);
        test::check(recorder.count() == 1, "the release sends one event");
        test::check(recorder.at<moth_ui::EventMouseUp>(0) != nullptr, "and it is a release");
    }

    void a_declined_event_leaves_the_frame_alone() {
        test::section("a declined event leaves the frame alone");

        InputBridge bridge;
        Recorder recorder;
        recorder.consume = false;

        pf::InputFrame frame = with_key(pf::Key::W);
        const bool took = bridge.take(frame, &recorder);

        test::check(!took, "the bridge reports it took nothing");
        test::check(key_down(frame, pf::Key::W), "the key is still down for the game");
        test::check(!bridge.owns(pf::Key::W), "and the UI owns nothing");
    }

    void a_consumed_key_never_reaches_the_game() {
        test::section("a consumed key never reaches the game");

        InputBridge bridge;
        Recorder recorder;
        recorder.consume = true;

        pf::InputFrame frame = with_key(pf::Key::W);
        const bool took = bridge.take(frame, &recorder);

        test::check(took, "the bridge reports it took something");
        test::check(!key_down(frame, pf::Key::W), "the key is gone from the frame");
        test::check(bridge.owns(pf::Key::W), "and the UI owns it");
    }

    void a_taken_key_stays_taken_until_it_comes_up() {
        test::section("a taken key stays taken until it comes up");

        InputBridge bridge;
        Recorder recorder;
        recorder.consume = true;

        pf::InputFrame down = with_key(pf::Key::W);
        (void)bridge.take(down, &recorder);

        // The whole hold, not only the edge. Without this the game reads the key
        // as down from the second frame onward, which is a pause menu that
        // swallows the press and then lets the player walk anyway.
        recorder.clear();
        recorder.consume = false;
        pf::InputFrame held = with_key(pf::Key::W);
        const bool took = bridge.take(held, &recorder);
        test::check(took, "the second frame is taken as well");
        test::check(!key_down(held, pf::Key::W), "the held key is still gone from the frame");
        test::check(recorder.count() == 0, "and no second press is sent");

        pf::InputFrame up;
        (void)bridge.take(up, &recorder);
        test::check(recorder.count() == 1, "the release still reaches the UI");
        test::check(!bridge.owns(pf::Key::W), "and the claim ends there");

        pf::InputFrame again = with_key(pf::Key::W);
        (void)bridge.take(again, &recorder);
        test::check(key_down(again, pf::Key::W), "so the next press reaches the game");
    }

    void a_taken_button_owns_its_release() {
        test::section("a taken button owns its release");

        InputBridge bridge;
        Recorder recorder;
        recorder.consume = true;

        pf::InputFrame down = with_button(pf::MouseButton::Left);
        (void)bridge.take(down, &recorder);
        test::check(bridge.owns(pf::MouseButton::Left), "the press is taken");
        test::check(!button_down(down, pf::MouseButton::Left),
                    "and the button is gone from the frame");

        recorder.clear();
        recorder.consume = false;
        pf::InputFrame held = with_button(pf::MouseButton::Left);
        (void)bridge.take(held, &recorder);
        test::check(!button_down(held, pf::MouseButton::Left),
                    "a drag that began on the UI stays with the UI");

        recorder.clear();
        pf::InputFrame up;
        const bool took = bridge.take(up, &recorder);

        // The release goes to the UI whatever the UI says about it. A press
        // taken on a menu owns its release, and that release is what activates a
        // moth_ui button.
        test::check(recorder.count() == 1, "the release reaches the UI");
        test::check(recorder.at<moth_ui::EventMouseUp>(0) != nullptr, "and it is a release");
        test::check(!took, "the frame is left alone once the button is up");
        test::check(!bridge.owns(pf::MouseButton::Left), "and the claim is over");
    }

    void the_next_frame_compares_against_the_devices_and_not_the_leftovers() {
        test::section("the next frame compares against the devices, not the leftovers");

        InputBridge bridge;
        Recorder recorder;
        recorder.consume = true;

        pf::InputFrame down = with_key(pf::Key::W);
        (void)bridge.take(down, &recorder);
        test::check(!key_down(down, pf::Key::W), "the taken key left the frame");

        // The frame the caller keeps says the key is up. If the bridge compared
        // the next frame against that, it would see a press again on every frame
        // the key is held. So it has to keep what the devices reported.
        recorder.clear();
        pf::InputFrame held = with_key(pf::Key::W);
        (void)bridge.take(held, &recorder);
        test::check(recorder.count() == 0, "a held key sends no second press");
    }

    void a_reload_drops_what_the_ui_owned() {
        test::section("a reload drops what the UI owned");

        InputBridge bridge;
        Recorder recorder;
        recorder.consume = true;

        pf::InputFrame down = with_key(pf::Key::W);
        (void)bridge.take(down, &recorder);
        test::check(bridge.owns(pf::Key::W), "the UI owns the key");

        // A reload builds a new node tree, and the new one knows nothing about
        // a key the old one took. Keeping the claim would leave the game unable
        // to read that key until a person let go and pressed it again.
        bridge.forget();
        test::check(!bridge.owns(pf::Key::W), "and a reload drops the claim");

        pf::InputFrame held = with_key(pf::Key::W);
        recorder.consume = false;
        (void)bridge.take(held, &recorder);
        test::check(key_down(held, pf::Key::W), "so the game reads the key again");
    }

    void no_listener_sends_nothing_and_holds_nothing() {
        test::section("no listener sends nothing and holds nothing");

        InputBridge bridge;
        Recorder recorder;
        recorder.consume = true;

        pf::InputFrame down = with_key(pf::Key::W);
        (void)bridge.take(down, &recorder);
        test::check(bridge.owns(pf::Key::W), "the UI owns the key");

        // A runtime whose layout failed to load passes null here. It must not
        // keep a claim the layout made before it went away.
        pf::InputFrame held = with_key(pf::Key::W);
        const bool took = bridge.take(held, nullptr);
        test::check(!took, "a null listener takes nothing");
        test::check(key_down(held, pf::Key::W), "the frame reaches the game whole");
        test::check(!bridge.owns(pf::Key::W), "and no claim survives");
    }

    void a_null_layout_answers_nothing() {
        test::section("a null layout answers nothing");

        // The listener points at the owner rather than at the root, so it has to
        // survive the owner holding nothing. That is the state between a failed
        // load and the next reload.
        std::shared_ptr<moth_ui::Node> root;
        engine::ui::LayoutListener listener{ &root };

        const moth_ui::EventKey event{ moth_ui::KeyAction::Down, moth_ui::Key::W, 0 };
        test::check(!listener.OnEvent(event), "an empty owner consumes nothing");

        engine::ui::LayoutListener no_owner{ nullptr };
        test::check(!no_owner.OnEvent(event), "and neither does no owner at all");
    }

    void every_engine_key_names_a_moth_ui_key() {
        test::section("every engine key names a moth_ui key");

        bool all_named = true;
        for (std::size_t i = 0; i < pf::kKeyCount; ++i) {
            if (engine::ui::moth_key(static_cast<pf::Key>(i)) == moth_ui::Key::Unknown) {
                all_named = false;
            }
        }
        test::check(all_named, "no key falls through to Unknown");

        bool all_buttons_named = true;
        for (std::size_t i = 0; i < pf::kMouseButtonCount; ++i) {
            if (engine::ui::moth_button(static_cast<pf::MouseButton>(i)) ==
                moth_ui::MouseButton::Unknown) {
                all_buttons_named = false;
            }
        }
        test::check(all_buttons_named, "and neither does a button");

        // The two enums are different sizes and different orders, so a mapping
        // that shifted by one would still name something. These pin the ends.
        test::check(engine::ui::moth_key(pf::Key::A) == moth_ui::Key::A, "A is A");
        test::check(engine::ui::moth_key(pf::Key::Down) == moth_ui::Key::Down, "Down is Down");
        test::check(engine::ui::moth_key(pf::Key::Num0) == moth_ui::Key::N0, "Num0 is N0");
        test::check(engine::ui::moth_key(pf::Key::Enter) == moth_ui::Key::Return,
                    "Enter is Return");
        test::check(engine::ui::moth_button(pf::MouseButton::Middle) ==
                        moth_ui::MouseButton::Middle,
                    "Middle is Middle");
        test::check(engine::ui::moth_button(pf::MouseButton::Right) ==
                        moth_ui::MouseButton::Right,
                    "Right is Right");
    }

    /**
     * A real moth_ui tree, with no device under it.
     *
     * All three of the things a Context needs are default constructible and do
     * nothing until create() opens them on a device. A node touches the
     * renderer when it draws and never when it takes an event, so a tree built
     * this way answers events correctly with no GPU. That is what lets the last
     * three cases below run in CI.
     */
    struct Tree {
        engine::ui::ImageFactory images;
        engine::ui::FontFactory fonts;
        engine::ui::Renderer renderer;
        moth_ui::Context context{ &images, &fonts, &renderer };
        std::shared_ptr<moth_ui::Group> group = moth_ui::Group::Create(context);
        std::shared_ptr<moth_ui::Node> root = group;
    };

    void events_reach_a_node_of_a_real_layout() {
        test::section("events reach a node of a real layout");

        Tree tree;
        const std::shared_ptr<moth_ui::Node> child = moth_ui::Node::Create(tree.context);
        tree.group->AddChild(child);

        std::vector<int> seen;
        child->SetEventHandler([&seen](moth_ui::Node*, const moth_ui::Event& event) {
            seen.push_back(event.GetType());
            return false;
        });

        engine::ui::LayoutListener listener{ &tree.root };
        InputBridge bridge;

        pf::InputFrame press = with_button(pf::MouseButton::Left);
        press.mouse_position = engine::Vec2{ 40.0F, 24.0F };
        (void)bridge.take(press, &listener);

        pf::InputFrame release;
        release.mouse_position = press.mouse_position;
        (void)bridge.take(release, &listener);

        pf::InputFrame key = with_key(pf::Key::Escape);
        key.mouse_position = press.mouse_position;
        (void)bridge.take(key, &listener);

        pf::InputFrame up;
        up.mouse_position = press.mouse_position;
        (void)bridge.take(up, &listener);

        const std::vector<int> expected{ moth_ui::EVENTTYPE_MOUSE_MOVE,
                                         moth_ui::EVENTTYPE_MOUSE_DOWN,
                                         moth_ui::EVENTTYPE_MOUSE_UP,
                                         moth_ui::EVENTTYPE_KEY,
                                         moth_ui::EVENTTYPE_KEY };
        test::check(seen == expected,
                    "a move, a press, a release and both key edges reach the node");
    }

    void a_node_that_consumes_takes_the_input_from_the_game() {
        test::section("a node that consumes takes the input from the game");

        // This is the pause menu case end to end. The layout is real, the node
        // answers true, and the frame the game reads comes back without the key.
        Tree tree;
        const std::shared_ptr<moth_ui::Node> child = moth_ui::Node::Create(tree.context);
        tree.group->AddChild(child);
        child->SetEventHandler([](moth_ui::Node*, const moth_ui::Event& event) {
            return event.GetType() == moth_ui::EVENTTYPE_KEY;
        });

        engine::ui::LayoutListener listener{ &tree.root };
        InputBridge bridge;

        pf::InputFrame frame = with_key(pf::Key::W);
        const bool took = bridge.take(frame, &listener);

        test::check(took, "the layout took the key");
        test::check(!key_down(frame, pf::Key::W), "so the game never sees it");
    }

    void a_captured_node_answers_before_the_broadcast() {
        test::section("a captured node answers before the broadcast");

        // A widget in an exclusive input mode has to win over its siblings. The
        // bridge follows what moth_ui::flow::TransitioningLayer does, and this
        // is the case that fails without it.
        Tree tree;
        const std::shared_ptr<moth_ui::Node> sibling = moth_ui::Node::Create(tree.context);
        const std::shared_ptr<moth_ui::Node> captured = moth_ui::Node::Create(tree.context);
        // The capture holder is added first, so a depth-first broadcast reaches
        // the sibling before it. Adding it last would let the test pass with no
        // capture handling at all, because moth_ui walks children in reverse
        // z-order.
        tree.group->AddChild(captured);
        tree.group->AddChild(sibling);

        int sibling_saw = 0;
        sibling->SetEventHandler([&sibling_saw](moth_ui::Node*, const moth_ui::Event&) {
            ++sibling_saw;
            return true;
        });
        int captured_saw = 0;
        captured->SetEventHandler([&captured_saw](moth_ui::Node*, const moth_ui::Event&) {
            ++captured_saw;
            return true;
        });

        tree.group->SetCapturedNode(captured);

        engine::ui::LayoutListener listener{ &tree.root };
        InputBridge bridge;
        pf::InputFrame frame = with_key(pf::Key::W);
        (void)bridge.take(frame, &listener);

        test::check(captured_saw == 1, "the capture holder saw the key");
        test::check(sibling_saw == 0, "and the sibling never did");
    }

} // namespace

int main() {
    an_unchanged_frame_sends_nothing();
    a_key_reports_both_edges_and_not_the_hold();
    a_key_event_carries_the_modifiers_of_its_frame();
    the_pointer_reports_where_it_is_and_how_far_it_moved();
    a_button_reports_a_press_and_a_release_at_the_pointer();
    a_declined_event_leaves_the_frame_alone();
    a_consumed_key_never_reaches_the_game();
    a_taken_key_stays_taken_until_it_comes_up();
    a_taken_button_owns_its_release();
    the_next_frame_compares_against_the_devices_and_not_the_leftovers();
    a_reload_drops_what_the_ui_owned();
    no_listener_sends_nothing_and_holds_nothing();
    a_null_layout_answers_nothing();
    every_engine_key_names_a_moth_ui_key();
    events_reach_a_node_of_a_real_layout();
    a_node_that_consumes_takes_the_input_from_the_game();
    a_captured_node_answers_before_the_broadcast();
    return test::report();
}
