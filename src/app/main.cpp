// datadiver: county records in, one schema out, in your terminal.
//
//   datadiver                       interactive shell (paste URLs, stay in)
//   datadiver align URL             one-shot: fetch, classify, match, extract
//   datadiver watch URL [--interval N]
//                                   re-fetch on an interval; show drift and
//                                   healing live as the page changes
//   datadiver ingest (--all | ID)   stateful ingestion into var/ (events,
//                                   properties, baselines, healing)
//   datadiver sources               list configured sources
//   datadiver train                 retrain the classifier from data/corpus
//
// Common flags: --schema FILE (default data/schema.json), --model FILE,
// --state DIR, --seeds FILE.

#include "commands.hpp"
#include "render.hpp"

#include "dd/core/core.hpp"
#include "dd/engine/heal.hpp"
#include "dd/engine/pipeline.hpp"
#include "dd/engine/schema.hpp"
#include "dd/engine/store.hpp"
#include "dd/ml/classify.hpp"
#include "dd/net/fetch.hpp"
#include "dd/parse/document.hpp"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

using dd::render::meter;
using dd::render::paint;
using dd::render::section;
using dd::render::stamp;

volatile std::sig_atomic_t g_interrupted = 0;
void handle_interrupt(int) { g_interrupted = 1; }

struct Args {
    std::string command;
    std::vector<std::string> positional;
    std::map<std::string, std::string> flags;
};

Args parse_args(int argc, char** argv) {
    Args args;
    if (argc >= 2) args.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--", 0) == 0) {
            if (arg == "--all") {
                args.flags["all"] = "true";
            } else if (i + 1 < argc) {
                args.flags[arg.substr(2)] = argv[++i];
            } else {
                args.flags[arg.substr(2)] = "";
            }
        } else {
            args.positional.push_back(arg);
        }
    }
    return args;
}

std::string flag(const Args& args, const char* name, const char* fallback) {
    const auto it = args.flags.find(name);
    return it == args.flags.end() ? fallback : it->second;
}

std::string fmt_ms(double ms) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f ms", ms);
    return buffer;
}

std::string fmt_bytes(std::int64_t n) {
    char buffer[32];
    if (n >= 1 << 20) std::snprintf(buffer, sizeof(buffer), "%.1f MB", n / 1048576.0);
    else if (n >= 1 << 10) std::snprintf(buffer, sizeof(buffer), "%.1f KB", n / 1024.0);
    else std::snprintf(buffer, sizeof(buffer), "%lld B", static_cast<long long>(n));
    return buffer;
}

// One alignment of one document, kept so watch can compare consecutive
// fetches.
struct Alignment {
    dd::doc::Model model;
    dd::classify::Prediction prediction;
    dd::schema::Mapping mapping;
    dd::schema::ExtractionResult extraction;
    double parse_ms = 0.0;
    double classify_ms = 0.0;
    double map_ms = 0.0;
};

Alignment align_document(const dd::schema::Registry& registry,
                         const dd::classify::Classifier& classifier,
                         const std::string& url, const std::string& content_type,
                         const std::string& body) {
    Alignment out;
    const dd::Stopwatch parse_watch;
    out.model = dd::doc::build_auto(content_type, body);
    out.parse_ms = parse_watch.elapsed_ms();

    const dd::Stopwatch classify_watch;
    out.prediction = classifier.classify(out.model, url);
    out.classify_ms = classify_watch.elapsed_ms();

    const dd::Stopwatch map_watch;
    out.mapping = dd::schema::infer_mapping(registry, out.model);
    out.extraction = dd::schema::apply_mapping(registry, out.mapping, out.model);
    out.map_ms = map_watch.elapsed_ms();
    return out;
}

