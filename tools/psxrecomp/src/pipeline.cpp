#include "psxrecomp/pipeline.h"

#include <algorithm>
#include <fstream>

#include "psxdisc/disc.h"
#include "psxdisc/iso9660.h"
#include "psxdisc/psxexe.h"

namespace fs = std::filesystem;

namespace psxrecomp {

namespace {

constexpr uint32_t kHiddenRootLba = 473;  // the AAA tree, not linked from the ISO9660 root

std::vector<uint8_t> read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

bool matches(const std::string& name, const std::string& pre, const std::string& post) {
    return name.size() >= pre.size() + post.size() && name.starts_with(pre) && name.ends_with(post);
}

// "dir/PRE*POST" -> the entries of `names` (full paths) that match, sorted.
std::vector<std::string> filter_glob(const std::vector<std::string>& names, const std::string& pattern) {
    const fs::path dir = fs::path(pattern).parent_path();
    const std::string fname = fs::path(pattern).filename().string();
    const auto star = fname.find('*');
    const std::string pre = fname.substr(0, star), post = fname.substr(star + 1);
    std::vector<std::string> out;
    for (const auto& n : names)
        if (fs::path(n).parent_path() == dir && matches(fs::path(n).filename().string(), pre, post)) out.push_back(n);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

bool write_if_changed(const fs::path& p, const std::string& text) {
    // Avoid touching unchanged files so incremental builds stay fast.
    {
        std::ifstream in(p, std::ios::binary);
        if (in) {
            std::string old{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            if (old == text) return false;
        }
    }
    std::ofstream(p, std::ios::binary) << text;
    return true;
}

FileSource directory_source(const fs::path& root) {
    FileSource s;
    s.read = [root](const std::string& path) { return read_all(root / path); };
    s.expand = [root](const std::string& pattern) {
        if (pattern.find('*') == std::string::npos) return std::vector<std::string>{pattern};
        const fs::path dir = fs::path(pattern).parent_path();
        std::vector<std::string> names;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(root / dir, ec)) names.push_back((dir / e.path().filename()).generic_string());
        return filter_glob(names, pattern);
    };
    return s;
}

FileSource disc_source(psxdisc::Disc& disc) {
    auto files = std::make_shared<std::vector<psxdisc::FileEntry>>();
    if (auto root = psxdisc::read_root(disc)) *files = psxdisc::walk(disc, root->lba, root->size);
    auto hidden = psxdisc::walk(disc, kHiddenRootLba, psxdisc::kForm1DataSize, "AAA/");
    files->insert(files->end(), hidden.begin(), hidden.end());
    FileSource s;
    s.read = [&disc, files](const std::string& path) -> std::vector<uint8_t> {
        for (const auto& f : *files)
            if (f.path == path) {
                auto data = disc.read_file(f.lba, f.size);
                return data.size() == f.size ? data : std::vector<uint8_t>{};
            }
        return {};
    };
    s.expand = [files](const std::string& pattern) {
        if (pattern.find('*') == std::string::npos) return std::vector<std::string>{pattern};
        std::vector<std::string> names;
        for (const auto& f : *files) names.push_back(f.path);
        return filter_glob(names, pattern);
    };
    return s;
}

bool Game::load(const toml::table& cfg, const FileSource& source, std::string& error) {
    jobs_.clear();
    results_.clear();
    tables_.clear();
    {
        auto file = source.read(cfg["exe"]["path"].value_or(std::string("SLUS_014.36")));
        auto exe = psxdisc::parse_psxexe(file);
        if (!exe) {
            error = "cannot load the game executable";
            return false;
        }
        SegmentJob j;
        j.seg = {"MAIN", exe->text_addr, exe->text};
        j.cfg.entries.push_back(exe->pc);
        j.cfg.code_end = cfg["exe"]["code_end"].value_or(0u);
        if (auto* arr = cfg["exe"]["functions"].as_array())
            for (auto& v : *arr) j.cfg.extra_functions.insert(v.value_or(0u));
        jobs_.push_back(std::move(j));
    }
    if (auto* slots = cfg["overlay_slot"].as_array()) {
        for (auto& node : *slots) {
            auto& slot = *node.as_table();
            uint32_t base = slot["base"].value_or(0u);
            std::vector<std::string> files;
            if (auto* a = slot["files"].as_array())
                for (auto& v : *a) files.push_back(v.value_or(std::string()));
            if (auto* a = slot["files_glob"].as_array())
                for (auto& v : *a)
                    for (auto& f : source.expand(v.value_or(std::string()))) files.push_back(f);
            for (const auto& f : files) {
                SegmentJob j;
                j.seg = {fs::path(f).stem().string(), base, source.read(f)};
                if (j.seg.bytes.empty()) {
                    error = "missing overlay " + f;
                    return false;
                }
                jobs_.push_back(std::move(j));
            }
        }
    }
    return true;
}

bool Game::analyze(const Progress& progress) {
    results_.clear();
    tables_.clear();
    for (std::size_t k = 0; k < jobs_.size(); ++k) {
        if (progress.cancelled()) return false;
        results_.push_back(psxrecomp::analyze(jobs_[k].seg, jobs_[k].cfg));
        if (progress.report) progress.report("Analysing", k + 1, jobs_.size());
    }
    tables_.reserve(jobs_.size());
    for (std::size_t k = 0; k < jobs_.size(); ++k) {
        uint32_t extent = jobs_[k].cfg.code_end ? jobs_[k].cfg.code_end - jobs_[k].seg.base : 0;
        tables_.push_back({&jobs_[k].seg, &results_[k].functions, extent});
    }
    return true;
}

EmitStats Game::statistics() const {
    EmitStats st;
    for (const auto& r : results_)
        for (const auto& [a, f] : r.functions) {
            ++st.functions;
            st.jump_tables += f.jump_tables.size();
            st.pointer_jumps += f.pointer_jumps.size();
            st.unresolved_jumps += f.indirect_jumps.size();
            st.functions_with_errors += !f.errors.empty();
        }
    SlotRelations slots = analyze_slots(tables_);
    for (const auto& l : slots.stale_if) st.stale_links += l.size(), st.stale_overlays += !l.empty();
    st.ambiguous_overlays = slots.ambiguous;
    return st;
}

bool Game::emit(const fs::path& out_dir, const Progress& progress, EmitStats& stats, std::string& error) {
    stats = statistics();
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    if (ec) {
        error = "cannot create " + out_dir.string();
        return false;
    }
    std::string manifest;
    for (std::size_t k = 0; k < jobs_.size(); ++k) {
        if (progress.cancelled()) return false;
        EmitContext c;
        c.seg = &jobs_[k].seg;
        c.local = &results_[k].functions;
        c.main = &results_[0].functions;  // job 0 is always MAIN
        for (auto& f : emit_segment(c, 64, &stats.warnings)) {
            stats.files_changed += write_if_changed(out_dir / f.name, f.text);
            manifest += f.name + "\n";
            stats.sources.push_back(f.name);
            ++stats.files;
        }
        if (progress.report) progress.report("Writing code", k + 1, jobs_.size());
    }
    auto t = emit_tables(tables_, &stats.warnings);
    stats.files_changed += write_if_changed(out_dir / t.name, t.text);
    manifest += t.name + "\n";
    stats.sources.push_back(t.name);
    ++stats.files;
    write_if_changed(out_dir / "sources.txt", manifest);
    return true;
}

}  // namespace psxrecomp
