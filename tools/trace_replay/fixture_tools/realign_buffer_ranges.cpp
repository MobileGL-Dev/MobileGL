// realign_buffer_ranges: make a GL trace's indexed uniform / storage buffer range binds portable.
//
// An app sizes glBindBufferRange offsets by the GL_{UNIFORM,SHADER_STORAGE}_BUFFER_OFFSET_ALIGNMENT
// of the driver it was captured on (Flywheel's staging scatter lists: 16 on desktop). A driver that
// requires more rejects those binds with GL_INVALID_VALUE. This tool rewrites every such bind whose
// offset is not a multiple of --align:
//   - the bind itself becomes glBindBuffer(target, buffer) (the generic binding it also set);
//   - right before the next draw / dispatch, the bytes the range holds at that point are uploaded into a
//     fresh --align-aligned slot of one relocation buffer, and that slot is bound at the same index.
// The bytes come from the trace itself (buffer data / subdata and the persistent-map memcpy commits), so a
// source buffer written any other way (GPU copies, clears) is refused rather than guessed.
//
// Inserting calls renumbers every later call: --map-call N prints where original call N landed (a case's
// target_call). The output is snappy-compressed; `apitrace repack --brotli` it before packaging.
//
// Build (A = 3rdparty/apitrace, B = a Release build of it, e.g. `cmake -S $A -B $B -G Ninja -DENABLE_GUI=OFF
// && ninja -C $B apitrace`):
//   g++ -std=c++17 -O2 -flto=auto -fno-rtti -fno-exceptions -I$A/lib/trace -I$A/lib/os -I$B -I$B/lib/trace \
//     realign_buffer_ranges.cpp -o realign_buffer_ranges $B/lib/trace/libcommon.a $B/lib/guids/libguids.a \
//     $B/lib/highlight/libhighlight.a $B/lib/os/libos.a $B/thirdparty/libbacktrace.a $B/thirdparty/libsnappy.a \
//     $B/thirdparty/libzstd_seekable.a -lbrotlidec -lbrotlienc -lz -lzstd -lpthread -ldl
// Usage: realign_buffer_ranges [--align N] [--map-call N]... in.trace out.trace

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "trace_model.hpp"
#include "trace_parser.hpp"
#include "trace_writer.hpp"

namespace {

constexpr unsigned kUniformBuffer = 0x8A11;
constexpr unsigned kShaderStorageBuffer = 0x90D2;
constexpr unsigned kDynamicStorageBit = 0x0100;
constexpr unsigned kRelocBufferName = 1000000;

[[noreturn]] void Fail(const trace::Call *call, const std::string &what) {
    std::fprintf(stderr, "error: call %u %s: %s\n", call ? call->no : 0u, call ? call->name() : "-", what.c_str());
    std::exit(1);
}

bool Is(const trace::Call *call, const char *name) { return std::strcmp(call->name(), name) == 0; }

bool StartsWith(const char *s, const char *prefix) { return std::strncmp(s, prefix, std::strlen(prefix)) == 0; }

bool ReadsIndexedBuffers(const trace::Call *call) {
    const char *n = call->name();
    return StartsWith(n, "glDraw") || StartsWith(n, "glMultiDraw") || StartsWith(n, "glDispatchCompute");
}

bool IsRangeTarget(unsigned target) { return target == kUniformBuffer || target == kShaderStorageBuffer; }

struct MapRegion {
    unsigned buffer;
    uint64_t offset;
    uint64_t length;
    uint64_t ptr;
};

struct Reloc {
    unsigned src = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
    bool dirty = true;
    std::vector<char> last;
    const trace::EnumSig *targetSig = nullptr;
};

// Signatures for the calls this tool inserts; ids are allocated past the trace's own.
struct Sigs {
    const char *createNames[2] = {"n", "buffers"};
    const char *storageNames[4] = {"buffer", "size", "data", "flags"};
    const char *subDataNames[4] = {"buffer", "offset", "size", "data"};
    const char *bindRangeNames[5] = {"target", "index", "buffer", "offset", "size"};
    const char *bindNames[2] = {"target", "buffer"};
    trace::FunctionSig create{}, storage{}, subData{}, bindRange{}, bind{};

