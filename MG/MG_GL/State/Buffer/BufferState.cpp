//
// Created by BZLZHH on 2025/5/1.
//

// TODO: Add more gl error check for buffer state manager.

#include "BufferState.h"

GLenum BufferState::GenName(GLuint *buffer) {
    MG_Util::Debug::LogD("MG_State: Buffer: GenName");
    if (!buffer)
        return GL_INVALID_VALUE;

    GLuint id = 0;
    if (freeId_.empty()) {
        id = lastId_++;
    } else {
        id = freeId_.back();
        freeId_.pop_back();
    }

    *buffer = id;
    MG_Util::Debug::LogD("MG_State: Buffer: Gen new name %d", id);

    return GL_NO_ERROR;
}

GLenum BufferState::GenNameN(GLsizei n, GLuint* buffers) {
    MG_Util::Debug::LogD("MG_State: Buffer: GenNameN called with n=%d", n);
    if (n < 0) return GL_INVALID_VALUE;

    for (GLsizei i = 0; i < n; ++i) {
        GLenum result = GenName(&buffers[i]);
        if (result != GL_NO_ERROR) {
            MG_Util::Debug::LogE("MG_State: Buffer: GenNameN failed with error 0x%x", result);
            return result;
        }
    }
    MG_Util::Debug::LogD("MG_State: Buffer: GenNameN created buffers successfully");
    return GL_NO_ERROR;
}

GLenum BufferState::Create(GLuint buffer) {
    MG_Util::Debug::LogD("MG_State: Buffer: Create called");
    if (!buffer)
        return GL_INVALID_VALUE;

    if (ValidateAllocatedHandle(buffer))
        return GL_INVALID_VALUE;

    BufferObject& obj = buffers_[buffer];
    MG_Util::Debug::LogD("MG_State: Buffer: Create created buffer %d", buffer);
    obj.generated = true;
    obj.dirty = true;

    return GL_NO_ERROR;
}

GLenum BufferState::Bind(GLenum target, GLuint buffer) {
    // We don't handle unallocated buffer names here, just plain bind
    if (!IsValidTarget_(target)) return GL_INVALID_ENUM;
    MG_Util::Debug::LogD("MG_State: Buffer: Bind called with target=%s, buffer=%u", MG_Util::Debug::GLEnumToString(target), buffer);
    currentBindings_[target] = buffer;
    MG_Util::Debug::LogD("MG_State: Buffer: Bind succeed bind buffer %u to target 0x%x", buffer, target);
    return GL_NO_ERROR;
}

GLenum BufferState::CommitStorage(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    MG_Util::Debug::LogD("MG_State: Buffer: CommitStorage called on target 0x%x",target);
    auto it = currentBindings_.find(target);
    if (it == currentBindings_.end() || it->second == 0)
        return GL_INVALID_OPERATION;

    BufferObject& obj = buffers_[it->second];
    MG_Util::Debug::LogD("MG_State: Buffer: CommitStorage get buffer object %u at target 0x%x",it->second,target);
    obj.usage = usage;
    obj.data.resize(size);
    if (data) {
        memcpy(obj.data.data(), data, size);
        obj.dataValid = true;
    }
    obj.dirty = true;
    MG_Util::Debug::LogD("MG_State: Buffer: CommitStorage buffer at target 0x%x committed storage, size = %zu, usage=0x%x", target, size, usage);

    return GL_NO_ERROR;
}

GLenum BufferState::CommitStorageRegion(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    MG_Util::Debug::LogD("MG_State: Buffer: BufferSubData called on target 0x%x, offset=%ld, size=%ld", target, offset, size);
    if (!IsValidTarget_(target)) return GL_INVALID_ENUM;
    if (offset < 0 || size < 0) return GL_INVALID_VALUE;

    auto it = currentBindings_.find(target);
    if (it == currentBindings_.end() || it->second == 0)
        return GL_INVALID_OPERATION;

    BufferObject& obj = buffers_[it->second];
    MG_Util::Debug::LogD("MG_State: Buffer: BufferSubData get buffer object %u at target 0x%x", it->second, target);

    if (static_cast<size_t>(offset + size) > obj.data.size()) return GL_INVALID_VALUE;

    if (data) {
        memcpy(obj.data.data() + offset, data, size);
        obj.dirty = true;
    }
    return GL_NO_ERROR;
}

