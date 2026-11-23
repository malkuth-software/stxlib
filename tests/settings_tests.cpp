// Tests of stxlib::settings (include/stxlib/settings.hpp) and the pieces it
// added to files.hpp (FileLock, identify), in a fresh directory under the
// host's temporary directory that the test removes again:
// - the schema's refusals (names, secrets, ranges, migrations);
// - a round trip, the file's form, unknown keys kept, a hand edit, a bad
//   value;
// - damage and recovery from the last good copy, and the defaults without
//   one;
// - migration, a file older than every step, and a file newer than the
//   program (read-only, owner decision 5);
// - two stores of one part: different keys both saved, the same key a
//   conflict (decision 1, option a), resolve both ways, finish's conflict
//   copy and adopting or discarding it, reload adopting only unchanged keys
//   (decision 7);
// - the FileLock in one process, and two processes racing their saves (the
//   test starts itself twice with "race").
// Tested, not proved (proofs/REPORT.md).

#include "stxlib/capabilities.hpp"
#include "stxlib/children.hpp"
#include "stxlib/clocks.hpp"
#include "stxlib/desktop.hpp"
#include "stxlib/files.hpp"
#include "stxlib/path.hpp"
#include "stxlib/settings.hpp"
#include "stxlib/text.hpp"

#include "test_support.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using stxlib::Buffer;
using stxlib::Byte;
using stxlib::Duration;
using stxlib::ErrorKind;
using stxlib::FileLock;
using stxlib::HostCode;
using stxlib::OperationResult;
using stxlib::OperationStatus;
using stxlib::Path;
using stxlib::TextView;
using stxlib::usize;
using stxlib::settings::Issue;
using stxlib::settings::LoadState;
using stxlib::settings::MigrationEntries;
using stxlib::settings::Resolution;
using stxlib::settings::SaveOutcome;
using stxlib::settings::Schema;
using stxlib::settings::Store;

TextView view_of(const std::string& text) {
    OperationResult<TextView> valid = stxlib::validate_utf8(text.data(), text.size());
    const TextView result = valid.succeeded() ? *valid.value() : TextView("");
    return result;
}

Path under(const Path& base, const std::string& relative) {
    OperationResult<Path> segment = Path::parse(view_of(relative));
    CHECK(segment.succeeded());
    if (segment.failed()) return Path();
    OperationResult<Path> joined = base.join(*segment.value());
    CHECK(joined.succeeded());
    if (joined.failed()) return Path();
    Path result = static_cast<OperationResult<Path>&&>(joined).take_value();
    return result;
}

std::string read_text(const Path& path) {
    OperationResult<Buffer<Byte>> read = stxlib::read_file(path, 1 << 20);
    if (read.failed()) return std::string();
    const stxlib::View<Byte> view = (*read.value()).view();
    std::string result(reinterpret_cast<const char*>(view.data()), view.count());
    return result;
}

bool write_text(const Path& path, const std::string& text) {
    const stxlib::View<Byte> bytes(reinterpret_cast<const Byte*>(text.data()), text.size());
    const OperationStatus written = stxlib::write_file(path, bytes, stxlib::Durability::none);
    const bool result = written.succeeded();
    return result;
}

bool exists(const Path& path) {
    const OperationResult<stxlib::FileInformation> found = stxlib::inspect(path, stxlib::LinkMode::follow);
    const bool result = found.succeeded();
    return result;
}

template <typename Value>
ErrorKind kind_of(const OperationResult<Value>& result) {
    const ErrorKind kind = result.succeeded() ? static_cast<ErrorKind>(0) : (*result.error()).kind();
    return kind;
}

template <typename Value>
HostCode code_of(const OperationResult<Value>& result) {
    const HostCode code = result.succeeded() ? HostCode::none : static_cast<HostCode>((*result.error()).code());
    return code;
}

HostCode code_of(const OperationStatus& status) {
    const HostCode code = status.succeeded() ? HostCode::none : static_cast<HostCode>((*status.error()).code());
    return code;
}

bool has_issue(const Store& store, const Issue issue) {
    const stxlib::View<stxlib::settings::Diagnostic> diagnostics = store.diagnostics();
    for (usize index = 0; index < diagnostics.count(); ++index) {
        if (diagnostics[index].issue == issue) return true;
    }
    return false;
}

