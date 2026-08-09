// MobileGL - MobileGL/MG_Test/Util/AsyncPoolBench.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

// A head-to-head harness for the two ShaderCompilePool execution engines
// (MOBILEGL_ASYNC_POOL=asio|libfork). Not a gtest: it measures one wall-clock interval per
// process, because most of what it drives is memoized per process (the shader preprocess
// cache and the compile-adoption map both live for the life of the GL context), so a second
// timed repetition inside one process would measure the cache, not the compiler. The driver
// script re-executes the binary for every repetition instead.
//
// Two modes:
//
//   corpus - the REAL frontend path. glCreateShader/glShaderSource are done untimed, then
//            the clock starts and glCompileShader/glLinkProgram submit every job, and stops
//            once glGetProgramiv(GL_LINK_STATUS) has joined all of them. That is exactly the
//            first-submit-to-all-joined interval a shaderpack load pays.
//
//   micro  - N trivial JobNodes straight through ShaderCompilePool::Post, isolating the
//            executor's own dispatch overhead from any workload contention.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Includes.h"
#include "Init.h"
#include <Config.h>

#include <MG_Impl/GLImpl/Program/GL_Program.h>
#include <MG_Util/Async/JobNode.h>
#include <MG_Util/Async/ShaderCompilePool.h>

using namespace MobileGL;
using namespace MobileGL::MG_Util::Async;
namespace GLImpl = MobileGL::MG_Impl::GLImpl;
namespace fs = std::filesystem;

namespace {
    using Clock = std::chrono::steady_clock;

    double MillisSince(const Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    GLenum StageFromExtension(const std::string& ext) {
        if (ext == ".vert") return GL_VERTEX_SHADER;
        if (ext == ".frag") return GL_FRAGMENT_SHADER;
        if (ext == ".geom") return GL_GEOMETRY_SHADER;
        if (ext == ".comp") return GL_COMPUTE_SHADER;
        if (ext == ".tesc") return GL_TESS_CONTROL_SHADER;
        if (ext == ".tese") return GL_TESS_EVALUATION_SHADER;
        return 0;
    }

    std::string ReadFile(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream buf;
        buf << in.rdbuf();
        return buf.str();
    }

    struct CorpusShader {
        std::string name;
        std::string source;
        GLenum stage = 0;
    };

    // One program's worth of the corpus: the trace's link group. Shaders are indices into
    // the flat shader list, because a source shared by several programs must stay ONE entry
    // - that sharing is what the compile-adoption map sees in the real path too.
    struct CorpusProgram {
        std::vector<SizeT> shaders;
    };

    struct Corpus {
        std::vector<CorpusShader> shaders;
        std::vector<CorpusProgram> programs;
        SizeT totalBytes = 0;
    };

    // Reads a corpus directory written by extract_corpus.py: one file per compiled shader,
    // stage in the extension, plus manifest.txt naming the trace's link groups.
    Corpus LoadCorpus(const fs::path& dir) {
        Corpus corpus;
        std::unordered_map<std::string, SizeT> byName;

        const auto intern = [&](const std::string& name) -> SizeT {
            if (const auto it = byName.find(name); it != byName.end()) return it->second;
            const fs::path path = dir / name;
            if (!fs::exists(path)) return static_cast<SizeT>(-1);
            CorpusShader shader;
            shader.name = name;
            shader.source = ReadFile(path);
            shader.stage = StageFromExtension(path.extension().string());
            if (shader.stage == 0) return static_cast<SizeT>(-1);
            corpus.totalBytes += shader.source.size();
            corpus.shaders.push_back(Move(shader));
            const SizeT index = corpus.shaders.size() - 1;
            byName.emplace(name, index);
            return index;
        };

        const fs::path manifest = dir / "manifest.txt";
        if (fs::exists(manifest)) {
            std::ifstream in(manifest);
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty() || line[0] == '#') continue;
                CorpusProgram program;
                std::istringstream fields(line);
                std::string name;
                while (fields >> name) {
                    const SizeT index = intern(name);
                    if (index != static_cast<SizeT>(-1)) program.shaders.push_back(index);
                }
                if (!program.shaders.empty()) corpus.programs.push_back(Move(program));
            }
        }