GLenum BufferState::AcquireBufferMemoryRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access, void** mappedPointer) {
    MG_Util::Debug::LogD("MG_State: Buffer: AcquireBufferMemoryRange called with target=0x%x, offset=%ld, length=%ld, access=0x%x", target, offset, length, access);
    if (!IsValidTarget_(target)) {
        MG_Util::Debug::LogE("MG_State: Buffer: AcquireBufferMemoryRange failed: invalid target 0x%x", target);
        return GL_INVALID_ENUM;
    }
    if (offset < 0 || length <= 0) {
        MG_Util::Debug::LogE("MG_State: Buffer: AcquireBufferMemoryRange failed: invalid offset %ld or length %ld", offset, length);
        return GL_INVALID_VALUE;
    }

    auto it = currentBindings_.find(target);
    if (it == currentBindings_.end() || it->second == 0) {
        MG_Util::Debug::LogE("MG_State: Buffer: AcquireBufferMemoryRange failed: no buffer bound to target 0x%x", target);
        return GL_INVALID_OPERATION;
    }

    BufferObject& obj = buffers_[it->second];
    MG_Util::Debug::LogD("MG_State: Buffer: AcquireBufferMemoryRange operating on buffer %u", it->second);
    if (obj.isMapped) {
        MG_Util::Debug::LogE("MG_State: Buffer: AcquireBufferMemoryRange failed: buffer %u is already mapped", it->second);
        return GL_INVALID_OPERATION;
    }

    const GLbitfield validFlags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_BUFFER_BIT ;
    if ((access & ~validFlags) != 0) {
        MG_Util::Debug::LogE("MG_State: Buffer: AcquireBufferMemoryRange failed: invalid access flags 0x%x", access);
        return GL_INVALID_VALUE;
    }

    if (static_cast<size_t>(offset + length) > obj.data.size()) {
        MG_Util::Debug::LogE("MG_State: Buffer: AcquireBufferMemoryRange failed: requested range [%ld, %ld) exceeds buffer size %zu", offset, offset + length, obj.data.size());
        return GL_INVALID_VALUE;
    }

    obj.isMapped = true;
    obj.mapOffset = offset;
    obj.mapLength = length;
    obj.mapAccessFlags = access;
    obj.dirty = true;
    *mappedPointer = obj.data.data() + offset;
    MG_Util::Debug::LogD("MG_State: Buffer: AcquireBufferMemoryRange succeeded for buffer %u. Mapped pointer: %p", it->second, *mappedPointer);
    return GL_NO_ERROR;
}

GLenum BufferState::SyncBufferMemory(GLenum target, GLintptr offset, GLsizeiptr length) {
    if (!IsValidTarget_(target)) return GL_INVALID_ENUM;
    if (offset < 0 || length <= 0) return GL_INVALID_VALUE;

    auto it = currentBindings_.find(target);
    if (it == currentBindings_.end() || it->second == 0)
        return GL_INVALID_OPERATION;

    BufferObject& obj = buffers_[it->second];
    if (!obj.isMapped || !(obj.mapAccessFlags & GL_MAP_FLUSH_EXPLICIT_BIT))
        return GL_INVALID_OPERATION;

    if (offset < obj.mapOffset || offset + length > obj.mapOffset + obj.mapLength)
        return GL_INVALID_VALUE;

    // TODO: Add support for flush range.
    // Now only mark it as dirty.
    obj.dirty = true;
    return GL_NO_ERROR;
}

GLenum BufferState::CopyBufferRange(GLenum readTarget, GLenum writeTarget, GLintptr readOffset, GLintptr writeOffset, GLsizeiptr size) {
    if (!IsValidTarget_(readTarget) || !IsValidTarget_(writeTarget))
        return GL_INVALID_ENUM;
    if (readOffset < 0 || writeOffset < 0 || size < 0)
        return GL_INVALID_VALUE;

    GLuint readBuffer = currentBindings_[readTarget];
    GLuint writeBuffer = currentBindings_[writeTarget];
    if (readBuffer == 0 || writeBuffer == 0 || readBuffer == writeBuffer)
        return GL_INVALID_OPERATION;

    BufferObject& src = buffers_[readBuffer];
    BufferObject& dst = buffers_[writeBuffer];

    if (static_cast<size_t>(readOffset + size) > src.data.size() ||
        static_cast<size_t>(writeOffset + size) > dst.data.size())
        return GL_INVALID_VALUE;

    memcpy(dst.data.data() + writeOffset, src.data.data() + readOffset, size);
    dst.dirty = true;
    return GL_NO_ERROR;
}

GLenum BufferState::AcquireBufferMemory(GLenum target, GLenum access, void** mappedPointer) {
    GLbitfield flags = 0;
    switch (access) {
        case GL_READ_ONLY:  flags = GL_MAP_READ_BIT; break;
        case GL_WRITE_ONLY: flags = GL_MAP_WRITE_BIT; break;
        case GL_READ_WRITE: flags = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT; break;
        default:
            flags = access;
    }
    return AcquireBufferMemoryRange(target, 0, buffers_[currentBindings_[target]].data.size(), flags, mappedPointer);
}