    void Init(trace::Id firstId) {
        create = {firstId + 0, "glCreateBuffers", 2, createNames};
        storage = {firstId + 1, "glNamedBufferStorage", 4, storageNames};
        subData = {firstId + 2, "glNamedBufferSubData", 4, subDataNames};
        bindRange = {firstId + 3, "glBindBufferRange", 5, bindRangeNames};
        bind = {firstId + 4, "glBindBuffer", 2, bindNames};
    }
};

class Rewriter {
public:
    uint64_t align = 1024;
    std::set<unsigned> mapCalls;

    int Run(const char *in, const char *out) {
        Scan(in);
        if (relocSrc_.empty()) {
            std::fprintf(stderr, "nothing to realign at %llu bytes\n", (unsigned long long)align);
            return 2;
        }
        sigs_.Init(maxSigId_ + 1);
        stride_ = (maxRelocSize_ + align - 1) / align * align;
        Pass(in, nullptr);  // dry run: count the slots
        slotCap_ = slotsUsed_;
        Pass(in, out);
        std::fprintf(stderr,
                     "realigned %llu binds of %zu source buffer(s) to %llu bytes: %llu uploads, relocation buffer %llu "
                     "bytes, %llu calls inserted\n",
                     (unsigned long long)relocBinds_, relocSrc_.size(), (unsigned long long)align,
                     (unsigned long long)slotsUsed_, (unsigned long long)(slotCap_ * stride_),
                     (unsigned long long)inserted_);
        for (const auto &m : callMap_) {
            std::printf("call %u -> %llu\n", m.first, (unsigned long long)m.second);
        }
        return 0;
    }

private:
    // Pass 0: which buffers need relocating, and the sizes involved.
    void Scan(const char *in) {
        trace::Parser p;
        if (!p.open(in)) Fail(nullptr, std::string("cannot open ") + in);
        trace::Call *call;
        while ((call = p.parse_call())) {
            maxSigId_ = std::max(maxSigId_, call->sig->id);
            if (Is(call, "glBindBufferRange")) {
                unsigned target = unsigned(call->arg(0).toSInt());
                uint64_t offset = uint64_t(call->arg(3).toSInt());
                if (IsRangeTarget(target) && offset % align != 0) {
                    relocSrc_.insert(unsigned(call->arg(2).toUInt()));
                    maxRelocSize_ = std::max(maxRelocSize_, uint64_t(call->arg(4).toSInt()));
                }
            } else if (Is(call, "glBindBuffersRange")) {
                unsigned target = unsigned(call->arg(0).toSInt());
                const trace::Array *offsets = call->arg(4).toArray();
                if (IsRangeTarget(target) && offsets) {
                    for (const trace::Value *v : offsets->values) {
                        if (uint64_t(v->toSInt()) % align != 0) Fail(call, "misaligned glBindBuffersRange is not handled");
                    }
                }
            }
            delete call;
        }
    }

    void Pass(const char *in, const char *out) {
        trace::Parser p;
        if (!p.open(in)) Fail(nullptr, std::string("cannot open ") + in);
        trace::Writer writer;
        writer_ = nullptr;
        if (out) {
            if (!writer.open(out, p.getVersion(), p.getProperties())) Fail(nullptr, std::string("cannot create ") + out);
            writer_ = &writer;
        }
        shadow_.clear();
        maps_.clear();
        generic_.clear();
        relocs_.clear();
        slotsUsed_ = 0;
        relocBinds_ = 0;
        inserted_ = 0;
        created_ = false;

        trace::Call *call;
        while ((call = p.parse_call())) {
            Process(call);
            delete call;
        }
        if (writer_) writer.close();
        writer_ = nullptr;
    }

    unsigned BufferArg(trace::Call *call, unsigned index) { return unsigned(call->arg(index).toUInt()); }

    unsigned Generic(unsigned target) {
        auto it = generic_.find(target);
        return it == generic_.end() ? 0 : it->second;
    }

    std::vector<char> *Shadow(unsigned buffer) {
        auto it = shadow_.find(buffer);
        return it == shadow_.end() ? nullptr : &it->second;
    }

