// The first-run wizard's logic: state machine, disc identification, hashes, download plan, unpacking, the install
// sequence and command lines. Discs are synthetic; nothing here depends on game data except one test that skips
// without the disc.
#include <gtest/gtest.h>
#include <miniz.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <map>
#include <random>

#include "assets.h"
#include "commands.h"
#include "disc_check.h"
#include "disc_location.h"
#include "dw3/texrep/pack_job.h"
#include "hd_install.h"
#include "process_runner.h"
#include "sha256.h"
#include "wizard_state.h"
#include "zip_extract.h"

using namespace dw3setup;
namespace fs = std::filesystem;

namespace {

std::string hash_of(const std::string& text) {
    return sha256_hex({reinterpret_cast<const uint8_t*>(text.data()), text.size()});
}

class TempDir {
public:
    TempDir() {
        static int counter = 0;
        path_ = fs::temp_directory_path() / ("dw3wizard-test-" + std::to_string(std::random_device{}()) + "-" + std::to_string(++counter));
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void put32(std::vector<uint8_t>& v, std::size_t off, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[off + i] = static_cast<uint8_t>(x >> (8 * i));
}

void add_record(std::vector<uint8_t>& sec, std::size_t& pos, const std::string& name, uint32_t lba, uint32_t size) {
    std::size_t len = 33 + name.size();
    if (len & 1) ++len;
    sec[pos] = static_cast<uint8_t>(len);
    put32(sec, pos + 2, lba);
    put32(sec, pos + 10, size);
    sec[pos + 32] = static_cast<uint8_t>(name.size());
    std::copy(name.begin(), name.end(), sec.begin() + pos + 33);
    pos += len;
}

struct SyntheticDisc {
    std::string boot_line = "BOOT = cdrom:\\SLUS_014.36;1\r\nTCB = 4\r\n";
    std::string exe_name = "SLUS_014.36;1";
    std::string exe_content = "synthetic executable";
    bool with_system_cnf = true;
    bool with_exe = true;
    bool with_volume = true;
};

// A raw 2352-byte-per-sector image: volume descriptor at 16, root directory at 20, SYSTEM.CNF at 21, executable at 22.
fs::path write_disc(const fs::path& dir, const std::string& name, const SyntheticDisc& d) {
    const std::size_t sectors = 30;
    std::vector<uint8_t> image(sectors * 2352, 0);
    auto data_at = [&](uint32_t lba) { return image.begin() + lba * 2352 + 24; };
    if (d.with_volume) {
        std::vector<uint8_t> pvd(2048, 0);
        pvd[0] = 1;
        std::copy_n("CD001", 5, pvd.begin() + 1);
        put32(pvd, 156 + 2, 20);
        put32(pvd, 156 + 10, 2048);
        std::copy(pvd.begin(), pvd.end(), data_at(16));
    }
    std::vector<uint8_t> root(2048, 0);
    std::size_t pos = 0;
    if (d.with_system_cnf) add_record(root, pos, "SYSTEM.CNF;1", 21, static_cast<uint32_t>(d.boot_line.size()));
    if (d.with_exe) add_record(root, pos, d.exe_name, 22, static_cast<uint32_t>(d.exe_content.size()));
    std::copy(root.begin(), root.end(), data_at(20));
    std::copy(d.boot_line.begin(), d.boot_line.end(), data_at(21));
    std::copy(d.exe_content.begin(), d.exe_content.end(), data_at(22));
    const fs::path path = dir / name;
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    return path;
}

ExpectedDisc expected_for(const SyntheticDisc& d) { return {"SLUS-01436", hash_of(d.exe_content)}; }

}  // namespace

// ---- SHA-256 -------------------------------------------------------------------------------------------------------

TEST(Sha256, KnownVectors) {
    EXPECT_EQ(hash_of(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(hash_of("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(hash_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Sha256, IncrementalMatchesOneShot) {
    const std::string text(1000, 'a');
    Sha256 h;
    for (std::size_t i = 0; i < text.size(); i += 37)
        h.update({reinterpret_cast<const uint8_t*>(text.data()) + i, std::min<std::size_t>(37, text.size() - i)});
    EXPECT_EQ(h.finish_hex(), hash_of(text));
}

TEST(Sha256, MillionAs) {
    const std::string text(1000000, 'a');
    EXPECT_EQ(hash_of(text), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, File) {
    TempDir dir;
    const fs::path file = dir.path() / "f.bin";
    std::ofstream(file, std::ios::binary) << "abc";
    std::string hex;
    ASSERT_TRUE(sha256_file(file, hex));
    EXPECT_EQ(hex, hash_of("abc"));
    EXPECT_FALSE(sha256_file(dir.path() / "missing", hex));
}

// ---- Disc identification ---------------------------------------------------------------------------------------------

TEST(DiscCheck, SerialFromSystemCnf) {
    EXPECT_EQ(serial_from_system_cnf("BOOT = cdrom:\\SLUS_014.36;1\r\nTCB = 4\r\n"), "SLUS-01436");
    EXPECT_EQ(serial_from_system_cnf("TCB = 4\nboot=cdrom:SLES_123.45;1\n"), "SLES-12345");
    EXPECT_EQ(boot_file_from_system_cnf("BOOT = cdrom:\\SLUS_014.36;1"), "SLUS_014.36");
    EXPECT_EQ(serial_from_system_cnf("TCB = 4\n"), "");
    EXPECT_EQ(serial_from_system_cnf(""), "");
}

TEST(DiscCheck, RegionOfSerial) {
    EXPECT_EQ(region_of_serial("SLUS-01436"), "USA");
    EXPECT_EQ(region_of_serial("SCUS-94163"), "USA");
    EXPECT_EQ(region_of_serial("SLES-02380"), "Europe");
    EXPECT_EQ(region_of_serial("SLPS-03000"), "Japan");
    EXPECT_EQ(region_of_serial("ABCD-00000"), "");
    EXPECT_EQ(region_of_serial("SL"), "");
}

TEST(DiscCheck, AcceptsMatchingDisc) {
    TempDir dir;
    SyntheticDisc d;
    const DiscCheck r = check_disc(write_disc(dir.path(), "good.bin", d), expected_for(d));
    EXPECT_TRUE(r.ok()) << r.message;
    EXPECT_EQ(r.serial, "SLUS-01436");
}

TEST(DiscCheck, AcceptsCueSheet) {
    TempDir dir;
    SyntheticDisc d;
    write_disc(dir.path(), "game.bin", d);
    const fs::path cue = dir.path() / "game.cue";
    std::ofstream(cue) << "FILE \"game.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n";
    EXPECT_TRUE(check_disc(cue, expected_for(d)).ok());
}

TEST(DiscCheck, RejectsOtherRegion) {
    TempDir dir;
    SyntheticDisc d;
    d.boot_line = "BOOT = cdrom:\\SLES_123.45;1\n";
    d.exe_name = "SLES_123.45;1";
    const DiscCheck r = check_disc(write_disc(dir.path(), "eu.bin", d), expected_for(d));
    EXPECT_EQ(r.status, DiscStatus::WrongRegion);
    EXPECT_EQ(r.serial, "SLES-12345");
    EXPECT_NE(r.message.find("Europe"), std::string::npos);
    EXPECT_NE(r.message.find("SLUS-01436"), std::string::npos);
}

TEST(DiscCheck, RejectsOtherUsaGame) {
    TempDir dir;
    SyntheticDisc d;
    d.boot_line = "BOOT = cdrom:\\SLUS_000.01;1\n";
    d.exe_name = "SLUS_000.01;1";
    const DiscCheck r = check_disc(write_disc(dir.path(), "other.bin", d), expected_for(d));
    EXPECT_EQ(r.status, DiscStatus::WrongGame);
}

TEST(DiscCheck, RejectsChangedExecutable) {
    TempDir dir;
    SyntheticDisc d;
    ExpectedDisc expected = expected_for(d);
    d.exe_content = "patched executable";
    const DiscCheck r = check_disc(write_disc(dir.path(), "patched.bin", d), expected);
    EXPECT_EQ(r.status, DiscStatus::ModifiedExecutable);
}

TEST(DiscCheck, RejectsBrokenImages) {
    TempDir dir;
    SyntheticDisc d;
    EXPECT_EQ(check_disc(dir.path() / "nothing.cue", expected_for(d)).status, DiscStatus::NotFound);

    const fs::path cooked = dir.path() / "cooked.iso";
    std::ofstream(cooked, std::ios::binary) << std::string(2048 * 4, 'x');
    EXPECT_EQ(check_disc(cooked, expected_for(d)).status, DiscStatus::UnsupportedFormat);

    SyntheticDisc no_volume;
    no_volume.with_volume = false;
    EXPECT_EQ(check_disc(write_disc(dir.path(), "novol.bin", no_volume), expected_for(d)).status, DiscStatus::NotPlayStation);

    SyntheticDisc no_cnf;
    no_cnf.with_system_cnf = false;
    EXPECT_EQ(check_disc(write_disc(dir.path(), "nocnf.bin", no_cnf), expected_for(d)).status, DiscStatus::NotPlayStation);

    SyntheticDisc no_exe;
    no_exe.with_exe = false;
    EXPECT_EQ(check_disc(write_disc(dir.path(), "noexe.bin", no_exe), expected_for(d)).status, DiscStatus::NotPlayStation);
}

TEST(DiscCheck, RetailConstantsAreWellFormed) {
    EXPECT_EQ(retail_usa().serial, "SLUS-01436");
    EXPECT_EQ(retail_usa().exe_sha256.size(), 64u);
}

#ifdef DW3_GAME_DIR
TEST(DiscCheck, RealDisc) {
    const fs::path cue = fs::path(DW3_GAME_DIR) / "Digimon World 3 (USA).cue";
    if (!fs::exists(cue)) GTEST_SKIP() << "no disc image";
    const DiscCheck r = check_disc(cue);
    EXPECT_TRUE(r.ok()) << r.message;
}
#endif

TEST(DiscLocation, Order) {
    auto valid = [](const std::string& p) { return p != "bad"; };
    EXPECT_EQ(locate_disc("flag.cue", "stored.cue", "default.cue", valid), "flag.cue");
    EXPECT_EQ(locate_disc("", "stored.cue", "default.cue", valid), "stored.cue");
    EXPECT_EQ(locate_disc("", "bad", "default.cue", valid), "default.cue");
    EXPECT_EQ(locate_disc("", "", "default.cue", valid), "default.cue");
    EXPECT_FALSE(locate_disc("", "bad", "bad", valid).has_value());
    EXPECT_FALSE(locate_disc("", "", "", valid).has_value());
}

// ---- Platform assets ---------------------------------------------------------------------------------------------------

TEST(Assets, UpscalerPerPlatform) {
    using dw3::platform::Arch;
    using dw3::platform::Os;
    const std::string base = "https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan/releases/download/v0.2.0/";
    auto win = upscaler_asset(Os::Windows, Arch::X86_64);
    ASSERT_TRUE(win);
    EXPECT_EQ(win->url, base + "realesrgan-ncnn-vulkan-v0.2.0-windows.zip");
    EXPECT_TRUE(upscaler_asset(Os::Windows, Arch::Arm64));
    auto mac = upscaler_asset(Os::MacOS, Arch::Arm64);
    ASSERT_TRUE(mac);
    EXPECT_EQ(mac->url, base + "realesrgan-ncnn-vulkan-v0.2.0-macos.zip");
    auto linux_x64 = upscaler_asset(Os::Linux, Arch::X86_64);
    ASSERT_TRUE(linux_x64);
    EXPECT_EQ(linux_x64->url, base + "realesrgan-ncnn-vulkan-v0.2.0-ubuntu.zip");
    EXPECT_FALSE(upscaler_asset(Os::Linux, Arch::Arm64));
    for (auto os : {Os::Windows, Os::MacOS, Os::Linux}) {
        auto a = upscaler_asset(os, Arch::X86_64);
        ASSERT_TRUE(a);
        EXPECT_EQ(a->sha256.size(), 64u);
        EXPECT_TRUE(a->is_archive);
        EXPECT_GT(a->size_bytes, 0u);
    }
}

TEST(Assets, PlanListsUpscalerThenModels) {
    const DownloadPlan plan = plan_for(dw3::platform::Os::Linux, dw3::platform::Arch::X86_64);
    ASSERT_EQ(plan.assets.size(), 3u);
    EXPECT_TRUE(plan.assets[0].is_archive);
    EXPECT_EQ(plan.assets[1].file_name, std::string(kModelName) + ".param");
    EXPECT_EQ(plan.assets[2].file_name, std::string(kModelName) + ".bin");
    EXPECT_TRUE(plan.unsupported.empty());
    const DownloadPlan arm = plan_for(dw3::platform::Os::Linux, dw3::platform::Arch::Arm64);
    EXPECT_FALSE(arm.unsupported.empty());
    EXPECT_FALSE(plan_problem(arm).empty());
}

TEST(Assets, UnsetValuesFailClearly) {
    DownloadPlan plan = plan_for(dw3::platform::Os::Linux, dw3::platform::Arch::X86_64);
    EXPECT_EQ(plan_problem(plan), "");  // the shipped plan is complete
    for (Asset& a : plan.assets) {
        a.url = "https://example.test/" + a.file_name;
        a.sha256 = std::string(64, 'a');
    }
    EXPECT_EQ(plan_problem(plan), "");
    plan.assets[2].sha256.clear();
    EXPECT_NE(plan_problem(plan).find(plan.assets[2].file_name), std::string::npos);
    plan.assets[2].sha256 = std::string(64, 'a');
    plan.assets[1].url = "https://example.invalid/PLACEHOLDER/x";
    EXPECT_NE(plan_problem(plan).find("address"), std::string::npos);
}

TEST(Assets, SizesAndPaths) {
    DownloadPlan plan = plan_for(dw3::platform::Os::Windows, dw3::platform::Arch::X86_64);
    bool known = true;
    EXPECT_EQ(total_download_bytes(plan, known), plan.assets[0].size_bytes);
    EXPECT_FALSE(known);  // the model files have no recorded size yet
    EXPECT_EQ(format_bytes(3 * 1024 * 1024 / 2), "1.5 MB");
    EXPECT_EQ(format_bytes(100), "1 KB");
    const fs::path data = "data";
    EXPECT_EQ(upscaler_executable(data, dw3::platform::Os::Windows), data / "tools" / "realesrgan-ncnn-vulkan.exe");
    EXPECT_EQ(upscaler_executable(data, dw3::platform::Os::Linux), data / "tools" / "realesrgan-ncnn-vulkan");
    EXPECT_EQ(models_dir(data), data / "tools" / "models");
    EXPECT_EQ(texture_pack_dir(data, "p"), data / "texture-packs" / "p");
}

// ---- Command lines -----------------------------------------------------------------------------------------------------

TEST(Commands, CurlDownload) {
    const auto cmd = curl_download_command("https://example.test/a b.zip", fs::path("dir") / "a.part");
    ASSERT_GE(cmd.size(), 4u);
    EXPECT_EQ(cmd.front(), "curl");
    EXPECT_EQ(cmd.back(), "https://example.test/a b.zip");  // one argument, never split or quoted
    EXPECT_EQ(cmd[cmd.size() - 2], "--");
    auto has = [&](const char* flag) { return std::find(cmd.begin(), cmd.end(), flag) != cmd.end(); };
    EXPECT_TRUE(has("--fail"));
    EXPECT_TRUE(has("--location"));
    const auto out = std::find(cmd.begin(), cmd.end(), "--output");
    ASSERT_NE(out, cmd.end());
    EXPECT_EQ(*(out + 1), (fs::path("dir") / "a.part").string());
    EXPECT_EQ(curl_version_command(), (std::vector<std::string>{"curl", "--version"}));
}

TEST(Commands, CurlExitDescriptions) {
    EXPECT_NE(describe_curl_exit(6).find("resolved"), std::string::npos);
    EXPECT_NE(describe_curl_exit(22).find("HTTP"), std::string::npos);
    EXPECT_NE(describe_curl_exit(99).find("99"), std::string::npos);
}

TEST(Commands, UpscalerCommand) {
    dw3::texrep::PackJob job;
    job.upscaler = "tools/up";
    job.models = "tools/models";
    job.model = "m";
    job.model_scale = 4;
    const auto cmd = dw3::texrep::upscaler_command(job, "in", "out");
    const std::vector<std::string> expected = {"tools/up", "-i", "in", "-o", "out", "-n", "m", "-s", "4", "-m", "tools/models", "-f", "png"};
    EXPECT_EQ(cmd, expected);
    job.gpu = "1";
    const auto with_gpu = dw3::texrep::upscaler_command(job, "in", "out");
    ASSERT_EQ(with_gpu.size(), expected.size() + 2);
    EXPECT_EQ(with_gpu[with_gpu.size() - 2], "-g");
    EXPECT_EQ(with_gpu.back(), "1");
}

// ---- State machine -------------------------------------------------------------------------------------------------------

TEST(WizardState, HappyPathWithSkip) {
    WizardState w;
    EXPECT_EQ(w.step(), Step::Welcome);
    EXPECT_FALSE(w.can_go_back());
    w.next();
    EXPECT_EQ(w.step(), Step::ChooseDisc);
    EXPECT_FALSE(w.can_continue());  // no path yet
    w.next();
    EXPECT_EQ(w.step(), Step::ChooseDisc);
    w.set_disc_path("game.cue");
    w.next();
    EXPECT_EQ(w.step(), Step::Verify);
    EXPECT_TRUE(w.verify_pending());
    EXPECT_FALSE(w.can_continue());
    EXPECT_FALSE(w.can_go_back());  // not while checking
    DiscCheck ok;
    ok.status = DiscStatus::Ok;
    w.finish_verify(ok);
    EXPECT_TRUE(w.can_continue());
    w.next();
    EXPECT_EQ(w.step(), Step::HdOffer);
    w.skip_hd();
    EXPECT_EQ(w.step(), Step::Done);
    w.press_finish();
    const WizardOutcome o = w.outcome();
    EXPECT_TRUE(o.completed);
    EXPECT_EQ(o.disc_path, "game.cue");
    EXPECT_FALSE(o.hd_backgrounds);
}

TEST(WizardState, FailedVerifyBlocksAndReturns) {
    WizardState w;
    w.next();
    w.set_disc_path("wrong.cue");
    w.next();
    DiscCheck bad;
    bad.status = DiscStatus::WrongRegion;
    bad.message = "no";
    w.finish_verify(bad);
    EXPECT_FALSE(w.can_continue());
    w.next();
    EXPECT_EQ(w.step(), Step::Verify);
    EXPECT_TRUE(w.can_go_back());
    w.back();
    EXPECT_EQ(w.step(), Step::ChooseDisc);
    EXPECT_EQ(w.disc_path(), "wrong.cue");
    w.set_disc_path("right.cue");
    w.next();
    EXPECT_TRUE(w.verify_pending());  // a new path is checked again
}

TEST(WizardState, ChangingPathDiscardsCheck) {
    WizardState w;
    w.next();
    w.set_disc_path("a.cue");
    w.next();
    DiscCheck ok;
    ok.status = DiscStatus::Ok;
    w.finish_verify(ok);
    w.back();
    w.set_disc_path("b.cue");
    w.next();
    EXPECT_FALSE(w.can_continue());
}

TEST(WizardState, HdAcceptSuccess) {
    WizardState w;
    w.next();
    w.set_disc_path("g.cue");
    w.next();
    DiscCheck ok;
    ok.status = DiscStatus::Ok;
    w.finish_verify(ok);
    w.next();
    w.accept_hd();
    EXPECT_EQ(w.step(), Step::HdWork);
    EXPECT_EQ(w.hd_phase(), HdPhase::Preparing);
    w.set_hd_phase(HdPhase::Downloading);
    w.set_hd_phase(HdPhase::Generating);
    EXPECT_EQ(w.hd_phase(), HdPhase::Generating);
    w.finish_hd();
    EXPECT_EQ(w.step(), Step::Done);
    w.press_finish();
    EXPECT_TRUE(w.outcome().hd_backgrounds);
}

TEST(WizardState, HdFailureRetryAndSkip) {
    WizardState w;
    w.next();
    w.set_disc_path("g.cue");
    w.next();
    DiscCheck ok;
    ok.status = DiscStatus::Ok;
    w.finish_verify(ok);
    w.next();
    w.accept_hd();
    w.fail_hd("curl missing");
    EXPECT_EQ(w.hd_phase(), HdPhase::Failed);
    EXPECT_EQ(w.hd_error(), "curl missing");
    w.finish_hd();  // ignored while failed
    EXPECT_EQ(w.step(), Step::HdWork);
    w.retry_hd();
    EXPECT_EQ(w.hd_phase(), HdPhase::Preparing);
    EXPECT_TRUE(w.hd_error().empty());
    w.fail_hd("again");
    w.skip_hd();
    EXPECT_EQ(w.step(), Step::Done);
    w.press_finish();
    EXPECT_FALSE(w.outcome().hd_backgrounds);
}

TEST(WizardState, OutOfOrderCallsAreIgnored) {
    WizardState w;
    w.accept_hd();
    w.skip_hd();
    w.finish_hd();
    w.fail_hd("x");
    w.press_finish();
    EXPECT_EQ(w.step(), Step::Welcome);
    EXPECT_FALSE(w.outcome().completed);
    w.quit();
    EXPECT_TRUE(w.quit_requested());
    EXPECT_FALSE(w.outcome().completed);
}

// ---- Unpacking ---------------------------------------------------------------------------------------------------------

namespace {

fs::path make_zip(const fs::path& path, const std::map<std::string, std::string>& files) {
    mz_zip_archive zip{};
    EXPECT_TRUE(mz_zip_writer_init_file(&zip, path.string().c_str(), 0));
    for (const auto& [name, content] : files)
        EXPECT_TRUE(mz_zip_writer_add_mem(&zip, name.c_str(), content.data(), content.size(), MZ_DEFAULT_LEVEL));
    EXPECT_TRUE(mz_zip_writer_finalize_archive(&zip));
    mz_zip_writer_end(&zip);
    return path;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST(ZipExtract, StripsSingleTopFolder) {
    TempDir dir;
    const fs::path zip = make_zip(dir.path() / "a.zip", {{"top/tool", "binary"}, {"top/sub/data.txt", "text"}});
    std::string error;
    ASSERT_TRUE(extract_zip(zip, dir.path() / "out", error)) << error;
    EXPECT_EQ(slurp(dir.path() / "out" / "tool"), "binary");
    EXPECT_EQ(slurp(dir.path() / "out" / "sub" / "data.txt"), "text");
}

TEST(ZipExtract, KeepsLayoutWithoutSingleTopFolder) {
    TempDir dir;
    const fs::path zip = make_zip(dir.path() / "a.zip", {{"tool", "binary"}, {"models/m.bin", "m"}});
    std::string error;
    ASSERT_TRUE(extract_zip(zip, dir.path() / "out", error)) << error;
    EXPECT_EQ(slurp(dir.path() / "out" / "tool"), "binary");
    EXPECT_EQ(slurp(dir.path() / "out" / "models" / "m.bin"), "m");
}

TEST(ZipExtract, RefusesPathsOutsideDestination) {
    TempDir dir;
    const fs::path zip = make_zip(dir.path() / "evil.zip", {{"top/../../evil.txt", "x"}});
    std::string error;
    EXPECT_FALSE(extract_zip(zip, dir.path() / "out", error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(fs::exists(dir.path() / "evil.txt"));
}

TEST(ZipExtract, ReportsBadArchive) {
    TempDir dir;
    std::ofstream(dir.path() / "bad.zip") << "not a zip";
    std::string error;
    EXPECT_FALSE(extract_zip(dir.path() / "bad.zip", dir.path() / "out", error));
    EXPECT_FALSE(error.empty());
}

// ---- Install sequence -----------------------------------------------------------------------------------------------------

namespace {

// Stands in for curl and the pack generator: "downloads" are served from memory.
struct FakeSystem {
    std::map<std::string, std::string> files;  // url -> content
    bool curl_installed = true;
    int curl_download_exit = 0;
    int downloads = 0;
    int curl_probes = 0;
    bool generated = false;
    fs::path generated_pack;
    bool generate_ok = true;

    int run(const std::vector<std::string>& cmd, const std::atomic<bool>&) {
        if (!curl_installed) return kProcessNotStarted;
        if (cmd.size() == 2 && cmd[1] == "--version") {
            ++curl_probes;
            return 0;
        }
        ++downloads;
        if (curl_download_exit != 0) return curl_download_exit;
        const auto out = std::find(cmd.begin(), cmd.end(), "--output");
        const std::string& url = cmd.back();
        std::ofstream(*(out + 1), std::ios::binary) << files.at(url);
        return 0;
    }
};

struct InstallFixture {
    TempDir dir;
    FakeSystem system;
    HdConfig config;
    std::string zip_content;

    InstallFixture() {
        const fs::path zip = make_zip(dir.path() / "src.zip", {{"up/realesrgan-ncnn-vulkan", "exe"}, {"up/README.md", "r"}});
        zip_content = slurp(zip);
        config.user_data = dir.path() / "data";
        config.disc = dir.path() / "disc.cue";
        config.os = dw3::platform::Os::Linux;
        Asset up{"up.zip", "https://example.test/up.zip", hash_of(zip_content), zip_content.size(), true};
        Asset param{"m.param", "https://example.test/m.param", hash_of("param"), 5, false};
        Asset bin{"m.bin", "https://example.test/m.bin", hash_of("bin"), 3, false};
        config.plan.assets = {up, param, bin};
        system.files = {{up.url, zip_content}, {param.url, "param"}, {bin.url, "bin"}};
        config.run = [this](const std::vector<std::string>& cmd, const std::atomic<bool>& cancel) { return system.run(cmd, cancel); };
        config.generate = [this](const fs::path& upscaler, const fs::path& models, const fs::path& pack,
                                 const std::function<void(std::size_t, std::size_t)>& progress, const std::atomic<bool>&, std::string& error) {
            system.generated = true;
            system.generated_pack = pack;
            EXPECT_TRUE(fs::exists(upscaler));
            EXPECT_TRUE(fs::exists(models / "m.param"));
            progress(1, 2);
            error = "generator failed";
            return system.generate_ok;
        };
    }
};

}  // namespace

TEST(HdInstall, DownloadsVerifiesUnpacksAndGenerates) {
    InstallFixture f;
    HdInstaller installer(f.config);
    const HdResult r = installer.run();
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(f.system.downloads, 3);
    EXPECT_TRUE(fs::exists(f.config.user_data / "tools" / "realesrgan-ncnn-vulkan"));
    EXPECT_EQ(slurp(f.config.user_data / "tools" / "models" / "m.param"), "param");
    EXPECT_TRUE(f.system.generated);
    EXPECT_EQ(f.system.generated_pack, f.config.user_data / "texture-packs" / kPackName);
#ifndef _WIN32
    EXPECT_NE(fs::status(f.config.user_data / "tools" / "realesrgan-ncnn-vulkan").permissions() & fs::perms::owner_exec, fs::perms::none);
#endif
    EXPECT_EQ(installer.snapshot().phase, HdPhase::Generating);
}

TEST(HdInstall, WrongHashFailsAndDeletesFile) {
    InstallFixture f;
    f.system.files["https://example.test/m.bin"] = "tampered";
    HdInstaller installer(f.config);
    const HdResult r = installer.run();
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("m.bin"), std::string::npos);
    EXPECT_NE(r.error.find("SHA-256"), std::string::npos);
    EXPECT_FALSE(fs::exists(f.config.user_data / "downloads" / "m.bin"));
    EXPECT_FALSE(fs::exists(f.config.user_data / "downloads" / "m.bin.part"));
    EXPECT_FALSE(f.system.generated);
}

TEST(HdInstall, UnsetHashRefusesBeforeDownloading) {
    InstallFixture f;
    f.config.plan.assets[2].sha256.clear();
    HdInstaller installer(f.config);
    const HdResult r = installer.run();
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("SHA-256"), std::string::npos);
    EXPECT_EQ(f.system.downloads, 0);
    EXPECT_EQ(f.system.curl_probes, 0);
}

TEST(HdInstall, MissingCurlOffersManualDownload) {
    InstallFixture f;
    f.system.curl_installed = false;
    HdInstaller installer(f.config);
    const HdResult r = installer.run();
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.manual_download);
    EXPECT_NE(r.error.find("curl"), std::string::npos);
}

TEST(HdInstall, FilesPlacedByHandNeedNoCurl) {
    InstallFixture f;
    f.system.curl_installed = false;
    fs::create_directories(f.config.user_data / "downloads");
    for (const Asset& a : f.config.plan.assets) std::ofstream(f.config.user_data / "downloads" / a.file_name, std::ios::binary) << f.system.files[a.url];
    HdInstaller installer(f.config);
    const HdResult r = installer.run();
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_EQ(f.system.downloads, 0);
}

TEST(HdInstall, DownloadFailureReportsReason) {
    InstallFixture f;
    f.system.curl_download_exit = 6;
    HdInstaller installer(f.config);
    const HdResult r = installer.run();
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.manual_download);
    EXPECT_NE(r.error.find("resolved"), std::string::npos);
}

TEST(HdInstall, GeneratorFailureIsReported) {
    InstallFixture f;
    f.system.generate_ok = false;
    HdInstaller installer(f.config);
    const HdResult r = installer.run();
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "generator failed");
}

TEST(HdInstall, CancelStopsBeforeWork) {
    InstallFixture f;
    HdInstaller installer(f.config);
    installer.cancel();
    const HdResult r = installer.run();
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.error, "Cancelled.");
    EXPECT_EQ(f.system.downloads, 0);
}

TEST(WizardState, PrepareStepComesAfterVerifyWhenRequired) {
    WizardState w;
    w.require_prepare(true);
    w.next();
    w.set_disc_path("game.cue");
    w.next();
    DiscCheck ok;
    ok.status = DiscStatus::Ok;
    w.finish_verify(ok);
    w.next();
    EXPECT_EQ(w.step(), Step::PrepareWork);
    EXPECT_FALSE(w.can_continue());
    EXPECT_FALSE(w.can_go_back());
    w.fail_prepare("compiler missing");
    EXPECT_TRUE(w.prepare_failed());
    EXPECT_EQ(w.prepare_error(), "compiler missing");
    w.finish_prepare();  // a failed attempt cannot be finished
    EXPECT_EQ(w.step(), Step::PrepareWork);
    w.retry_prepare();
    EXPECT_FALSE(w.prepare_failed());
    w.finish_prepare();
    EXPECT_EQ(w.step(), Step::HdOffer);
    w.skip_hd();
    w.press_finish();
    EXPECT_TRUE(w.outcome().prepared);
}

TEST(WizardState, ResumeAtPrepareSkipsTheDiscSteps) {
    WizardState w;
    w.resume_at_prepare("game.cue");
    EXPECT_EQ(w.step(), Step::PrepareWork);
    EXPECT_EQ(w.disc_path(), "game.cue");
    w.finish_prepare();
    EXPECT_EQ(w.step(), Step::HdOffer);
    EXPECT_TRUE(w.can_go_back());
}

TEST(WizardState, NoPrepareStepWhenNotRequired) {
    WizardState w;
    w.next();
    w.set_disc_path("game.cue");
    w.next();
    DiscCheck ok;
    ok.status = DiscStatus::Ok;
    w.finish_verify(ok);
    w.next();
    EXPECT_EQ(w.step(), Step::HdOffer);
    w.skip_hd();
    w.press_finish();
    EXPECT_FALSE(w.outcome().prepared);
}