void print_alignment(const dd::fetch::Result& fetched, const Alignment& a) {
    section("Fetch");
    dd::render::kv({
        {"url", fetched.final_url},
        {"bytes", fmt_bytes(fetched.bytes)},
        {"transfer", fmt_ms(fetched.total_ms)},
        {"format", std::string{dd::doc::format_name(a.model.format)}},
        {"records found", std::to_string(a.model.records.size())},
    });

    section("Classify");
    std::size_t shown = 0;
    for (const dd::model::Scored& s : a.prediction.distribution) {
        if (++shown > 3) break;
        std::printf("  %-22s %s\n", s.label.c_str(), meter(s.probability).c_str());
    }

    section("Match");
    if (a.mapping.fields.empty()) {
        std::printf("  %s no usable mapping: no label carried identity evidence\n",
                    stamp("failed").c_str());
    } else {
        std::vector<std::vector<std::string>> rows;
        for (const dd::schema::FieldMapping& fm : a.mapping.fields) {
            const auto rate_it = a.extraction.field_rates.find(fm.field);
            rows.push_back({fm.field, "<- " + fm.source_label, meter(fm.label_similarity),
                            meter(fm.value_pass_rate), meter(fm.confidence),
                            rate_it == a.extraction.field_rates.end() ? ""
                                                                      : meter(rate_it->second)});
        }
        dd::render::table({"canonical", "source label", "label match", "values valid",
                           "confidence", "extracted"},
                          rows);
        std::vector<std::string> unmatched;
        for (const std::string& label : a.model.labels) {
            bool used = false;
            for (const dd::schema::FieldMapping& fm : a.mapping.fields) {
                if (fm.source_label == label) used = true;
            }
            if (!used) unmatched.push_back(label);
        }
        if (!unmatched.empty()) {
            std::printf("  %s %s\n", paint("dim", "no signal:").c_str(),
                        paint("dim", dd::str::join(unmatched, ", ")).c_str());
        }
    }

    if (!a.extraction.records.empty()) {
        section("Extract");
        std::vector<std::string> header;
        for (const dd::schema::FieldMapping& fm : a.mapping.fields) header.push_back(fm.field);
        std::vector<std::vector<std::string>> rows;
        const std::size_t limit = 8;
        for (std::size_t i = 0; i < a.extraction.records.size() && i < limit; ++i) {
            std::vector<std::string> row;
            for (const std::string& field : header) {
                const auto it = a.extraction.records[i].values.find(field);
                std::string value = it == a.extraction.records[i].values.end() ? "" : it->second;
                if (value.size() > 28) value = value.substr(0, 27) + "…";
                row.push_back(value);
            }
            rows.push_back(std::move(row));
        }
        dd::render::table(header, rows);
        if (a.extraction.records.size() > limit) {
            std::printf("  %s\n",
                        paint("dim", "... " + std::to_string(a.extraction.records.size() - limit) +
                                         " more records").c_str());
        }
    }

    section("Result");
    const bool usable = !a.mapping.fields.empty() && a.extraction.rate > 0.0;
    dd::render::kv({
        {"status", usable ? stamp("aligned") : stamp("failed")},
        {"extraction rate", meter(a.extraction.rate)},
        {"mapping confidence", meter(a.mapping.confidence)},
        {"classified as", a.prediction.label + "  " + meter(a.prediction.confidence)},
        {"stage timings", "parse " + fmt_ms(a.parse_ms) + ", classify " + fmt_ms(a.classify_ms) +
                              ", match+extract " + fmt_ms(a.map_ms)},
    });
}

// Fetch and align one URL, printing the full report.
std::optional<Alignment> run_align(const dd::schema::Registry& registry,
                                   const dd::classify::Classifier& classifier,
                                   const std::string& url) {
    const dd::fetch::Result fetched = dd::fetch::get(url);
    if (!fetched.ok) {
        std::printf("%s fetch failed: %s\n", stamp("failed").c_str(), fetched.error.c_str());
        return std::nullopt;
    }
    try {
        Alignment a =
            align_document(registry, classifier, url, fetched.content_type, fetched.body);
        print_alignment(fetched, a);
        return a;
    } catch (const dd::Error& e) {
        std::printf("%s %s\n", stamp("failed").c_str(), e.what());
        return std::nullopt;
    }
}