    void Specify(trace::Call *call, unsigned buffer, uint64_t size, trace::Value *data) {
        if (!relocSrc_.count(buffer)) return;
        std::vector<char> &s = shadow_[buffer];
        s.assign(size, 0);
        const trace::Blob *blob = data ? data->toBlob() : nullptr;
        if (blob) std::memcpy(s.data(), blob->buf, std::min<uint64_t>(size, blob->size));
        for (auto &r : relocs_) {
            if (r.second.src == buffer) r.second.dirty = true;
        }
        (void)call;
    }

    void Write(trace::Call *call, unsigned buffer, uint64_t offset, const char *bytes, uint64_t size) {
        std::vector<char> *s = Shadow(buffer);
        if (!s) return;
        if (offset + size > s->size()) Fail(call, "write past the end of a tracked buffer");
        std::memcpy(s->data() + offset, bytes, size);
    }

    void Refuse(trace::Call *call, unsigned buffer) {
        if (relocSrc_.count(buffer)) Fail(call, "writes a relocated source buffer in a way the trace does not carry the bytes of");
    }

    void Emit(trace::Call *call) {
        if (!writer_) return;
        if (mapCalls.count(call->no)) callMap_[call->no] = writtenCalls_;
        writer_->writeCall(call);
        ++writtenCalls_;
    }

    void EmitNew(trace::Call *call) {
        ++inserted_;
        if (writer_) {
            writer_->writeCall(call);
            ++writtenCalls_;
        }
        delete call;
    }

    trace::Call *NewCall(const trace::FunctionSig *sig, unsigned thread) {
        return new trace::Call(sig, trace::CallFlags(0), thread);
    }

    void EmitBind(unsigned thread, const trace::EnumSig *targetSig, unsigned target, unsigned buffer) {
        trace::Call *c = NewCall(&sigs_.bind, thread);
        c->args[0].value = targetSig ? static_cast<trace::Value *>(new trace::Enum(targetSig, target))
                                     : static_cast<trace::Value *>(new trace::SInt(target));
        c->args[1].value = new trace::UInt(buffer);
        EmitNew(c);
    }

    void CreateRelocBuffer(unsigned thread) {
        trace::Call *c = NewCall(&sigs_.create, thread);
        c->args[0].value = new trace::SInt(1);
        trace::Array *names = new trace::Array(1);
        names->values[0] = new trace::UInt(kRelocBufferName);
        c->args[1].value = names;
        EmitNew(c);

        c = NewCall(&sigs_.storage, thread);
        c->args[0].value = new trace::UInt(kRelocBufferName);
        c->args[1].value = new trace::SInt(int64_t(slotCap_ * stride_));
        c->args[2].value = new trace::Null();
        c->args[3].value = new trace::UInt(kDynamicStorageBit);
        EmitNew(c);
        created_ = true;
    }

    void Upload(trace::Call *before, unsigned target, unsigned index, Reloc &r) {
        std::vector<char> *s = Shadow(r.src);
        if (!s) Fail(before, "a relocated range reads a buffer whose storage the trace never specified");
        if (r.offset + r.size > s->size()) Fail(before, "a relocated range runs past its buffer");
        std::vector<char> bytes(s->begin() + r.offset, s->begin() + r.offset + r.size);
        if (!r.dirty && bytes == r.last) return;

        unsigned thread = before->thread_id;
        if (writer_ && slotsUsed_ >= slotCap_) Fail(before, "relocation slots exceeded the dry run");
        uint64_t slotOffset = slotsUsed_++ * stride_;
        if (!created_) CreateRelocBuffer(thread);

        trace::Call *c = NewCall(&sigs_.subData, thread);
        c->args[0].value = new trace::UInt(kRelocBufferName);
        c->args[1].value = new trace::SInt(int64_t(slotOffset));
        c->args[2].value = new trace::SInt(int64_t(r.size));
        trace::Blob *blob = new trace::Blob(r.size);
        std::memcpy(blob->buf, bytes.data(), r.size);
        c->args[3].value = blob;
        EmitNew(c);

        c = NewCall(&sigs_.bindRange, thread);
        c->args[0].value = r.targetSig ? static_cast<trace::Value *>(new trace::Enum(r.targetSig, target))
                                       : static_cast<trace::Value *>(new trace::SInt(target));
        c->args[1].value = new trace::UInt(index);
        c->args[2].value = new trace::UInt(kRelocBufferName);
        c->args[3].value = new trace::SInt(int64_t(slotOffset));
        c->args[4].value = new trace::SInt(int64_t(r.size));
        EmitNew(c);

        // glBindBufferRange also moved the generic binding; put back the app's.
        EmitBind(thread, r.targetSig, target, Generic(target));

        r.last = std::move(bytes);
        r.dirty = false;
    }