GLenum BufferState::ReleaseBufferMemory(GLenum target) {
    if (!IsValidTarget_(target))
        return GL_INVALID_ENUM;

    auto it = currentBindings_.find(target);
    if (it == currentBindings_.end() || it->second == 0)
        return GL_INVALID_OPERATION;

    BufferObject& obj = buffers_[it->second];
    if (!obj.isMapped)
        return GL_INVALID_OPERATION;

    if ((obj.mapAccessFlags & GL_MAP_FLUSH_EXPLICIT_BIT) == 0) {
        obj.dirty = true;
    }

    obj.isMapped = false;
    obj.mapOffset = 0;
    obj.mapLength = 0;
    obj.mapAccessFlags = 0;
    obj.accessMode = GL_READ_WRITE;

    return GL_NO_ERROR;
}

bool BufferState::ValidateAllocatedHandle(GLuint buffer) {
    bool isValid = buffers_.find(buffer) != buffers_.end();
    MG_Util::Debug::LogD("MG_State: Buffer: ValidateAllocatedHandle called on buffer %u returns %d", buffer, isValid);
    return isValid;
}

bool BufferState::ValidateGeneratedName(GLuint buffer) {
    bool inFreeList = std::find(freeId_.begin(), freeId_.end(), buffer) != freeId_.end();
    bool lessThanLast = buffer < lastId_; // lastId_ is not generated yet
    MG_Util::Debug::LogD("MG_State: Buffer: ValidateGeneratedName called on buffer %u returns %d", buffer, lessThanLast && !inFreeList);
    return lessThanLast && !inFreeList;
}

void BufferState::Delete(GLuint buffer) {
    buffers_.erase(buffer);
    if (ValidateGeneratedName(buffer))
        // TODO: Prevent the object that uses a freeId from conflicting with legacy object in the backend.
        //freeId_.emplace_back(buffer);
    for (auto& [target, id] : currentBindings_) {
        if (id == buffer) id = 0;
    }
    MG_Util::Debug::LogD("MG_State: Buffer: Delete buffer %u", buffer);
}

GLenum BufferState::DeleteN(GLsizei n, const GLuint* buffers) {
    MG_Util::Debug::LogD("MG_State: Buffer: DeleteN called with n=%d", n);
    if (n < 0) return GL_INVALID_VALUE;

    for (GLsizei i = 0; i < n; ++i) {
        Delete(buffers[i]);
    }
    MG_Util::Debug::LogD("MG_State: Buffer: DeleteN deleted buffers successfully");
    return GL_NO_ERROR;
}

bool BufferState::IsValidTarget_(GLenum target) {
    MG_Util::Debug::LogD("MG_State: Buffer: IsValidTarget_ called with target=0x%x,result=%d", target, !(MG_Constants::Buffer::VALID_TARGETS.find(target) == MG_Constants::Buffer::VALID_TARGETS.end()));
    return !(MG_Constants::Buffer::VALID_TARGETS.find(target) == MG_Constants::Buffer::VALID_TARGETS.end());
}

GLenum BufferState::QueryPropertyIntVector(GLenum target, GLenum pname, GLint* params) const {
    MG_Util::Debug::LogD("MG_State: Buffer: QueryPropertyIntVector called at target 0x%x,param name = 0x%x",target,pname);
    static const std::set<GLenum> validTargets = {
            GL_ARRAY_BUFFER, GL_ELEMENT_ARRAY_BUFFER,
            GL_PIXEL_PACK_BUFFER, GL_PIXEL_UNPACK_BUFFER
    };
    if (validTargets.find(target) == validTargets.end()) {
        return GL_INVALID_ENUM;
    }
    
    if (MG_Constants::Buffer::VALID_PARAM_NAMES.find(pname) == MG_Constants::Buffer::VALID_PARAM_NAMES.end()) {
        return GL_INVALID_ENUM;
    }
    
    auto bindingIt = currentBindings_.find(target);
    if (bindingIt == currentBindings_.end() || bindingIt->second == 0) {
        return GL_INVALID_OPERATION;
    }
    GLuint bufferId = bindingIt->second;
    auto bufferIt = buffers_.find(bufferId);
    if (bufferIt == buffers_.end()) {
        return GL_INVALID_OPERATION;
    }
    
    const BufferObject& buffer = bufferIt->second;
    
    switch (pname) {
        case GL_BUFFER_ACCESS:
            *params = static_cast<GLint>(buffer.accessMode);
            break;
        case GL_BUFFER_MAPPED:
            *params = buffer.isMapped ? GL_TRUE : GL_FALSE;
            break;
        case GL_BUFFER_SIZE:
            *params = static_cast<GLint>(buffer.data.size());
            break;
        case GL_BUFFER_USAGE:
            *params = static_cast<GLint>(buffer.usage);
            break;
        MG_Util::Debug::LogD("MG_State: Buffer: QueryPropertyIntVector Query info about buffer %u succeed", target);
        default:
            return GL_INVALID_ENUM;
    }
    return GL_NO_ERROR;
}


GLuint BufferState::GetCurrentBinding(GLenum target) const {
    auto it = currentBindings_.find(target);
    if (it != currentBindings_.end()) {
        return it->second;
    }
    return 0;
}