std::string now_clock() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buffer[16];
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &tm);
    return buffer;
}

// Watch a URL: refetch on an interval, skip unchanged bytes cheaply, and
// when the page changes shape run the healer against the previous mapping
// and show the repair live.
int watch_url(const dd::schema::Registry& registry, const dd::classify::Classifier& classifier,
              const std::string& url, int interval_seconds) {
    std::printf("watching %s every %ds; ctrl-c to stop\n", url.c_str(), interval_seconds);

    std::optional<Alignment> previous;
    std::uint64_t previous_hash = 0;
    g_interrupted = 0;
    std::signal(SIGINT, handle_interrupt);

    while (g_interrupted == 0) {
        const dd::fetch::Result fetched = dd::fetch::get(url);
        if (!fetched.ok) {
            std::printf("[%s] %s fetch failed: %s\n", now_clock().c_str(),
                        stamp("failed").c_str(), fetched.error.c_str());
        } else {
            const std::uint64_t hash = dd::str::hash64(fetched.body);
            if (previous.has_value() && hash == previous_hash) {
                std::printf("[%s] %s %s in %s: same bytes, nothing to do\n", now_clock().c_str(),
                            stamp("unchanged").c_str(), fmt_bytes(fetched.bytes).c_str(),
                            fmt_ms(fetched.total_ms).c_str());
            } else {
                try {
                    Alignment current = align_document(registry, classifier, url,
                                                       fetched.content_type, fetched.body);
                    if (!previous.has_value()) {
                        std::printf("[%s] first fetch, learning the mapping\n",
                                    now_clock().c_str());
                        print_alignment(fetched, current);
                    } else {
                        // Would the previous mapping still work on this page?
                        const dd::schema::ExtractionResult with_old = dd::schema::apply_mapping(
                            registry, previous->mapping, current.model);
                        const double baseline = previous->extraction.rate;
                        if (with_old.rate < baseline * 0.6) {
                            std::printf("[%s] %s bytes changed and the old mapping collapsed: "
                                        "%.0f%% -> %.0f%%\n",
                                        now_clock().c_str(), stamp("drift").c_str(),
                                        baseline * 100.0, with_old.rate * 100.0);
                            const dd::heal::Proposal proposal = dd::heal::propose(
                                registry, current.model, previous->mapping, baseline);
                            for (const std::string& change : proposal.changes) {
                                std::printf("    %s\n", change.c_str());
                            }
                            std::printf("[%s] %s recovered %.0f%% at %.0f%% confidence\n",
                                        now_clock().c_str(),
                                        proposal.acceptable ? stamp("healed").c_str()
                                                            : stamp("review").c_str(),
                                        proposal.result.rate * 100.0,
                                        proposal.confidence * 100.0);
                            print_alignment(fetched, current);
                        } else {
                            std::printf("[%s] %s content updated, mapping still fits: "
                                        "%zu records at %.0f%%\n",
                                        now_clock().c_str(), stamp("ok").c_str(),
                                        current.extraction.records.size(),
                                        current.extraction.rate * 100.0);
                        }
                    }
                    previous = std::move(current);
                    previous_hash = hash;
                } catch (const dd::Error& e) {
                    std::printf("[%s] %s %s\n", now_clock().c_str(), stamp("failed").c_str(),
                                e.what());
                }
            }
        }
        for (int slept = 0; slept < interval_seconds * 10 && g_interrupted == 0; ++slept) {
            struct timespec ts {0, 100000000};
            nanosleep(&ts, nullptr);
        }
    }
    std::signal(SIGINT, SIG_DFL);
    std::printf("\nstopped watching\n");
    return 0;
}