        // Anything in the directory the manifest never linked still gets compiled, as a
        // program-less group, so the corpus on disk and the corpus measured are the same set.
        std::vector<fs::path> leftovers;
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            const std::string name = entry.path().filename().string();
            if (name == "manifest.txt") continue;
            if (StageFromExtension(entry.path().extension().string()) == 0) continue;
            if (byName.count(name) != 0) continue;
            leftovers.push_back(entry.path());
        }
        std::sort(leftovers.begin(), leftovers.end());
        for (const auto& path : leftovers) intern(path.filename().string());

        return corpus;
    }

    struct CorpusResult {
        double submitMs = 0;  // first glCompileShader -> last glLinkProgram returned
        double joinMs = 0;    // last submit -> every program joined
        double totalMs = 0;   // the number that matters: first submit -> all joined
        SizeT linkFailures = 0;
        SizeT compileFailures = 0;
    };

    CorpusResult RunCorpus(const Corpus& corpus) {
        // ---- Untimed: create every GL object and stage every source ----------------------
        // glShaderSource is a memcpy into the shader object and glAttachShader is a pointer
        // append; neither touches the pool. Keeping them outside the clock makes the measured
        // interval exactly the compile+link critical path, which is what an application's
        // loading screen waits on.
        std::vector<GLuint> shaderNames(corpus.shaders.size(), 0);
        for (SizeT i = 0; i < corpus.shaders.size(); ++i) {
            const CorpusShader& shader = corpus.shaders[i];
            const GLuint name = GLImpl::CreateShader(shader.stage);
            const GLchar* text = shader.source.c_str();
            const GLint length = static_cast<GLint>(shader.source.size());
            GLImpl::ShaderSource(name, 1, &text, &length);
            shaderNames[i] = name;
        }

        std::vector<GLuint> programNames(corpus.programs.size(), 0);
        for (SizeT p = 0; p < corpus.programs.size(); ++p) {
            const GLuint program = GLImpl::CreateProgram();
            for (const SizeT shaderIndex : corpus.programs[p].shaders) {
                GLImpl::AttachShader(program, shaderNames[shaderIndex]);
            }
            programNames[p] = program;
        }

        // ---- Timed ------------------------------------------------------------------------
        const Clock::time_point start = Clock::now();

        // Submission order follows the trace: a program's shaders, then its link. That order
        // is what exercises ProgramLinkTask::SubmitAfter's dependency chaining rather than a
        // flat burst of independent compiles.
        std::vector<Bool> submitted(corpus.shaders.size(), false);
        for (SizeT p = 0; p < corpus.programs.size(); ++p) {
            for (const SizeT shaderIndex : corpus.programs[p].shaders) {
                if (submitted[shaderIndex]) continue;
                submitted[shaderIndex] = true;
                GLImpl::CompileShader(shaderNames[shaderIndex]);
            }
            GLImpl::LinkProgram(programNames[p]);
        }
        for (SizeT i = 0; i < corpus.shaders.size(); ++i) {
            if (submitted[i]) continue;
            submitted[i] = true;
            GLImpl::CompileShader(shaderNames[i]);
        }

        const Clock::time_point submitted_at = Clock::now();

        CorpusResult result;
        // GL_LINK_STATUS is a joining query (GL_COMPLETION_STATUS_KHR is the one that must
        // not join), so this loop is the all-joined barrier.
        for (const GLuint program : programNames) {
            GLint status = 0;
            GLImpl::GetProgramiv(program, GL_LINK_STATUS, &status);
            if (status == GL_FALSE) ++result.linkFailures;
        }
        for (const GLuint shader : shaderNames) {
            GLint status = 0;
            GLImpl::GetShaderiv(shader, GL_COMPILE_STATUS, &status);
            if (status == GL_FALSE) ++result.compileFailures;
        }

        result.totalMs = MillisSince(start);
        result.submitMs = std::chrono::duration<double, std::milli>(submitted_at - start).count();
        result.joinMs = result.totalMs - result.submitMs;

        for (const GLuint program : programNames) GLImpl::DeleteProgram(program);
        for (const GLuint shader : shaderNames) GLImpl::DeleteShader(shader);
        return result;
    }

    // ---- Executor microbenchmark ----------------------------------------------------------
    // The body is deliberately near-empty: what is being measured is Post -> engine ->
    // RunOnWorker -> next dispatch, i.e. the executor's own cost per job, with no compiler
    // work to hide it.
    //
    // The barrier is an all-jobs-ran latch, and it has to be. This bench used to stop the
    // clock at StopAndDrain(), which is not a "wait for everything" - it is the teardown path,
    // and its contract is to ABANDON whatever the budget has not dispatched yet (see
    // ShaderCompilePool::StopAndDrain, and the JobNodeTest case that pins exactly that). With
    // 100k jobs behind a budget of N, most of them were therefore cancelled rather than run,
    // and the fraction that survived was decided by how fast the engine drained the queue
    // relative to the posting loop - i.e. by the very quantity under test. Measured on this
    // machine at 8 workers: Asio ran 75,906 of 100,000 and libfork 99,998, and both were
    // scored as if they had run 100,000. The reported "libfork is 1.36x faster" was libfork
    // being charged for 32% more work than Asio.
    class TrivialJob final : public JobNode {
    public:
        TrivialJob(std::atomic<Uint64>* sink, const Uint64 total, std::mutex* mutex,
                   std::condition_variable* cv)
            : m_sink(sink), m_total(total), m_mutex(mutex), m_cv(cv) {}

    private:
        void RunBody() override {
            if (m_sink->fetch_add(1, std::memory_order_acq_rel) + 1 == m_total) {
                // The last job wakes the timer. Under the lock, so the waiter cannot miss it
                // between its predicate check and its wait.
                const std::lock_guard<std::mutex> lock(*m_mutex);
                m_cv->notify_all();
            }
        }

        std::atomic<Uint64>* m_sink;
        Uint64 m_total;
        std::mutex* m_mutex;
        std::condition_variable* m_cv;
    };

    struct MicroResult {
        double ms = 0;
        Uint64 ran = 0;
    };

    MicroResult RunMicrobench(const Uint threads, const SizeT jobs) {
        ShaderCompilePool pool(threads);
        std::atomic<Uint64> counter{0};
        std::mutex mutex;
        std::condition_variable cv;
        const auto total = static_cast<Uint64>(jobs);

        // Nodes are allocated up front: MakeShared is not what is under test, and leaving it
        // inside the loop would put an allocator on the critical path in front of the
        // dispatch path this is meant to isolate.
        std::vector<SharedPtr<JobNode>> nodes;
        nodes.reserve(jobs);
        for (SizeT i = 0; i < jobs; ++i) {
            nodes.push_back(MakeShared<TrivialJob>(&counter, total, &mutex, &cv));
        }

        const Clock::time_point start = Clock::now();
        for (auto& node : nodes) pool.Post(Move(node));
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&] { return counter.load(std::memory_order_acquire) >= total; });
        }
        const double ms = MillisSince(start);

        MicroResult result;
        result.ms = ms;
        result.ran = counter.load(std::memory_order_acquire);
        return result;
    }

    [[noreturn]] void Usage() {
        std::fprintf(stderr,
                     "usage: AsyncPoolBench --corpus DIR\n"
                     "       AsyncPoolBench --micro JOBS --threads N\n"
                     "env: MOBILEGL_ASYNC_POOL=asio|libfork, "
                     "MOBILEGL_ASYNC_SHADER_COMPILE_THREADS=N\n");
        std::exit(2);
    }
} // namespace

