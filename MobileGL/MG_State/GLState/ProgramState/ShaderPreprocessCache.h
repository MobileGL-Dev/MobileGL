// MobileGL - MobileGL/MG_State/GLState/ProgramState/ShaderPreprocessCache.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v3.0:
//   https://www.gnu.org/licenses/gpl-3.0.txt
//   https://www.gnu.org/licenses/lgpl-3.0.txt
// SPDX-License-Identifier: LGPL-3.0-only
// End of Source File Header

#pragma once
#include <Includes.h>
#include <list>
#include <mutex>
// Deliberately NOT ShaderObject.h: ShaderCompileTask.h needs this header, and ShaderObject.h
// needs ShaderCompileTask.h. Only ShaderStage was ever used from there.
#include <MG_State/GLState/ProgramState/ShaderStage.h>
#include <MG_State/GLState/ProgramState/ShaderSourceKey.h>

namespace MobileGL::MG_State::GLState {
    // Where the shared, source-only half of ShaderObject::Compile() stopped. The two
    // rejection verdicts are kept apart (rather than collapsed into "failed") so a hit
    // reproduces the original diagnosis, not just the original info log.
    enum class ShaderPreprocessOutcome : Uint8 {
        // The source-only half ran clean; preprocessedSource and both maps are valid.
        Preprocessed,
        // ValidateComputeLocalSizeLimits rejected it (compute only).
        ComputeLocalSizeRejected,
        // FindReservedIdentifierViolation rejected it.
        ReservedIdentifierRejected,
        // FindShaderStorageBindingViolation rejected it: a storage block declared a binding at or
        // past GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS.
        ResourceBindingRejected,
        // The source-only half was clean but glslang rejected the preprocessed source.
        // Memoizing this saves the parse itself on every later object with that source.
        ParseFailed,
    };

    // Everything ShaderObject::Compile() derives from the source text alone, i.e.
    // everything that is identical for two shader objects holding byte-identical source.
    //
    // "The source text alone" is now literally true: the preprocessed text, an accept/reject
    // verdict, and the log that explains a rejection. Anything that needs to know what the
    // shader MEANS is derived from the parse instead - see the note on the missing fields.
    struct ShaderPreprocessResult {
        ShaderPreprocessOutcome outcome = ShaderPreprocessOutcome::Preprocessed;
        // Valid unless the preprocessor itself never ran; kept even for the rejection
        // outcomes because that is the text the diagnostics refer to.
        String preprocessedSource;
        // NO EXTRACTED SIDE CHANNELS ANY MORE, and their absence is the point. Explicit
        // uniform locations, explicit opaque bindings and unqualified storage blocks used to be
        // lexed out of the text here, which meant reading MACRO-UNEXPANDED source: MobileGL's
        // preprocessor rewrites the text, it does not run the C preprocessor, so
        // `binding = SOME_MACRO` reached the scanners verbatim. All three now come from
        // glslang - the first from a snapshot taken inside the parse, the other two from the
        // IO mapper's collect callback - and none of them is a function of the source text
        // ALONE any more, which is the only thing this struct is allowed to hold.
        // The compile info log to publish; empty when outcome == Preprocessed.
        String infoLog;

        Bool Preprocessed() const { return outcome == ShaderPreprocessOutcome::Preprocessed; }
    };

    // Cache hits hand out shared ownership, not a raw pointer into the entry list. That is
    // what makes the cache safe once compiles run concurrently: a reader keeps its payload
    // alive across any eviction, and a 107 KB preprocessedSource is never copied on a hit.
    using ShaderPreprocessResultPtr = SharedPtr<const ShaderPreprocessResult>;