class Scratch final {
public:
    explicit Scratch(const std::string& name) {
        OperationResult<Path> temporary = stxlib::temporary_directory();
        CHECK(temporary.succeeded());
        if (temporary.failed()) return;
        const long long stamp = stxlib::IntervalTime::now().count_nanoseconds();
        root = under(*temporary.value(), "stxlib-settings-" + name + "-" + std::to_string(stamp));
        CHECK(stxlib::create_directory(root).succeeded());
    }
    ~Scratch() { (void)stxlib::remove_tree(root); }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    Path root{};
};

// The test schema: version `version` of part "preferences".
Schema make_schema(const stxlib::u32 version) {
    OperationResult<Schema> made = Schema::create("preferences", version);
    CHECK(made.succeeded());
    if (made.failed()) return Schema();
    Schema schema = static_cast<OperationResult<Schema>&&>(made).take_value();
    CHECK(schema.add_boolean("view.wrap", false).succeeded());
    CHECK(schema.add_integer("font.size", 6, 72, 12).succeeded());
    CHECK(schema.add_real("brush.size", 0.5, 64.0, 3.5).succeeded());
    CHECK(schema.add_text("theme", 64, "light").succeeded());
    const TextView sides[] = {"left", "right"};
    CHECK(schema.add_choice("panel.side", stxlib::View<TextView>(sides), 0).succeeded());
    CHECK(schema.add_text_list("recent.path", 4096, 4).succeeded());
    return schema;
}

Store open_store(const Path& directory, const Schema& schema) {
    OperationResult<Store> opened = Store::open_in(directory, schema);
    CHECK(opened.succeeded());
    if (opened.failed()) return Store();
    Store result = static_cast<OperationResult<Store>&&>(opened).take_value();
    return result;
}

bool saved(Store& store) {
    const OperationResult<SaveOutcome> outcome = store.save();
    const bool result = outcome.succeeded();
    return result;
}

// ---- the schema --------------------------------------------------------------

void schema_refusals() {
    CHECK(Schema::create("Bad Part", 1).failed());
    CHECK(Schema::create("preferences", 0).failed());
    Schema schema = make_schema(1);
    CHECK(code_of(schema.add_boolean("view.wrap", true)) == HostCode::invalid_name);  // repeated
    CHECK(code_of(schema.add_boolean("9lives", true)) == HostCode::invalid_name);     // not a letter first
    CHECK(code_of(schema.add_text("sync.token", 64, "")) == HostCode::invalid_name);  // a secret
    CHECK(code_of(schema.add_text("account.password", 64, "")) == HostCode::invalid_name);
    CHECK(code_of(schema.add_integer("zoom", 10, 5, 7)) == HostCode::invalid_value);   // empty range
    CHECK(code_of(schema.add_integer("zoom", 1, 5, 7)) == HostCode::invalid_value);    // default outside
    CHECK(code_of(schema.add_text("name", 4, "toolong")) == HostCode::invalid_value);
    CHECK(code_of(schema.add_migration(1, nullptr)) == HostCode::invalid_value);
    CHECK(schema.version() == 1);
}

// ---- round trip, form, unknown keys, hand edits ------------------------------

