// MobileGL - MobileGL/MG_Benchmark/Buffer/BufferBench.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#include <cstring>
#include <vector>
#include <benchmark/benchmark.h>

#include "Init.h"
#include "MG_Impl/GLImpl/Buffer/GL_Buffer.h"
#include "MG_State/GLState/Core.h"

using namespace MobileGL;
using namespace MobileGL::MG_Impl::GLImpl;

constexpr GLuint BUFFER_COUNT = 32;
constexpr GLsizeiptr BUFFER_SIZE = 1024 * 1024;

static void BM_GenerateAndDeleteBuffers(benchmark::State& state) {
    GLuint n = static_cast<GLuint>(state.range(0));
    int repeat = static_cast<int>(state.range(1));
    std::vector<GLuint> buffers(n);

    for (auto _ : state) {
        for (int i = 0; i < repeat; i++) {
            GenBuffers(n, buffers.data());
            DeleteBuffers(n, buffers.data());
        }
    }

    state.SetItemsProcessed(state.iterations() * repeat * n);
}

BENCHMARK(BM_GenerateAndDeleteBuffers)
    ->Args({128, 5})
    ->Args({1024, 10})
    ->Args({1536, 16})
    ->Args({2048, 32})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

static void BM_CreateBufferObjectsAndBindBuffer(benchmark::State& state) {
    GLuint buffers[BUFFER_COUNT];
    GenBuffers(BUFFER_COUNT, buffers);

    for (auto _ : state) {
        for (GLuint i = 0; i < BUFFER_COUNT; i++) {
            GLenum target = (i % 2 == 0) ? GL_ARRAY_BUFFER : GL_UNIFORM_BUFFER;
            BindBuffer(target, buffers[i]);
        }
    }

    state.SetItemsProcessed(state.iterations() * BUFFER_COUNT);

    DeleteBuffers(BUFFER_COUNT, buffers);
}
BENCHMARK(BM_CreateBufferObjectsAndBindBuffer)->Unit(benchmark::kMillisecond)->UseRealTime();

static void BM_DeleteBufferObjects(benchmark::State& state) {
    Initialize();
    std::vector<GLuint> buffers(BUFFER_COUNT);

    for (auto _ : state) {
        state.PauseTiming();
        GenBuffers(BUFFER_COUNT, buffers.data());
        for (GLuint i = 0; i < BUFFER_COUNT; i++) {
            GLenum target = (i % 2 == 0) ? GL_ARRAY_BUFFER : GL_UNIFORM_BUFFER;
            BindBuffer(target, buffers[i]);
        }
        state.ResumeTiming();

        DeleteBuffers(BUFFER_COUNT, buffers.data());
    }

    state.SetItemsProcessed(state.iterations() * BUFFER_COUNT);
}
BENCHMARK(BM_DeleteBufferObjects)->Unit(benchmark::kMillisecond)->UseRealTime();

static void BM_UpdateData(benchmark::State& state) {
    GLuint buffers[BUFFER_COUNT];
    GenBuffers(BUFFER_COUNT, buffers);

    for (GLuint i = 0; i < BUFFER_COUNT; i++)
        BindBuffer(GL_ARRAY_BUFFER, buffers[i]);

    std::vector<char> data(BUFFER_SIZE, 0);

    for (auto _ : state) {
        for (GLuint i = 0; i < BUFFER_COUNT; i++) {
            BindBuffer(GL_ARRAY_BUFFER, buffers[i]);
            BufferData(GL_ARRAY_BUFFER, BUFFER_SIZE, data.data(), GL_STATIC_DRAW);
        }
    }

    state.SetItemsProcessed(state.iterations() * BUFFER_COUNT);

    DeleteBuffers(BUFFER_COUNT, buffers);
}
BENCHMARK(BM_UpdateData)->Unit(benchmark::kMillisecond)->UseRealTime();