    // P0b layer 2: a per-context, bounded memo of the source-only half of shader
    // compilation, keyed by (stage, xxhash64(source), source length).
    //
    // Motivation: in the Iris shader-pack corpus ~21% of every glCompileShader in a trace
    // is a *different* shader object holding byte-identical source (packs glue the same
    // common/composite GLSL into many program stages), so the preprocess + reserved-
    // identifier scan + explicit-location/binding extraction runs over the same megabytes
    // again and again. Layer 1 (in ShaderObject) covers the same object recompiled with
    // unchanged source; this covers the cross-object case.
    //
    // What is NOT cached: the glslang parse. glslang's TShader is consume-once (mapIO
    // mutates the aliased intermediate at link), so every shader object still needs its
    // own parse; only the text-processing half is shared.
    //
    // Correctness: the 64-bit hash is a lookup accelerator only. Every hit re-compares the
    // full stored original source with memcmp before it is honored, so a hash collision
    // degrades to a miss, never to a wrong answer. That is why the full original text is
    // stored rather than a prefix/suffix digest - the cache is bounded, so the cost is.
    //
    // Eviction: FIFO (insertion order), bounded by BOTH an entry count and a stored-source
    // byte budget, whichever binds first. FIFO rather than LRU because shader-pack loading
    // is a burst of mostly-distinct sources whose reuse clusters around insertion time;
    // LRU's extra list splice on every hit buys nothing measurable here, and FIFO keeps
    // Find() a genuinely const, read-only operation.
    class ShaderPreprocessCache {
    public:
        static constexpr SizeT kMaxEntries = 128;
        static constexpr SizeT kMaxStoredSourceBytes = 8u * 1024u * 1024u;

        // Returns the memoized result for this exact source under this exact compile
        // environment, or null on a miss. The returned SharedPtr owns its payload, so it
        // stays valid for as long as the caller holds it - across Insert(), Clear(), and
        // across the destruction of the cache itself.
        //
        // envFingerprint joins the key because the source-only pipeline's compute
        // local-size verdict is computed against CompileEnv's device limits: a memo must
        // never outlive the environment it was computed against (memo-hazard rule).
        ShaderPreprocessResultPtr Find(ShaderStage stage, Uint64 sourceHash, const String& source,
                                       Uint64 envFingerprint) const;

        // Memoizes `result` for this source. A source whose own storage cost already
        // exceeds the byte budget is simply not cached (caching it would evict everything
        // else and then itself).
        void Insert(ShaderStage stage, Uint64 sourceHash, const String& source, Uint64 envFingerprint,
                    ShaderPreprocessResultPtr result);

        void Clear();

        static Uint64 HashSource(const String& source) {
            return static_cast<Uint64>(XXH64(source.data(), source.length(), 0));
        }

        SizeT GetEntryCount() const {
            const std::lock_guard<std::mutex> lock(m_mutex);
            return m_entries.size();
        }
        SizeT GetStoredSourceBytes() const {
            const std::lock_guard<std::mutex> lock(m_mutex);
            return m_storedSourceBytes;
        }

    private:
        // Shared with ShaderCompileAdoptionMap so the two per-context memos cannot key
        // themselves on different notions of "the same compile" - see ShaderSourceKey.h.
        using Key = ShaderSourceKey;
        using KeyHasher = ShaderSourceKeyHasher;

        struct Entry {
            Key key;
            // The full original (pre-preprocess) source, kept so a hit can be confirmed by
            // comparison instead of trusting the hash.
            String originalSource;
            ShaderPreprocessResultPtr result;
        };

        using EntryList = std::list<Entry>;

        static SizeT EntryBytes(const String& source, const ShaderPreprocessResult& result) {
            return source.length() + result.preprocessedSource.length();
        }

        void EvictUntilWithinBudgetLocked();

        void EraseEntryLocked(EntryList::iterator it);

        // P1: every public entry point takes this. The lock alone would NOT have been
        // enough - the old Find() handed back a raw pointer into an entry that a
        // concurrent Insert()'s FIFO eviction could erase while the caller was still
        // reading it. Shared ownership of the payload is what closes that hole; the mutex
        // only protects the containers below.
        mutable std::mutex m_mutex;
        EntryList m_entries;                                 // front = oldest (FIFO victim)
        UnorderedMap<Key, EntryList::iterator, KeyHasher> m_index;
        SizeT m_storedSourceBytes = 0;
    };
} // namespace MobileGL::MG_State::GLState