void round_trip() {
    Scratch scratch("trip");
    const Schema schema = make_schema(1);
    {
        Store store = open_store(scratch.root, schema);
        CHECK(store.load_state() == LoadState::fresh);
        CHECK(store.integer("font.size") == 12);
        CHECK(store.text("theme") == TextView("light"));
        // Nothing changed: nothing written.
        CHECK(saved(store));
        CHECK(!exists(store.file()));
        CHECK(store.set_integer("font.size", 99).failed()); // outside the range
        CHECK(store.set_integer("font.size", 14).succeeded());
        CHECK(store.set_boolean("view.wrap", true).succeeded());
        CHECK(store.set_real("brush.size", 7.25).succeeded());
        CHECK(store.set_text("theme", "dark\nline").succeeded());
        CHECK(store.set_choice("panel.side", 1).succeeded());
        const TextView recent[] = {"/a/b.txt", "/c=d.txt"};
        CHECK(store.set_items("recent.path", stxlib::View<TextView>(recent)).succeeded());
        CHECK(store.changed());
        CHECK(saved(store));
        CHECK(!store.changed());
    }
    Store again = open_store(scratch.root, schema);
    CHECK(again.load_state() == LoadState::loaded);
    CHECK(again.integer("font.size") == 14);
    CHECK(again.boolean("view.wrap"));
    CHECK(again.real("brush.size") == 7.25);
    CHECK(again.text("theme") == TextView("dark\nline"));
    CHECK(again.choice("panel.side") == 1);
    CHECK(again.item_count("recent.path") == 2);
    CHECK(again.item("recent.path", 1) == TextView("/c=d.txt"));
    CHECK(again.diagnostics().count() == 0);
    // The file's form: header, schema line, escaped values, check line.
    const std::string text = read_text(again.file());
    CHECK(text.rfind("stxlib.settings 1\nschema=local.settings.store/preferences 1\n", 0) == 0);
    CHECK(text.find("theme=dark\\nline\n") != std::string::npos);
    CHECK(text.find("recent.path=/a/b.txt\nrecent.path=/c=d.txt\n") != std::string::npos);
    CHECK(text.find("\ncheck=blake2s:") != std::string::npos);
}

void unknown_keys_and_hand_edits() {
    Scratch scratch("unknown");
    const Schema wide = make_schema(1);
    OperationResult<Schema> narrow_made = Schema::create("preferences", 1);
    CHECK(narrow_made.succeeded());
    Schema narrow = static_cast<OperationResult<Schema>&&>(narrow_made).take_value();
    CHECK(narrow.add_integer("font.size", 6, 72, 12).succeeded());
    {
        Store store = open_store(scratch.root, wide);
        CHECK(store.set_text("theme", "sepia").succeeded());
        CHECK(saved(store));
    }
    {
        // A program that does not know "theme" keeps it.
        Store store = open_store(scratch.root, narrow);
        CHECK(store.set_integer("font.size", 20).succeeded());
        CHECK(saved(store));
    }
    {
        Store store = open_store(scratch.root, wide);
        CHECK(store.text("theme") == TextView("sepia"));
        CHECK(store.integer("font.size") == 20);
    }
    // A hand edit: the checksum disagrees, the value is read; a bad value
    // keeps its default with a diagnostic.
    Store probe = open_store(scratch.root, wide);
    std::string text = read_text(probe.file());
    const usize at = text.find("font.size=20");
    CHECK(at != std::string::npos);
    text.replace(at, 12, "font.size=30");
    const usize theme = text.find("theme=sepia");
    CHECK(theme != std::string::npos);
    text.insert(theme, "view.wrap=maybe\n");
    CHECK(write_text(probe.file(), text));
    Store edited = open_store(scratch.root, wide);
    CHECK(edited.load_state() == LoadState::loaded);
    CHECK(edited.integer("font.size") == 30);
    CHECK(!edited.boolean("view.wrap"));
    CHECK(has_issue(edited, Issue::checksum_mismatch));
    CHECK(has_issue(edited, Issue::invalid_value));
}

// ---- damage and recovery -------------------------------------------------------

void damage_and_recovery() {
    Scratch scratch("damage");
    const Schema schema = make_schema(1);
    Path file{};
    {
        Store store = open_store(scratch.root, schema);
        file = store.file();
        CHECK(store.set_integer("font.size", 16).succeeded());
        CHECK(saved(store));
        // The second save keeps the first file as the last good copy.
        CHECK(store.set_integer("font.size", 18).succeeded());
        CHECK(saved(store));
    }
    CHECK(exists(under(scratch.root, "preferences.settings.last-good")));
    // Truncated mid-line: damaged; the last good copy is read and restored.
    std::string text = read_text(file);
    CHECK(write_text(file, text.substr(0, text.size() / 2)));
    {
        Store store = open_store(scratch.root, schema);
        CHECK(store.load_state() == LoadState::recovered);
        CHECK(store.integer("font.size") == 16);
        CHECK(has_issue(store, Issue::damaged_file));
        CHECK(has_issue(store, Issue::recovered));
    }
    CHECK(read_text(file).find("font.size=16\n") != std::string::npos);
    OperationResult<stxlib::DirectoryListing> listing = stxlib::list_directory(scratch.root, 64);
    CHECK(listing.succeeded());
    bool set_aside = false;
    if (listing.succeeded()) {
        for (usize index = 0; index < (*listing.value()).count(); ++index) {
            const stxlib::PathText name = (*listing.value()).name(index);
            const std::string spelled(name.data(), name.count());
            if (spelled.rfind("preferences.settings.damaged-", 0) == 0) set_aside = true;
        }
    }
    CHECK(set_aside);
    // Damaged with no good copy: the defaults.
    Scratch bare("bare");
    CHECK(write_text(under(bare.root, "preferences.settings"), "not a settings file\n"));
    Store fresh = open_store(bare.root, schema);
    CHECK(fresh.load_state() == LoadState::reset);
    CHECK(fresh.integer("font.size") == 12);
}

