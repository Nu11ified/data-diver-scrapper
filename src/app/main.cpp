// datadiver: the engine's front door.
//
//   datadiver serve   [--port N] [--state DIR] [--model FILE] [--web DIR] [--seeds FILE]
//   datadiver ingest  (--all | SOURCE_ID) [--state DIR] [--model FILE] [--seeds FILE]
//   datadiver sources [--state DIR] [--seeds FILE]

#include "dd/ml/classify.hpp"
#include "dd/engine/pipeline.hpp"
#include "dd/net/server.hpp"
#include "dd/engine/store.hpp"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

volatile std::sig_atomic_t g_stop = 0;
void handle_signal(int) { g_stop = 1; }

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

void print_run(const dd::store::RunRecord& run) {
    std::printf("%-28s %-6s %-18s records=%-4lld events=%-4lld rate=%.2f %s\n",
                run.source_id.c_str(), run.ok ? "ok" : "FAIL", run.classification.c_str(),
                static_cast<long long>(run.records_extracted),
                static_cast<long long>(run.events_new), run.extraction_rate,
                run.ok ? "" : (run.stage + ": " + run.error).c_str());
}

int usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  datadiver serve   [--port N] [--state DIR] [--model FILE] [--web DIR] [--seeds FILE]\n"
                 "  datadiver ingest  (--all | SOURCE_ID) [--state DIR] [--model FILE] [--seeds FILE]\n"
                 "  datadiver sources [--state DIR] [--seeds FILE]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);
    const std::string state_dir = flag(args, "state", "var");
    const std::string model_path = flag(args, "model", "data/model/source_classifier.json");
    const std::string seeds_path = flag(args, "seeds", "data/sources.json");

    try {
        if (args.command == "serve") {
            dd::store::Store store{state_dir};
            store.seed(seeds_path);
            dd::pipeline::Pipeline pipeline{store, dd::classify::Classifier::load(model_path)};

            dd::server::Options options;
            options.port = std::atoi(flag(args, "port", "8080").c_str());
            options.web_root = flag(args, "web", "web");
            dd::server::Server server{store, pipeline, options};
            server.start();
            std::printf("Data Diver serving on http://127.0.0.1:%d (state: %s)\n",
                        server.port(), state_dir.c_str());

            std::signal(SIGINT, handle_signal);
            std::signal(SIGTERM, handle_signal);
            while (g_stop == 0) {
                struct timespec ts {0, 200000000};
                nanosleep(&ts, nullptr);
            }
            std::printf("shutting down\n");
            server.stop();
            return 0;
        }

        if (args.command == "ingest") {
            dd::store::Store store{state_dir};
            store.seed(seeds_path);
            dd::pipeline::Pipeline pipeline{store, dd::classify::Classifier::load(model_path)};
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
                std::printf("%-28s %-40s %s\n", s.id.c_str(), s.name.c_str(), s.url.c_str());
            }
            return 0;
        }

        return usage();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "datadiver: %s\n", e.what());
        return 1;
    }
}