void print_schema(const dd::schema::Registry& registry) {
    section("Schema");
    std::vector<std::vector<std::string>> rows;
    for (const dd::schema::FieldDef& f : registry.fields()) {
        rows.push_back({f.name, std::string{dd::schema::kind_name(f.kind)},
                        f.role.empty() ? "" : f.role, f.identity ? "yes" : "",
                        std::to_string(f.synonyms.size()) + " synonyms"});
    }
    dd::render::table({"field", "kind", "role", "identity", "lexicon"}, rows);
}

std::vector<std::string> shell_tokens(const std::string& input) {
    std::vector<std::string> out;
    std::string current;
    bool quoted = false;
    for (char c : input) {
        if (c == '"') {
            quoted = !quoted;
            continue;
        }
        if (c == ' ' && !quoted) {
            if (!current.empty()) out.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

void print_run(const dd::store::RunRecord& run);

// Store-backed state, built on first use so pasting a URL stays instant.
struct ShellState {
    std::string state_dir;
    std::string seeds_path;
    std::string model_path;
    std::string schema_path;
    std::optional<dd::store::Store> store;
    std::optional<dd::pipeline::Pipeline> pipeline;

    dd::pipeline::Pipeline& ensure() {
        if (!pipeline.has_value()) {
            store.emplace(state_dir);
            store->seed(seeds_path);
            pipeline.emplace(*store, dd::classify::Classifier::load(model_path),
                             dd::schema::Registry::load(schema_path));
        }
        return *pipeline;
    }
};

int shell(const dd::schema::Registry& registry, const dd::classify::Classifier& classifier,
          ShellState& state) {
    std::printf("%s\n", paint("bold", "Data Diver").c_str());
    std::printf("%s\n",
                paint("dim", "paste a URL to align it; 'help' lists commands.").c_str());

    std::string line;
    while (true) {
        std::printf("\n%s ", paint("teal", "diver>").c_str());
        std::fflush(stdout);
        if (!std::getline(std::cin, line)) break;
        const std::string input = dd::str::trim(line);
        if (input.empty()) continue;
        if (input == "quit" || input == "exit" || input == "q") break;
        if (input == "help") {
            std::printf("  URL                    align a page or API response\n"
                        "  watch URL [secs]       refetch on an interval, heal on drift\n"
                        "  counties               county list with sources and properties\n"
                        "  county NAME            the county's properties, filled per schema\n"
                        "  sources                configured sources\n"
                        "  add \"County\" \"Name\" URL   add a source\n"
                        "  run (all | SOURCE_ID)  ingest into the store\n"
                        "  map SOURCE_ID          learned mapping with sample values\n"
                        "  review SOURCE_ID       confirm or refuse uncertain matches\n"
                        "  model                  classifier status\n"
                        "  train [ALPHA|sweep]    retrain with validation breakdown\n"
                        "  schema                 the canonical fields\n"
                        "  quit                   leave\n");
            continue;
        }
        if (input == "counties") {
            state.ensure();
            dd::cli::counties(*state.store);
            continue;
        }
        if (input == "sources") {
            state.ensure();
            for (const dd::store::Source& s : state.store->sources()) {
                std::printf("  %-26s %-40s %s\n", s.id.c_str(), s.name.c_str(), s.url.c_str());
            }
            continue;
        }
        if (input.rfind("county ", 0) == 0) {
            state.ensure();
            dd::cli::county_properties(*state.store, state.pipeline->registry(),
                                       dd::str::trim(input.substr(7)));
            continue;
        }
        if (input.rfind("add ", 0) == 0) {
            const std::vector<std::string> parts = shell_tokens(input);
            if (parts.size() != 4) {
                std::printf("  usage: add \"County Name\" \"Source Name\" URL\n");
                continue;
            }
            state.ensure();
            const dd::store::Source s =
                state.store->add_source(parts[2], parts[3], parts[1]);
            std::printf("  added %s; 'run %s' to ingest it\n", s.id.c_str(), s.id.c_str());
            continue;
        }
        if (input.rfind("run", 0) == 0 && (input == "run" || input[3] == ' ')) {
            const std::vector<std::string> parts = shell_tokens(input);
            dd::pipeline::Pipeline& pipeline = state.ensure();
            if (parts.size() < 2 || parts[1] == "all") {
                for (const dd::store::Source& s : state.store->sources()) {
                    if (s.enabled) print_run(pipeline.run_source(s));
                }
            } else {
                try {
                    print_run(pipeline.run_source_id(parts[1]));
                } catch (const dd::Error& e) {
                    std::printf("  %s\n", e.what());
                }
            }
            continue;
        }
        if (input.rfind("map ", 0) == 0) {
            dd::pipeline::Pipeline& pipeline = state.ensure();
            dd::cli::show_mapping(*state.store, pipeline, dd::str::trim(input.substr(4)));
            continue;
        }
        if (input.rfind("review ", 0) == 0) {
            dd::pipeline::Pipeline& pipeline = state.ensure();
            dd::cli::review(*state.store, pipeline, dd::str::trim(input.substr(7)), std::cin);
            continue;
        }
        if (input == "model") {
            if (state.pipeline.has_value()) dd::cli::model_status(state.pipeline->classifier());
            else dd::cli::model_status(classifier);
            continue;
        }
        if (input.rfind("train", 0) == 0 && (input == "train" || input[5] == ' ')) {
            const std::vector<std::string> parts = shell_tokens(input);
            const bool sweep = parts.size() >= 2 && parts[1] == "sweep";
            const double alpha =
                (!sweep && parts.size() >= 2) ? std::atof(parts[1].c_str()) : 1.0;
            dd::cli::train(state.pipeline.has_value() ? &*state.pipeline : nullptr,
                           "data/corpus", state.model_path, alpha > 0.0 ? alpha : 1.0, sweep);
            continue;
        }
        if (input == "schema") {
            print_schema(registry);
            continue;
        }
        if (input.rfind("watch ", 0) == 0) {
            const std::vector<std::string> parts = dd::str::split(input, ' ');
            if (parts.size() < 2 || parts[1].empty()) {
                std::printf("  usage: watch URL [seconds]\n");
                continue;
            }
            const int seconds =
                parts.size() >= 3 ? std::max(1, std::atoi(parts[2].c_str())) : 15;
            watch_url(registry, classifier, parts[1], seconds);
            continue;
        }
        const dd::Stopwatch total;
        run_align(registry, classifier, input);
        std::printf("\n%s\n",
                    paint("dim", "total " + fmt_ms(total.elapsed_ms()) + " end to end").c_str());
    }
    std::printf("bye\n");
    return 0;
}

void print_run(const dd::store::RunRecord& run) {
    std::printf("%-28s %s %-18s records=%-4lld events=%-4lld rate=%.2f %s\n",
                run.source_id.c_str(), run.ok ? stamp("ok").c_str() : stamp("failed").c_str(),
                run.classification.c_str(), static_cast<long long>(run.records_extracted),
                static_cast<long long>(run.events_new), run.extraction_rate,
                run.ok ? "" : (run.stage + ": " + run.error).c_str());
}

int usage() {
    std::fprintf(
        stderr,
        "usage:\n"
        "  datadiver                          interactive shell\n"
        "  datadiver align URL                one-shot alignment report\n"
        "  datadiver watch URL [--interval N] monitor a url, heal on drift\n"
        "  datadiver ingest (--all | ID)      stateful ingestion into var/\n"
        "  datadiver sources                  list configured sources\n"
        "  datadiver counties                 county rollup from the store\n"
        "  datadiver county NAME              a county's properties per schema\n"
        "  datadiver map SOURCE_ID            learned mapping with sample values\n"
        "  datadiver review SOURCE_ID         confirm or refuse uncertain matches\n"
        "  datadiver model                    classifier status\n"
        "  datadiver train [--alpha A|--sweep] retrain with validation breakdown\n"
        "flags: --schema FILE  --model FILE  --state DIR  --seeds FILE\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);
    const std::string state_dir = flag(args, "state", "var");
    const std::string model_path = flag(args, "model", "data/model/source_classifier.json");
    const std::string seeds_path = flag(args, "seeds", "data/sources.json");
    const std::string schema_path = flag(args, "schema", "data/schema.json");

    try {
        if (args.command.empty() || args.command == "shell") {
            ShellState state;
            state.state_dir = state_dir;
            state.seeds_path = seeds_path;
            state.model_path = model_path;
            state.schema_path = schema_path;
            return shell(dd::schema::Registry::load(schema_path),
                         dd::classify::Classifier::load(model_path), state);
        }

        if (args.command == "align") {
            if (args.positional.empty()) return usage();
            const dd::schema::Registry registry = dd::schema::Registry::load(schema_path);
            const dd::classify::Classifier classifier =
                dd::classify::Classifier::load(model_path);
            const dd::Stopwatch total;
            const std::optional<Alignment> result =
                run_align(registry, classifier, args.positional[0]);
            if (result.has_value()) {
                std::printf("\n%s\n",
                            paint("dim", "total " + fmt_ms(total.elapsed_ms()) + " end to end")
                                .c_str());
            }
            return result.has_value() ? 0 : 1;
        }

        if (args.command == "watch") {
            if (args.positional.empty()) return usage();
            const int interval = std::max(1, std::atoi(flag(args, "interval", "15").c_str()));
            return watch_url(dd::schema::Registry::load(schema_path),
                             dd::classify::Classifier::load(model_path), args.positional[0],
                             interval);
        }

        if (args.command == "ingest") {
            dd::store::Store store{state_dir};
            store.seed(seeds_path);
            dd::pipeline::Pipeline pipeline{store, dd::classify::Classifier::load(model_path),
                                            dd::schema::Registry::load(schema_path)};
            bool any_failed = false;
            if (args.flags.count("all") != 0) {
                for (const dd::store::Source& s : store.sources()) {
                    if (!s.enabled) continue;
                    const dd::store::RunRecord run = pipeline.run_source(s);
                    print_run(run);
                    if (!run.ok) any_failed = true;
                }
            } else if (!args.positional.empty()) {
                const dd::store::RunRecord run = pipeline.run_source_id(args.positional[0]);
                print_run(run);
                any_failed = !run.ok;
            } else {
                return usage();
            }
            return any_failed ? 1 : 0;
        }

        if (args.command == "sources") {
            dd::store::Store store{state_dir};
            store.seed(seeds_path);
            for (const dd::store::Source& s : store.sources()) {
                std::printf("%-28s %-42s %s\n", s.id.c_str(), s.name.c_str(), s.url.c_str());
            }
            return 0;
        }

        if (args.command == "train") {
            const double alpha = std::atof(flag(args, "alpha", "1.0").c_str());
            return dd::cli::train(nullptr, flag(args, "corpus", "data/corpus"), model_path,
                                  alpha > 0.0 ? alpha : 1.0, args.flags.count("sweep") != 0);
        }

        if (args.command == "counties" || args.command == "county" || args.command == "map" ||
            args.command == "review" || args.command == "model") {
            dd::store::Store store{state_dir};
            store.seed(seeds_path);
            dd::pipeline::Pipeline pipeline{store, dd::classify::Classifier::load(model_path),
                                            dd::schema::Registry::load(schema_path)};
            if (args.command == "counties") {
                dd::cli::counties(store);
                return 0;
            }
            if (args.command == "model") {
                dd::cli::model_status(pipeline.classifier());
                return 0;
            }
            if (args.positional.empty()) return usage();
            const std::string target = dd::str::join(args.positional, " ");
            if (args.command == "county") dd::cli::county_properties(store, pipeline.registry(), target);
            else if (args.command == "map") dd::cli::show_mapping(store, pipeline, target);
            else dd::cli::review(store, pipeline, target, std::cin);
            return 0;
        }

        return usage();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "datadiver: %s\n", e.what());
        return 1;
    }
}