// ---- migration and versions ------------------------------------------------------

OperationStatus rename_size(MigrationEntries& entries) noexcept {
    const OperationStatus result = entries.rename_key("size", "font.size");
    return result;
}

void migration_and_versions() {
    Scratch scratch("versions");
    {
        OperationResult<Schema> old_made = Schema::create("preferences", 1);
        CHECK(old_made.succeeded());
        Schema old_schema = static_cast<OperationResult<Schema>&&>(old_made).take_value();
        CHECK(old_schema.add_integer("size", 6, 72, 12).succeeded());
        Store store = open_store(scratch.root, old_schema);
        CHECK(store.set_integer("size", 22).succeeded());
        CHECK(saved(store));
    }
    Schema current = make_schema(2);
    CHECK(current.add_migration(1, &rename_size).succeeded());
    {
        Store store = open_store(scratch.root, current);
        CHECK(store.load_state() == LoadState::loaded);
        CHECK(store.integer("font.size") == 22);
        // Nothing changed, but the file is written in version 2.
        CHECK(saved(store));
        CHECK(read_text(store.file()).find("/preferences 2\n") != std::string::npos);
    }
    // Older than every step reaches: defaults, with a diagnostic.
    Scratch ancient("ancient");
    {
        OperationResult<Schema> old_made = Schema::create("preferences", 1);
        Schema old_schema = static_cast<OperationResult<Schema>&&>(old_made).take_value();
        CHECK(old_schema.add_integer("font.size", 6, 72, 12).succeeded());
        Store store = open_store(ancient.root, old_schema);
        CHECK(store.set_integer("font.size", 40).succeeded());
        CHECK(saved(store));
    }
    Schema third = make_schema(3);
    CHECK(third.add_migration(2, &rename_size).succeeded());
    {
        Store store = open_store(ancient.root, third);
        CHECK(store.integer("font.size") == 12);
        CHECK(has_issue(store, Issue::too_old));
    }
    // Newer than the program (decision 5): read, never written back.
    const Schema older = make_schema(1);
    Store reader = open_store(scratch.root, older);
    CHECK(reader.load_state() == LoadState::newer);
    CHECK(reader.read_only());
    CHECK(reader.integer("font.size") == 22);
    CHECK(reader.set_integer("font.size", 9).succeeded());
    const OperationResult<SaveOutcome> refused = reader.save();
    CHECK(code_of(refused) == HostCode::invalid_state);
    CHECK(read_text(reader.file()).find("font.size=22\n") != std::string::npos);
}

// ---- two stores of one part --------------------------------------------------------