int main(int argc, char** argv) {
    std::string corpusDir;
    SizeT microJobs = 0;
    Uint microThreads = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) Usage();
            return argv[++i];
        };
        if (arg == "--corpus") corpusDir = next();
        else if (arg == "--micro") microJobs = static_cast<SizeT>(std::stoull(next()));
        else if (arg == "--threads") microThreads = static_cast<Uint>(std::stoul(next()));
        else Usage();
    }
    if (corpusDir.empty() && microJobs == 0) Usage();

    Initialize();

    const AsyncPoolEngine engine = DetectAsyncPoolEngine();
    const char* engineName = AsyncPoolEngineName(engine);

    if (microJobs != 0) {
        const Uint threads = microThreads != 0 ? microThreads : DetectShaderCompileThreadCount();
        const MicroResult result = RunMicrobench(threads, microJobs);
        // `ran` is printed, not just checked, so that a run in which the arms did different
        // amounts of work is visible in the results file rather than on a stderr the driver
        // script redirects to /dev/null. ns_per_job divides by what actually ran.
        std::printf("RESULT mode=micro engine=%s threads=%u jobs=%zu ran=%llu total_ms=%.3f "
                    "ns_per_job=%.1f\n",
                    engineName, threads, microJobs, static_cast<unsigned long long>(result.ran),
                    result.ms, result.ms * 1e6 / static_cast<double>(result.ran));
        return result.ran == microJobs ? 0 : 1;
    }

    const Corpus corpus = LoadCorpus(corpusDir);
    if (corpus.shaders.empty()) {
        std::fprintf(stderr, "AsyncPoolBench: no shaders found in %s\n", corpusDir.c_str());
        return 1;
    }

    if (!AsyncShaderCompileActive()) {
        std::fprintf(stderr, "AsyncPoolBench: asynchronous compilation is OFF; measuring the "
                             "inline path\n");
    }

    const CorpusResult result = RunCorpus(corpus);
    const Uint threads = ShaderCompilePool::Get().GetThreadCount();

    std::printf("RESULT mode=corpus engine=%s threads=%u corpus=%s shaders=%zu programs=%zu "
                "bytes=%zu total_ms=%.3f submit_ms=%.3f join_ms=%.3f link_fail=%zu "
                "compile_fail=%zu\n",
                engineName, threads, corpusDir.c_str(), corpus.shaders.size(),
                corpus.programs.size(), corpus.totalBytes, result.totalMs, result.submitMs,
                result.joinMs, result.linkFailures, result.compileFailures);
    return 0;
}