static void BM_MapBuffer(benchmark::State& state) {
    GLuint buffers[BUFFER_COUNT];
    GenBuffers(BUFFER_COUNT, buffers);
    for (GLuint i = 0; i < BUFFER_COUNT; i++)
        BindBuffer(GL_ARRAY_BUFFER, buffers[i]);

    std::vector<char> data(BUFFER_SIZE, 0);

    for (auto _ : state) {
        for (GLuint i = 0; i < BUFFER_COUNT; i++) {
            BindBuffer(GL_ARRAY_BUFFER, buffers[i]);
            BufferData(GL_ARRAY_BUFFER, BUFFER_SIZE, data.data(), GL_DYNAMIC_DRAW);

            void* ptr = MapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
            if (ptr) {
                memset(ptr, 1, BUFFER_SIZE);
                UnmapBuffer(GL_ARRAY_BUFFER);
            }
        }
    }

    state.SetItemsProcessed(state.iterations() * BUFFER_COUNT);

    DeleteBuffers(BUFFER_COUNT, buffers);
}
BENCHMARK(BM_MapBuffer)->Unit(benchmark::kMillisecond)->UseRealTime();

static void BM_CopyBufferSubData(benchmark::State& state) {
    GLuint buffers[BUFFER_COUNT];
    GenBuffers(BUFFER_COUNT, buffers);

    std::vector<char> data(BUFFER_SIZE, 0);

    for (GLuint i = 0; i < BUFFER_COUNT; i++) {
        GLenum target = (i % 2 == 0) ? GL_COPY_READ_BUFFER : GL_COPY_WRITE_BUFFER;
        BindBuffer(target, buffers[i]);
        BufferData(target, BUFFER_SIZE, data.data(), GL_STATIC_DRAW);
    }

    for (auto _ : state) {
        for (GLuint i = 0; i < BUFFER_COUNT; i += 2) {
            BindBuffer(GL_COPY_READ_BUFFER, buffers[i]);
            BindBuffer(GL_COPY_WRITE_BUFFER, buffers[i + 1]);
            CopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, BUFFER_SIZE);
        }
    }

    state.SetItemsProcessed(state.iterations() * (BUFFER_COUNT / 2));

    DeleteBuffers(BUFFER_COUNT, buffers);
}
BENCHMARK(BM_CopyBufferSubData)->Unit(benchmark::kMillisecond)->UseRealTime();

static void BM_UpdateDataPartially(benchmark::State& state) {
    std::vector<GLuint> buffers(BUFFER_COUNT);
    std::vector<char> data(BUFFER_SIZE / 10, 1);

    GenBuffers(BUFFER_COUNT, buffers.data());
    for (GLuint i = 0; i < BUFFER_COUNT; i++) {
        BindBuffer(GL_ARRAY_BUFFER, buffers[i]);
        BufferData(GL_ARRAY_BUFFER, BUFFER_SIZE, nullptr, GL_DYNAMIC_DRAW);
    }

    for (auto _ : state) {
        for (GLuint i = 0; i < BUFFER_COUNT; i++) {
            BindBuffer(GL_ARRAY_BUFFER, buffers[i]);
            BufferSubData(GL_ARRAY_BUFFER, BUFFER_SIZE / 2, data.size(), data.data());
        }
    }

    state.SetItemsProcessed(state.iterations() * BUFFER_COUNT);

    DeleteBuffers(BUFFER_COUNT, buffers.data());
}
BENCHMARK(BM_UpdateDataPartially)->Unit(benchmark::kMillisecond)->UseRealTime();

int main(int argc, char** argv) {
    Initialize();
    benchmark::MaybeReenterWithoutASLR(argc, argv);
    char arg0_default[] = "benchmark";
    char* args_default = reinterpret_cast<char*>(arg0_default);
    if (!argv) {
        argc = 1;
        argv = &args_default;
    }
    ::benchmark::Initialize(&argc, argv);
    if (::benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    ::benchmark::RunSpecifiedBenchmarks();
    ::benchmark::Shutdown();
    return 0;
}