void two_stores() {
    Scratch scratch("two");
    const Schema schema = make_schema(1);
    Store first = open_store(scratch.root, schema);
    Store second = open_store(scratch.root, schema);
    // Different keys: both saved.
    CHECK(first.set_integer("font.size", 30).succeeded());
    CHECK(saved(first));
    CHECK(second.set_text("theme", "dark").succeeded());
    const OperationResult<SaveOutcome> both = second.save();
    CHECK(both.succeeded());
    if (both.succeeded()) CHECK((*both.value()).conflicts == 0);
    {
        Store reader = open_store(scratch.root, schema);
        CHECK(reader.integer("font.size") == 30);
        CHECK(reader.text("theme") == TextView("dark"));
    }
    // Frozen view: second has not adopted first's font size yet.
    CHECK(second.integer("font.size") == 12);
    // Reload adopts only unchanged keys: second's pending choice stays.
    CHECK(first.set_boolean("view.wrap", true).succeeded());
    CHECK(saved(first));
    CHECK(second.set_choice("panel.side", 1).succeeded());
    const OperationResult<bool> reloaded = second.reload_if_changed();
    CHECK(reloaded.succeeded() && *reloaded.value());
    CHECK(second.integer("font.size") == 30);
    CHECK(second.boolean("view.wrap"));
    CHECK(second.choice("panel.side") == 1);
    CHECK(saved(second));
    // The same key: the later save is refused for it, the file keeps the
    // earlier writer's value.
    CHECK(first.set_integer("font.size", 40).succeeded());
    CHECK(second.set_integer("font.size", 50).succeeded());
    CHECK(saved(first));
    const OperationResult<SaveOutcome> clash = second.save();
    CHECK(clash.succeeded());
    if (clash.succeeded()) CHECK((*clash.value()).conflicts == 1);
    CHECK(second.conflict_count() == 1);
    CHECK(second.conflict_key(0) == TextView("font.size"));
    CHECK(has_issue(second, Issue::conflict));
    CHECK(second.integer("font.size") == 50); // kept in memory
    CHECK(read_text(second.file()).find("font.size=40\n") != std::string::npos);
    // use_mine: the next save writes it.
    CHECK(second.resolve("font.size", Resolution::use_mine).succeeded());
    CHECK(saved(second));
    CHECK(read_text(second.file()).find("font.size=50\n") != std::string::npos);
    // keep_theirs: the file's value is adopted.
    CHECK(first.set_integer("font.size", 60).succeeded());
    CHECK(second.set_integer("font.size", 61).succeeded());
    CHECK(saved(second));
    CHECK(saved(first));
    CHECK(first.conflict_count() == 1);
    CHECK(first.resolve("font.size", Resolution::keep_theirs).succeeded());
    CHECK(first.integer("font.size") == 61);
    CHECK(!first.changed());
    CHECK(first.resolve("font.size", Resolution::keep_theirs).failed());
    // finish: a change still in conflict goes to a conflict copy.
    CHECK(first.set_text("theme", "first").succeeded());
    CHECK(second.set_text("theme", "second").succeeded());
    CHECK(saved(first));
    CHECK(second.finish().succeeded());
    CHECK(!second.changed());
    CHECK(exists(under(scratch.root, "preferences.1.settings")));
    Store later = open_store(scratch.root, schema);
    CHECK(later.text("theme") == TextView("first"));
    CHECK(later.conflict_copy_count() == 1);
    CHECK(has_issue(later, Issue::conflict_copy));
    if (later.conflict_copy_count() == 1) {
        CHECK(later.conflict_copy_number(0) == 1);
        CHECK(later.adopt_conflict_copy(1).succeeded());
        CHECK(later.text("theme") == TextView("second"));
        CHECK(later.changed());
        CHECK(saved(later));
    }
    CHECK(!exists(under(scratch.root, "preferences.1.settings")));
    CHECK(later.discard_conflict_copy(1).failed());
}

// ---- locks and processes ---------------------------------------------------------

void file_lock() {
    Scratch scratch("lock");
    const Path lock_path = under(scratch.root, ".lock");
    OperationResult<FileLock> held = FileLock::acquire(lock_path, Duration::milliseconds(0));
    CHECK(held.succeeded());
    CHECK(held.succeeded() && (*held.value()).held());
    // A second holder, even in this process, waits and gives up.
    const OperationResult<FileLock> second = FileLock::acquire(lock_path, Duration::milliseconds(30));
    CHECK(kind_of(second) == ErrorKind::limit_exceeded);
    CHECK(code_of(second) == HostCode::busy);
    if (held.succeeded()) (*held.value()).release();
    const OperationResult<FileLock> third = FileLock::acquire(lock_path, Duration::milliseconds(0));
    CHECK(third.succeeded());
    // identify tells a replaced file from the one read before.
    const Path file = under(scratch.root, "file");
    CHECK(write_text(file, "one"));
    const OperationResult<stxlib::FileIdentity> before = stxlib::identify(file);
    CHECK(write_text(file, "two"));
    const OperationResult<stxlib::FileIdentity> after = stxlib::identify(file);
    CHECK(before.succeeded() && after.succeeded());
    if (before.succeeded() && after.succeeded()) CHECK(*before.value() != *after.value());
    CHECK(kind_of(stxlib::identify(under(scratch.root, "missing"))) == ErrorKind::not_found);
}