    void Process(trace::Call *call) {
        const char *n = call->name();

        if (Is(call, "glBindBuffer")) {
            generic_[unsigned(call->arg(0).toSInt())] = BufferArg(call, 1);
        } else if (Is(call, "glBindBufferBase")) {
            unsigned target = unsigned(call->arg(0).toSInt());
            generic_[target] = BufferArg(call, 2);
            relocs_.erase({target, unsigned(call->arg(1).toUInt())});
        } else if (Is(call, "glBindBufferRange")) {
            unsigned target = unsigned(call->arg(0).toSInt());
            unsigned index = unsigned(call->arg(1).toUInt());
            unsigned buffer = BufferArg(call, 2);
            uint64_t offset = uint64_t(call->arg(3).toSInt());
            generic_[target] = buffer;
            if (IsRangeTarget(target) && offset % align != 0) {
                Reloc r;
                r.src = buffer;
                r.offset = offset;
                r.size = uint64_t(call->arg(4).toSInt());
                trace::Value &t = call->arg(0);
                trace::Enum *e = nullptr;
                // Enum values print by name in dumps; keep the trace's own enum signature when it has one.
                struct EnumProbe : trace::Visitor {
                    trace::Enum *found = nullptr;
                    void visit(trace::Enum *node) override { found = node; }
                } probe;
                t.visit(probe);
                e = probe.found;
                r.targetSig = e ? e->sig : nullptr;
                relocs_[{target, index}] = std::move(r);
                ++relocBinds_;
                if (mapCalls.count(call->no) && writer_) callMap_[call->no] = writtenCalls_;
                // Only the generic binding survives at this point; the indexed one is bound before its first read.
                EmitBind(call->thread_id, e ? e->sig : nullptr, target, buffer);
                --inserted_;  // a replacement, not an insertion
                return;
            }
            relocs_.erase({target, index});
        } else if (Is(call, "glBindBuffersBase") || Is(call, "glBindBuffersRange")) {
            unsigned target = unsigned(call->arg(0).toSInt());
            unsigned first = unsigned(call->arg(1).toUInt());
            unsigned count = unsigned(call->arg(2).toSInt());
            for (unsigned i = first; i < first + count; ++i) relocs_.erase({target, i});
        } else if (Is(call, "glNamedBufferStorage") || Is(call, "glNamedBufferData")) {
            Specify(call, BufferArg(call, 0), uint64_t(call->arg(1).toSInt()), &call->arg(2));
        } else if (Is(call, "glBufferStorage") || Is(call, "glBufferData")) {
            Specify(call, Generic(unsigned(call->arg(0).toSInt())), uint64_t(call->arg(1).toSInt()), &call->arg(2));
        } else if (Is(call, "glNamedBufferSubData") || Is(call, "glBufferSubData")) {
            unsigned buffer = Is(call, "glNamedBufferSubData") ? BufferArg(call, 0) : Generic(unsigned(call->arg(0).toSInt()));
            if (relocSrc_.count(buffer)) {
                const trace::Blob *blob = call->arg(3).toBlob();
                if (!blob) Fail(call, "subdata without bytes");
                Write(call, buffer, uint64_t(call->arg(1).toSInt()), blob->buf,
                      std::min<uint64_t>(uint64_t(call->arg(2).toSInt()), blob->size));
            }
        } else if (Is(call, "glCopyNamedBufferSubData")) {
            Refuse(call, BufferArg(call, 1));
        } else if (Is(call, "glCopyBufferSubData")) {
            Refuse(call, Generic(unsigned(call->arg(1).toSInt())));
        } else if (StartsWith(n, "glClearNamedBuffer")) {
            Refuse(call, BufferArg(call, 0));
        } else if (StartsWith(n, "glClearBuffer") && (StartsWith(n, "glClearBufferData") || StartsWith(n, "glClearBufferSubData"))) {
            Refuse(call, Generic(unsigned(call->arg(0).toSInt())));
        } else if (Is(call, "glMapNamedBufferRange") || Is(call, "glMapBufferRange")) {
            unsigned buffer = Is(call, "glMapNamedBufferRange") ? BufferArg(call, 0) : Generic(unsigned(call->arg(0).toSInt()));
            if (relocSrc_.count(buffer) && call->ret) {
                maps_.push_back({buffer, uint64_t(call->arg(1).toSInt()), uint64_t(call->arg(2).toSInt()),
                                 call->ret->toUIntPtr()});
            }
        } else if (Is(call, "glMapNamedBuffer") || Is(call, "glMapBuffer")) {
            unsigned buffer = Is(call, "glMapNamedBuffer") ? BufferArg(call, 0) : Generic(unsigned(call->arg(0).toSInt()));
            std::vector<char> *s = Shadow(buffer);
            if (s && call->ret) maps_.push_back({buffer, 0, s->size(), call->ret->toUIntPtr()});
        } else if (Is(call, "glUnmapNamedBuffer") || Is(call, "glUnmapBuffer")) {
            unsigned buffer = Is(call, "glUnmapNamedBuffer") ? BufferArg(call, 0) : Generic(unsigned(call->arg(0).toSInt()));
            maps_.erase(std::remove_if(maps_.begin(), maps_.end(), [&](const MapRegion &m) { return m.buffer == buffer; }),
                        maps_.end());
        } else if (Is(call, "memcpy")) {
            uint64_t dest = call->arg(0).toUIntPtr();
            const trace::Blob *blob = call->arg(1).toBlob();
            uint64_t size = call->arg(2).toUInt();
            if (blob) {
                size = std::min<uint64_t>(size, blob->size);
                for (const MapRegion &m : maps_) {
                    uint64_t lo = std::max(dest, m.ptr);
                    uint64_t hi = std::min(dest + size, m.ptr + m.length);
                    if (lo >= hi) continue;
                    Write(call, m.buffer, m.offset + (lo - m.ptr), blob->buf + (lo - dest), hi - lo);
                }
            }
        } else if (ReadsIndexedBuffers(call)) {
            for (auto &r : relocs_) Upload(call, r.first.first, r.first.second, r.second);
        }

        Emit(call);
    }

