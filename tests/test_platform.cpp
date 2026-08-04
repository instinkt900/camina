// M4.5 tests for the two platform pieces hot reload stands on.
//
// The watcher is polled rather than event driven, so a test drives it by
// calling poll() and never by sleeping. Every test here sets the interval and
// the settle time to zero, which makes a change arrive on the walk after the
// one that first saw it. That is the rule the debounce is built on, and it is
// what these tests check.
//
// The process runner is checked by running this same program again. That gives
// a child that exists on both platforms, and it lets one test read back the
// arguments the child received. Windows hands a child one string and the child
// splits it, so that test is the only thing that proves the quoting.

#include "check.h"
#include "platform/process.h"
#include "platform/watch.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

    using test::check;
    namespace pf = engine::platform;

    /// Runs this program as a child that exits with the code it is given.
    constexpr std::string_view kChildExit = "--child-exit";
    /// Runs this program as a child that writes back the arguments it received.
    constexpr std::string_view kChildArgs = "--child-args";

    std::filesystem::path scratch(std::string_view name) {
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "camina_test_platform" / name;
        test::remove_tree(path);
        std::filesystem::create_directories(path);
        return path;
    }

    void write_file(const std::filesystem::path& path, std::string_view text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
    }

    /// A watcher on @p root with the timers off, so a test drives it by polling.
    void start_watcher(pf::DirectoryWatcher& watcher, const std::filesystem::path& root) {
        watcher.set_interval(std::chrono::milliseconds{ 0 });
        watcher.set_settle(std::chrono::milliseconds{ 0 });
        check(watcher.start(root), "the watcher starts on a directory that is there");
    }

    /// Polls once and reports whether that poll produced nothing.
    [[nodiscard]] bool quiet(pf::DirectoryWatcher& watcher) {
        std::vector<pf::WatchEvent> changes;
        return !watcher.poll(changes) && changes.empty();
    }

    void test_a_missing_directory_will_not_start() {
        pf::DirectoryWatcher watcher;
        check(!watcher.start(scratch("missing") / "not_here"),
              "starting on a directory that is not there fails");

        std::vector<pf::WatchEvent> changes;
        check(!watcher.poll(changes),
              "polling a watcher that never started reports nothing");
    }

    void test_the_files_already_there_are_not_a_change() {
        const std::filesystem::path root = scratch("existing");
        write_file(root / "a.txt", "one");
        write_file(root / "deep" / "b.txt", "two");

        pf::DirectoryWatcher watcher;
        start_watcher(watcher, root);
        check(watcher.size() == 2, "the first walk finds both files");

        // The point of the test. A watcher that reported its starting tree as
        // a pile of changes would cook the whole tree on the first frame.
        check(quiet(watcher), "the files that were already there report nothing");
        check(quiet(watcher), "and they still report nothing on the next poll");
    }

    void test_a_new_file_arrives_after_it_settles() {
        const std::filesystem::path root = scratch("added");
        write_file(root / "a.txt", "one");

        pf::DirectoryWatcher watcher;
        start_watcher(watcher, root);

        write_file(root / "deep" / "new.txt", "hello");
        check(quiet(watcher), "the walk that first sees a new file reports nothing");

        std::vector<pf::WatchEvent> changes;
        check(watcher.poll(changes) && changes.size() == 1,
              "the next walk reports the new file");
        if (changes.size() != 1) {
            return;
        }
        check(changes.front().change == pf::FileChange::Added, "it reports as added");
        // A manifest stores forward slashes, so a watcher that reported a
        // backslash on Windows would never match an entry.
        check(changes.front().relative == "deep/new.txt",
              "the path is relative to the root and uses forward slashes");
        check(quiet(watcher), "the same file does not report a second time");
    }

    void test_a_changed_file_reports_as_modified() {
        const std::filesystem::path root = scratch("modified");
        write_file(root / "a.txt", "one");

        pf::DirectoryWatcher watcher;
        start_watcher(watcher, root);

        write_file(root / "a.txt", "one and more");
        check(quiet(watcher), "the walk that first sees the change reports nothing");

        std::vector<pf::WatchEvent> changes;
        check(watcher.poll(changes) && changes.size() == 1, "the next walk reports it");
        if (changes.size() != 1) {
            return;
        }
        check(changes.front().change == pf::FileChange::Modified,
              "a file that was already known reports as modified");
        check(changes.front().relative == "a.txt", "and it names the file");
    }

    void test_a_change_that_keeps_the_length_still_reports() {
        const std::filesystem::path root = scratch("same_length");
        write_file(root / "a.txt", "one");

        pf::DirectoryWatcher watcher;
        start_watcher(watcher, root);

        // Editing one character of a scene file does this, and a watcher that
        // compared only the size would never see it.
        //
        // The write time is set rather than left to the file system. Two
        // writes inside one write-time tick are the case watch.h warns about,
        // and the tick on Windows is coarse enough for two writes this close
        // together to land in the same one. Issue #57 is what removes the
        // limitation. This test is about the write time being compared at all,
        // so it makes sure there is a difference to compare.
        const std::filesystem::path file = root / "a.txt";
        write_file(file, "two");
        std::filesystem::last_write_time(file, std::filesystem::last_write_time(file) +
                                                   std::chrono::seconds{ 2 });
        check(quiet(watcher), "the walk that first sees it reports nothing");

        std::vector<pf::WatchEvent> changes;
        check(watcher.poll(changes) && changes.size() == 1,
              "a file that changed without changing length still reports");
        if (changes.size() != 1) {
            return;
        }
        check(changes.front().change == pf::FileChange::Modified, "and it reports as modified");
    }

    void test_a_deleted_file_reports_as_removed() {
        const std::filesystem::path root = scratch("removed");
        write_file(root / "a.txt", "one");
        write_file(root / "b.txt", "two");

        pf::DirectoryWatcher watcher;
        start_watcher(watcher, root);

        std::filesystem::remove(root / "b.txt");
        check(quiet(watcher), "the walk that first sees the removal reports nothing");

        std::vector<pf::WatchEvent> changes;
        check(watcher.poll(changes) && changes.size() == 1, "the next walk reports it");
        if (changes.size() != 1) {
            return;
        }
        check(changes.front().change == pf::FileChange::Removed, "it reports as removed");
        check(changes.front().relative == "b.txt", "and it names the file");
        check(quiet(watcher), "and it does not report again");
        check(watcher.size() == 1, "the watcher forgets the file that went away");
    }

    void test_a_file_still_being_written_is_held_back() {
        const std::filesystem::path root = scratch("settling");
        write_file(root / "a.txt", "one");

        pf::DirectoryWatcher watcher;
        start_watcher(watcher, root);

        // This is the failure the debounce exists to stop. A cooker handed a
        // file part way through a save reads a truncated glTF and reports a
        // parse error that names nothing the person did.
        write_file(root / "a.txt", "one two");
        check(quiet(watcher), "a file that just changed is not reported yet");

        write_file(root / "a.txt", "one two three");
        check(quiet(watcher), "and a file that changed again is still not reported");

        write_file(root / "a.txt", "one two three four");
        check(quiet(watcher), "however long the writing goes on");

        std::vector<pf::WatchEvent> changes;
        check(watcher.poll(changes) && changes.size() == 1,
              "the file reports once it stops moving");
        if (changes.size() != 1) {
            return;
        }
        check(changes.front().change == pf::FileChange::Modified, "and it reports as modified");
    }

    void test_a_change_that_undoes_itself_reports_nothing() {
        const std::filesystem::path root = scratch("undone");
        const std::filesystem::path file = root / "a.txt";
        write_file(file, "one");

        pf::DirectoryWatcher watcher;
        start_watcher(watcher, root);
        const auto written = std::filesystem::last_write_time(file);

        // Same length, so only the write time moves. That is what lets the
        // next step put the file back exactly as it was reported.
        write_file(file, "two");
        check(quiet(watcher), "the changed file is waiting to settle");

        write_file(file, "one");
        std::filesystem::last_write_time(file, written);

        // The file is byte for byte and stamp for stamp what the watcher
        // already knows, so there is nothing to tell anybody about.
        check(quiet(watcher), "a file that went back to what it was reports nothing");
        check(quiet(watcher), "and it stays quiet on the next poll");
    }

    void test_the_interval_keeps_a_poll_from_walking() {
        const std::filesystem::path root = scratch("interval");
        write_file(root / "a.txt", "one");

        pf::DirectoryWatcher watcher;
        start_watcher(watcher, root);
        // The first poll after start() always walks, so this uses it up.
        check(quiet(watcher), "the first poll walks and finds nothing");

        watcher.set_interval(std::chrono::hours{ 1 });
        write_file(root / "a.txt", "one two");
        check(quiet(watcher), "a poll inside the interval does not walk");
        check(quiet(watcher), "and neither does the next one");

        // With the interval back off, the same change comes through. Without
        // this the test above would pass on a watcher that never works.
        watcher.set_interval(std::chrono::milliseconds{ 0 });
        check(quiet(watcher), "the first walk after the wait sees the change");

        std::vector<pf::WatchEvent> changes;
        check(watcher.poll(changes) && changes.size() == 1, "and the next one reports it");
    }

    void test_several_files_report_together() {
        const std::filesystem::path root = scratch("several");
        write_file(root / "a.txt", "one");

        pf::DirectoryWatcher watcher;
        start_watcher(watcher, root);

        write_file(root / "a.txt", "one two");
        write_file(root / "b.txt", "two");
        std::filesystem::create_directories(root / "empty");
        check(quiet(watcher), "the first walk sees them");

        std::vector<pf::WatchEvent> changes;
        check(watcher.poll(changes) && changes.size() == 2,
              "one poll reports every file that settled");
        // A directory holds no bytes to cook, and the cooker walks the tree
        // itself, so an empty one is not a change anybody can act on.
        check(watcher.size() == 2, "a directory is not counted as a file");
    }

    void test_a_program_that_is_not_there_does_not_run() {
        const std::filesystem::path missing = scratch("process") / "no_such_program";
        const pf::ProcessResult result = pf::run_process(missing, {});
        check(!result.ran, "a program that is not there reports that it did not run");
    }

    void test_an_exit_code_comes_back(const std::filesystem::path& self) {
        const pf::ProcessResult ok = pf::run_process(self, { std::string{ kChildExit }, "0" });
        check(ok.ran && ok.exit_code == 0, "a child that exits with zero reports zero");

        // The caller has to tell a cook that failed from one that worked, and
        // the exit code is the only thing that says so.
        const pf::ProcessResult bad = pf::run_process(self, { std::string{ kChildExit }, "3" });
        check(bad.ran && bad.exit_code == 3, "a child that exits with three reports three");
    }

    void test_the_child_receives_the_arguments_unchanged(const std::filesystem::path& self) {
        const std::filesystem::path root = scratch("arguments");
        const std::filesystem::path out = root / "seen.txt";

        // A space is what a path under "Program Files" carries, and the quote
        // and the backslash are what the Windows quoting rules are about. A
        // shell would also read the dollar and the backtick, and this runner
        // starts the program directly so that nothing does.
        const std::vector<std::string> sent{ "plain", "with a space", "with\"quote",
                                             "back\\slash", "trailing\\", "$name`cmd`" };

        std::vector<std::string> arguments{ std::string{ kChildArgs }, out.string() };
        arguments.insert(arguments.end(), sent.begin(), sent.end());

        const pf::ProcessResult result = pf::run_process(self, arguments);
        check(result.ran && result.exit_code == 0, "the child ran and reported the arguments");

        std::vector<std::string> seen;
        std::ifstream file(out, std::ios::binary);
        for (std::string line; std::getline(file, line);) {
            seen.push_back(line);
        }

        check(seen.size() == sent.size(), "the child received every argument");
        if (seen.size() != sent.size()) {
            return;
        }
        bool same = true;
        for (std::size_t at = 0; at < sent.size(); ++at) {
            same = same && seen[at] == sent[at];
        }
        check(same, "and each one arrived exactly as it was sent");
    }

    /// Writes the arguments after the file name, one to a line.
    int child_args(int argc, char** argv) {
        std::ofstream file(argv[2], std::ios::binary | std::ios::trunc);
        for (int at = 3; at < argc; ++at) {
            file << argv[at] << '\n';
        }
        return file ? 0 : 1;
    }

} // namespace

int main(int argc, char** argv) {
    // The process tests run this same program as the child. Doing that before
    // anything else keeps the child out of the test output.
    if (argc >= 3 && std::string_view{ argv[1] } == kChildExit) {
        return std::atoi(argv[2]);
    }
    if (argc >= 3 && std::string_view{ argv[1] } == kChildArgs) {
        return child_args(argc, argv);
    }

    const std::filesystem::path self = std::filesystem::absolute(argv[0]);

    test::section("watcher");
    test_a_missing_directory_will_not_start();
    test_the_files_already_there_are_not_a_change();
    test_a_new_file_arrives_after_it_settles();
    test_a_changed_file_reports_as_modified();
    test_a_change_that_keeps_the_length_still_reports();
    test_a_deleted_file_reports_as_removed();
    test_a_file_still_being_written_is_held_back();
    test_a_change_that_undoes_itself_reports_nothing();
    test_the_interval_keeps_a_poll_from_walking();
    test_several_files_report_together();

    test::section("process");
    test_a_program_that_is_not_there_does_not_run();
    test_an_exit_code_comes_back(self);
    test_the_child_receives_the_arguments_unchanged(self);

    return test::report();
}
