// M4.5 tests for the two platform pieces hot reload stands on.
//
// The watcher cases run over a backend the caller names, so one set of cases
// covers every backend and the two are compared rather than tested separately.
//
// A test asks the watcher to wait rather than sleeping itself. It asks twice
// for each change: once with a window shorter than the settle time, which must
// report nothing, and once with a window long enough for any backend, which
// must report it. That states the debounce in time rather than in polls, which
// is the only form a backend fed by the operating system can answer.
//
// The process runner is checked by running this same program again. That gives
// a child that exists on both platforms, and it lets one test read back the
// arguments the child received. Windows hands a child one string and the child
// splits it, so that test is the only thing that proves the quoting.

#include "check.h"
#include "platform/paths.h"
#include "platform/process.h"
#include "platform/watch.h"
#include "platform/watch_backend.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

    using test::check;
    namespace pf = engine::platform;

    /// Runs this program as a child that exits with the code it is given.
    constexpr std::string_view kChildExit = "--child-exit";
    /// Runs this program as a child that writes back the arguments it received.
    constexpr std::string_view kChildArgs = "--child-args";

    /// Names this binary's scratch tree. See test::scratch.
    constexpr std::string_view kSuite = "platform";

    std::filesystem::path scratch(std::string_view name) {
        return test::scratch(kSuite, name);
    }

    void write_file(const std::filesystem::path& path, std::string_view text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
    }

    /// Makes the backend a run of the watcher cases drives.
    using MakeBackend = std::unique_ptr<pf::WatchBackend> (*)();

    /// One run of the watcher cases, over one backend.
    struct WatcherCase {
        MakeBackend make = nullptr;
        /// Names the backend. It joins each scratch path, so two runs cannot
        /// share a directory and delete each other's fixtures.
        std::string_view label;
    };

    /// How long a change must hold still before the watcher reports it.
    ///
    /// A test asks for a change and then asks that nothing arrived yet, so the
    /// two windows below sit either side of this. It is long enough that the
    /// gap is not a race and short enough that the suite does not crawl.
    constexpr std::chrono::milliseconds kSettle{ 200 };

    /// How long a test waits for a change it expects. Only a failure waits it out.
    constexpr std::chrono::milliseconds kArrives{ 5000 };

    /// How long a test waits to satisfy itself that nothing is coming.
    ///
    /// A change cannot be reported before it has held still for kSettle, so
    /// this window is safe by a factor of four whatever the machine is doing.
    constexpr std::chrono::milliseconds kQuiet{ 50 };

    /// How often the polling backend walks. A native backend ignores it.
    constexpr std::chrono::milliseconds kInterval{ 5 };

    std::filesystem::path case_scratch(const WatcherCase& over, std::string_view name) {
        return scratch(std::string{ over.label } + "_" + std::string{ name });
    }

    /// A watcher on @p root, set up so a test waits rather than sleeps.
    void start_watcher(pf::DirectoryWatcher& watcher, const std::filesystem::path& root) {
        watcher.set_interval(kInterval);
        watcher.set_settle(kSettle);
        check(watcher.start(root), "the watcher starts on a directory that is there");
    }

    /// Waits for the changes a test expects, and reports whether it got them.
    [[nodiscard]] bool arrived(pf::DirectoryWatcher& watcher, std::vector<pf::WatchEvent>& out,
                               std::size_t count) {
        return watcher.wait(out, kArrives) && out.size() == count;
    }

    /// Waits a short while and reports whether nothing came.
    [[nodiscard]] bool quiet(pf::DirectoryWatcher& watcher) {
        std::vector<pf::WatchEvent> changes;
        return !watcher.wait(changes, kQuiet) && changes.empty();
    }

    void test_a_missing_directory_will_not_start(const WatcherCase& over) {
        pf::DirectoryWatcher watcher(over.make());
        check(!watcher.start(case_scratch(over, "missing") / "not_here"),
              "starting on a directory that is not there fails");

        std::vector<pf::WatchEvent> changes;
        check(!watcher.poll(changes), "polling a watcher that never started reports nothing");
        check(!watcher.wait(changes, kQuiet), "and waiting on one reports nothing either");
    }

    void test_the_files_already_there_are_not_a_change(const WatcherCase& over) {
        const std::filesystem::path root = case_scratch(over, "existing");
        write_file(root / "a.txt", "one");
        write_file(root / "deep" / "b.txt", "two");

        pf::DirectoryWatcher watcher(over.make());
        start_watcher(watcher, root);
        check(watcher.size() == 2, "the first look finds both files");

        // The point of the test. A watcher that reported its starting tree as
        // a pile of changes would cook the whole tree on the first frame.
        check(quiet(watcher), "the files that were already there report nothing");
        check(quiet(watcher), "and they still report nothing after that");
    }

    void test_a_new_file_arrives_after_it_settles(const WatcherCase& over) {
        const std::filesystem::path root = case_scratch(over, "added");
        write_file(root / "a.txt", "one");

        pf::DirectoryWatcher watcher(over.make());
        start_watcher(watcher, root);

        write_file(root / "deep" / "new.txt", "hello");
        check(quiet(watcher), "a new file is not reported before it settles");

        std::vector<pf::WatchEvent> changes;
        check(arrived(watcher, changes, 1), "and then the new file reports");
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

    void test_a_changed_file_reports_as_modified(const WatcherCase& over) {
        const std::filesystem::path root = case_scratch(over, "modified");
        write_file(root / "a.txt", "one");

        pf::DirectoryWatcher watcher(over.make());
        start_watcher(watcher, root);

        write_file(root / "a.txt", "one and more");
        check(quiet(watcher), "the change is not reported before it settles");

        std::vector<pf::WatchEvent> changes;
        check(arrived(watcher, changes, 1), "and then it reports");
        if (changes.size() != 1) {
            return;
        }
        check(changes.front().change == pf::FileChange::Modified,
              "a file that was already known reports as modified");
        check(changes.front().relative == "a.txt", "and it names the file");
    }

    void test_a_change_that_keeps_the_length_still_reports(const WatcherCase& over) {
        const std::filesystem::path root = case_scratch(over, "same_length");
        write_file(root / "a.txt", "one");

        pf::DirectoryWatcher watcher(over.make());
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

        std::vector<pf::WatchEvent> changes;
        check(arrived(watcher, changes, 1),
              "a file that changed without changing length still reports");
        if (changes.size() != 1) {
            return;
        }
        check(changes.front().change == pf::FileChange::Modified, "and it reports as modified");
    }

    void test_a_deleted_file_reports_as_removed(const WatcherCase& over) {
        const std::filesystem::path root = case_scratch(over, "removed");
        write_file(root / "a.txt", "one");
        write_file(root / "b.txt", "two");

        pf::DirectoryWatcher watcher(over.make());
        start_watcher(watcher, root);

        std::filesystem::remove(root / "b.txt");
        check(quiet(watcher), "the removal is not reported before it settles");

        std::vector<pf::WatchEvent> changes;
        check(arrived(watcher, changes, 1), "and then it reports");
        if (changes.size() != 1) {
            return;
        }
        check(changes.front().change == pf::FileChange::Removed, "it reports as removed");
        check(changes.front().relative == "b.txt", "and it names the file");
        check(quiet(watcher), "and it does not report again");
        check(watcher.size() == 1, "the watcher forgets the file that went away");
    }

    void test_a_file_still_being_written_is_held_back(const WatcherCase& over) {
        const std::filesystem::path root = case_scratch(over, "settling");
        write_file(root / "a.txt", "one");

        pf::DirectoryWatcher watcher(over.make());
        start_watcher(watcher, root);

        // This is the failure the debounce exists to stop. A cooker handed a
        // file part way through a save reads a truncated glTF and reports a
        // parse error that names nothing the person did.
        //
        // Each write restarts the settle clock, so the quiet windows here add
        // up to less than one settle time and the file still reports nothing.
        write_file(root / "a.txt", "one two");
        check(quiet(watcher), "a file that just changed is not reported yet");

        write_file(root / "a.txt", "one two three");
        check(quiet(watcher), "and a file that changed again is still not reported");

        write_file(root / "a.txt", "one two three four");
        check(quiet(watcher), "however long the writing goes on");

        std::vector<pf::WatchEvent> changes;
        check(arrived(watcher, changes, 1), "the file reports once it stops moving");
        if (changes.size() != 1) {
            return;
        }
        check(changes.front().change == pf::FileChange::Modified, "and it reports as modified");
    }

    void test_a_change_that_undoes_itself_reports_nothing(const WatcherCase& over) {
        const std::filesystem::path root = case_scratch(over, "undone");
        const std::filesystem::path file = root / "a.txt";
        write_file(file, "one");

        pf::DirectoryWatcher watcher(over.make());
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
        check(quiet(watcher), "and it stays quiet after that");
    }

    void test_several_files_report_together(const WatcherCase& over) {
        const std::filesystem::path root = case_scratch(over, "several");
        write_file(root / "a.txt", "one");

        pf::DirectoryWatcher watcher(over.make());
        start_watcher(watcher, root);

        write_file(root / "a.txt", "one two");
        write_file(root / "b.txt", "two");
        std::filesystem::create_directories(root / "empty");

        std::vector<pf::WatchEvent> changes;
        check(arrived(watcher, changes, 2), "one wait reports every file that settled");
        // A directory holds no bytes to cook, and the cooker walks the tree
        // itself, so an empty one is not a change anybody can act on.
        check(watcher.size() == 2, "a directory is not counted as a file");
    }

    /// Runs every case that any backend has to pass.
    void run_watcher_cases(const WatcherCase& over) {
        test_a_missing_directory_will_not_start(over);
        test_the_files_already_there_are_not_a_change(over);
        test_a_new_file_arrives_after_it_settles(over);
        test_a_changed_file_reports_as_modified(over);
        test_a_change_that_keeps_the_length_still_reports(over);
        test_a_deleted_file_reports_as_removed(over);
        test_a_file_still_being_written_is_held_back(over);
        test_a_change_that_undoes_itself_reports_nothing(over);
        test_several_files_report_together(over);
    }

    // A candidate that is waiting out the settle time has to wake wait() by
    // itself. The file stopped changing, so nothing reaches the backend for
    // it, and a wait that leans on the backend alone sits until the deadline.
    //
    // This drives the polling backend with its interval turned up, because
    // that is the only way here to make a backend that will not wake. A
    // native backend is in the same position by nature: no more events
    // arrive for a file nobody is writing.
    void test_a_settling_change_reports_without_the_backend() {
        const std::filesystem::path root = scratch("settle_wakes");
        write_file(root / "a.txt", "one");

        pf::DirectoryWatcher watcher(pf::make_polling_watch_backend());
        watcher.set_interval(kInterval);
        watcher.set_settle(kSettle);
        check(watcher.start(root), "the watcher starts on a directory that is there");

        write_file(root / "a.txt", "one two");
        check(quiet(watcher), "the change is waiting to settle");

        // From here the backend has nothing more to say for an hour.
        watcher.set_interval(std::chrono::hours{ 1 });

        const auto began = std::chrono::steady_clock::now();
        std::vector<pf::WatchEvent> changes;
        const bool reported = arrived(watcher, changes, 1);
        const auto took =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - began);

        check(reported, "the change still reports, on the watcher's own clock");
        // When it reports is the whole point. A wait that leans on the backend
        // sits out the deadline and finds the change only on the way out, so
        // it reports the right thing far too late. The settle time is kSettle,
        // and the deadline is more than twenty times that.
        check(took < kArrives / 2, "and it reports when it settles rather than at the deadline");
    }

    // The interval belongs to the polling backend alone, so this case names
    // that backend rather than running over whichever one a build has. It is
    // also the one case that drives poll() directly, because "a poll inside
    // the interval does not walk" is a statement about poll().
    void test_the_interval_keeps_a_poll_from_walking() {
        const std::filesystem::path root = scratch("interval");
        write_file(root / "a.txt", "one");

        pf::DirectoryWatcher watcher(pf::make_polling_watch_backend());
        watcher.set_interval(std::chrono::milliseconds{ 0 });
        watcher.set_settle(std::chrono::milliseconds{ 0 });
        check(watcher.start(root), "the watcher starts on a directory that is there");

        std::vector<pf::WatchEvent> changes;
        // The first poll after start() always walks, so this uses it up.
        check(!watcher.poll(changes), "the first poll walks and finds nothing");

        watcher.set_interval(std::chrono::hours{ 1 });
        write_file(root / "a.txt", "one two");
        check(!watcher.poll(changes), "a poll inside the interval does not walk");
        check(!watcher.poll(changes), "and neither does the next one");

        // With the interval back off, the same change comes through. Without
        // this the test above would pass on a watcher that never works.
        watcher.set_interval(std::chrono::milliseconds{ 0 });
        check(!watcher.poll(changes), "the first walk after the wait sees the change");
        check(watcher.poll(changes) && changes.size() == 1, "and the next one reports it");
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

    /**
     * M9.1. The editor saves its layout here, and nothing else in the engine
     * writes outside the build tree, so this is the one place that must be a
     * real directory the program may write to.
     *
     * The name is a test name rather than "editor", so a run never touches the
     * settings of somebody's editor. The directory is removed on the way out.
     */
    void test_the_preferences_directory_is_there_and_writable() {
        // The process id is in the name for the reason test::scratch carries
        // one: two runs of this binary would otherwise write and remove one
        // directory. See issue #293.
        const std::string application = "camina_test_paths_" + test::process_id();
        const std::filesystem::path directory = pf::preferences_directory(application.c_str());
        check(!directory.empty(), "the platform says where the settings of this user go");
        if (directory.empty()) {
            return;
        }

        // A path with a trailing separator joins as "dir//name" and its
        // filename() is empty, so both of these are what a caller relies on.
        check(directory.filename() == application,
              "the last part of the path is the application name");
        check(std::filesystem::is_directory(directory),
              "the directory is created rather than only named");

        const std::filesystem::path file = directory / "layout.ini";
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << "one";
        }
        check(std::filesystem::exists(file), "and a file can be written into it");

        // The file this test wrote, and then the directory only when it is the
        // one this test asked for. A version of preferences_directory() that
        // answered with the parent would fail the check above and still reach
        // here, and removing that tree would take the layout of somebody's
        // editor with it.
        //
        // The error_code form of both, because remove() throws on a directory
        // that still holds something. Anything else in there belongs to
        // somebody else, and leaving it is the answer rather than the failure.
        std::error_code failed;
        std::filesystem::remove(file, failed);
        if (directory.filename() == application) {
            std::filesystem::remove(directory, failed);
        }
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

    // Every backend runs the same cases, so the two are compared rather than
    // tested separately. Issue #481 adds the second entry here.
    test::section("watcher (polling)");
    run_watcher_cases(WatcherCase{ .make = pf::make_polling_watch_backend, .label = "poll" });

    test::section("watcher (polling only)");
    test_a_settling_change_reports_without_the_backend();
    test_the_interval_keeps_a_poll_from_walking();

    test::section("paths");
    test_the_preferences_directory_is_there_and_writable();

    test::section("process");
    test_a_program_that_is_not_there_does_not_run();
    test_an_exit_code_comes_back(self);
    test_the_child_receives_the_arguments_unchanged(self);

    return test::report();
}