constexpr int race_rounds = 25;

// The child's part of the race: opens the store in `directory` and saves
// its own key `key` race_rounds times, retrying a busy save.
int race_child(const char* const directory, const char* const key) {
    OperationResult<Path> root = Path::parse(view_of(directory));
    if (root.failed()) return 2;
    OperationResult<Schema> made = Schema::create("race", 1);
    if (made.failed()) return 3;
    Schema schema = static_cast<OperationResult<Schema>&&>(made).take_value();
    if (schema.add_integer("first", 0, 1000, 0).failed() || schema.add_integer("second", 0, 1000, 0).failed()) {
        return 4;
    }
    OperationResult<Store> opened = Store::open_in(*root.value(), schema);
    if (opened.failed()) return 5;
    Store store = static_cast<OperationResult<Store>&&>(opened).take_value();
    for (int round = 1; round <= race_rounds; ++round) {
        if (store.set_integer(view_of(key), round).failed()) return 6;
        bool done = false;
        for (int attempt = 0; attempt < 50 && !done; ++attempt) {
            const OperationResult<SaveOutcome> outcome = store.save();
            done = outcome.succeeded();
        }
        if (!done) return 7;
        if (store.conflict_count() != 0) return 8;
    }
    return 0;
}

std::string self_program() {
    OperationResult<Path> self = stxlib::current_executable();
    CHECK(self.succeeded());
    if (self.failed()) return std::string();
    const stxlib::PathText text = (*self.value()).text();
    std::string result(text.data(), text.count());
    return result;
}

void two_processes() {
    Scratch scratch("race");
    const std::string program = self_program();
    const stxlib::PathText root_text = scratch.root.text();
    const std::string root(root_text.data(), root_text.count());
    const std::string keys[] = {"first", "second"};
    std::vector<stxlib::Child> children;
    for (const std::string& key : keys) {
        const std::string words[] = {"race", root, key};
        std::vector<stxlib::PathText> views;
        for (const std::string& word : words) views.push_back(view_of(word));
        const stxlib::ChildRequest request{.program = view_of(program),
                                           .arguments = stxlib::View<stxlib::PathText>(views.data(), views.size())};
        OperationResult<stxlib::Child> started = stxlib::Child::start(request);
        CHECK(started.succeeded());
        if (started.succeeded()) children.push_back(static_cast<OperationResult<stxlib::Child>&&>(started).take_value());
    }
    for (stxlib::Child& child : children) {
        const OperationResult<stxlib::ChildEnd> end = child.wait(Duration::seconds(120));
        CHECK(end.succeeded());
        if (end.succeeded()) {
            const stxlib::u32* const status = (*end.value()).exit_status();
            CHECK(status != nullptr && *status == 0);
        }
    }
    OperationResult<Schema> made = Schema::create("race", 1);
    CHECK(made.succeeded());
    Schema schema = static_cast<OperationResult<Schema>&&>(made).take_value();
    CHECK(schema.add_integer("first", 0, 1000, 0).succeeded());
    CHECK(schema.add_integer("second", 0, 1000, 0).succeeded());
    Store reader = open_store(scratch.root, schema);
    // Each process changed only its own key: neither lost a save.
    CHECK(reader.integer("first") == race_rounds);
    CHECK(reader.integer("second") == race_rounds);
    CHECK(reader.diagnostics().count() == 0);
}

} // namespace

int main(const int argument_count, char** const arguments) {
    if (argument_count == 4 && std::strcmp(arguments[1], "race") == 0) return race_child(arguments[2], arguments[3]);
    schema_refusals();
    round_trip();
    unknown_keys_and_hand_edits();
    damage_and_recovery();
    migration_and_versions();
    two_stores();
    file_lock();
    two_processes();
    return stxlib_tests::finish("settings_tests");
}