    Sigs sigs_;
    trace::Id maxSigId_ = 0;
    std::set<unsigned> relocSrc_;
    uint64_t maxRelocSize_ = 0;
    uint64_t stride_ = 0;
    uint64_t slotCap_ = 0;
    uint64_t slotsUsed_ = 0;
    uint64_t relocBinds_ = 0;
    uint64_t inserted_ = 0;
    uint64_t writtenCalls_ = 0;
    bool created_ = false;
    trace::Writer *writer_ = nullptr;
    std::unordered_map<unsigned, std::vector<char>> shadow_;
    std::vector<MapRegion> maps_;
    std::unordered_map<unsigned, unsigned> generic_;
    std::map<std::pair<unsigned, unsigned>, Reloc> relocs_;
    std::map<unsigned, uint64_t> callMap_;
};

}  // namespace

int main(int argc, char **argv) {
    Rewriter rw;
    std::vector<const char *> files;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--align") && i + 1 < argc) {
            rw.align = std::strtoull(argv[++i], nullptr, 0);
        } else if (!std::strcmp(argv[i], "--map-call") && i + 1 < argc) {
            rw.mapCalls.insert(unsigned(std::strtoul(argv[++i], nullptr, 0)));
        } else {
            files.push_back(argv[i]);
        }
    }
    if (files.size() != 2 || rw.align == 0) {
        std::fprintf(stderr, "usage: %s [--align N] [--map-call N]... in.trace out.trace\n", argv[0]);
        return 1;
    }
    return rw.Run(files[0], files[1]);
}
