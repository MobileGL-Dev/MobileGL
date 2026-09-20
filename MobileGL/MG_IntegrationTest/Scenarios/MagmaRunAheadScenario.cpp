// Real Magma run-ahead: observe client lead over an actually held apply thread,
// then verify GPU pixels/bytes. The scheduling hook never changes a watermark.
#include "../Harness/ScenarioFixture.h"
#include "../Harness/SplitRuntimePeek.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <initializer_list>
#include <thread>
#include <utility>
#include <vector>
#ifdef GLAPI
#undef GLAPI
#endif
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glcorearb.h>
#undef GL_GLEXT_PROTOTYPES

namespace MGITest {
namespace {
constexpr int kWidth = 32, kHeight = 8;
constexpr const char* kVertexId = R"(#version 430 core
void main() {
    vec2 p[3] = vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
    gl_Position=vec4(p[gl_VertexID],0,1);
})";
using Pixel = std::array<GLubyte, 4>;
constexpr Pixel red{255,0,0,255}, green{0,255,0,255}, blue{0,0,255,255}, yellow{255,255,0,255}, black{0,0,0,255};

struct HeldApply {
    ~HeldApply() { ReleaseSplitApplyHoldForTesting(); }
    void Release() { ReleaseSplitApplyHoldForTesting(); }
};

class MagmaRunAheadScenario : public ScenarioTest {
protected:
    GLuint target = 0;
    std::vector<GLuint> buffers, programs, textures, fbos, vaos;
    SplitRuntimeState held{};

    void SetUp() override {
        ScenarioTest::SetUp();
        if (!Ready()) return;
        if (!SplitLane::MarkerIsOne("MGITEST_MAGMA_RUNAHEAD_LANE"))
            GTEST_SKIP() << "dedicated Magma run-ahead lane only";
        ASSERT_EQ(Gl().BackendName(), "DirectVulkan");
        const auto runtime = PeekSplitRuntime();
        ASSERT_TRUE(runtime.peekAvailable && runtime.sessionActive && runtime.transportResolved);
        ASSERT_TRUE(runtime.runAheadArmed) << "Magma production session did not arm run-ahead";
        ASSERT_TRUE(runtime.presentCredit == 1 || runtime.presentCredit == 3);
        target = NewFbo(NewTexture(kWidth, kHeight, nullptr));
        glBindFramebuffer(GL_FRAMEBUFFER, target);
        glViewport(0, 0, kWidth, kHeight);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0,0,0,1);
        glClear(GL_COLOR_BUFFER_BIT);
        GLuint vao = 0;
        glGenVertexArrays(1, &vao);
        vaos.push_back(vao);
        glBindVertexArray(vao);
    }
    void TearDown() override {
        ReleaseSplitApplyHoldForTesting();
        if (!Ready()) return;
        glUseProgram(0);
        glBindVertexArray(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (!buffers.empty()) glDeleteBuffers(GLsizei(buffers.size()), buffers.data());
        if (!vaos.empty()) glDeleteVertexArrays(GLsizei(vaos.size()), vaos.data());
        if (!fbos.empty()) glDeleteFramebuffers(GLsizei(fbos.size()), fbos.data());
        if (!textures.empty()) glDeleteTextures(GLsizei(textures.size()), textures.data());
        for (GLuint program : programs) glDeleteProgram(program);
    }
    GLuint NewBuffer(GLenum target, GLsizeiptr size, const void* bytes) {
        GLuint buffer = 0;
        glGenBuffers(1, &buffer);
        buffers.push_back(buffer);
        glBindBuffer(target, buffer);
        glBufferData(target, size, bytes, GL_DYNAMIC_DRAW);
        return buffer;
    }
    GLuint NewTexture(int width, int height, const void* pixels) {
        GLuint texture = 0;
        glGenTextures(1, &texture);
        textures.push_back(texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        return texture;
    }
    GLuint NewFbo(GLuint texture) {
        GLuint fbo = 0;
        glGenFramebuffers(1,&fbo);
        fbos.push_back(fbo);
        glBindFramebuffer(GL_FRAMEBUFFER,fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
        EXPECT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
        return fbo;
    }
    GLuint Link(std::initializer_list<std::pair<GLenum,const char*>> stages) {
        const GLuint program = glCreateProgram();
        programs.push_back(program);
        for (const auto& [kind, source] : stages) {
            const GLuint shader = glCreateShader(kind);
            glShaderSource(shader,1,&source,nullptr);
            glCompileShader(shader);
            GLint ok = GL_FALSE;
            glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
            EXPECT_EQ(ok,GL_TRUE) << "run-ahead shader compile";
            glAttachShader(program,shader);
            glDeleteShader(shader);
        }
        glLinkProgram(program);
        GLint ok = GL_FALSE;
        glGetProgramiv(program,GL_LINK_STATUS,&ok);
        EXPECT_EQ(ok,GL_TRUE) << "run-ahead program link";
        return program;
    }
    void HoldNextBatch() {
        // Fully drain SETUP, then hold after one real clear batch. No wait/readback
        // is inserted among the operations whose queued ordering is under test.
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        ASSERT_TRUE(ArmSplitApplyHoldForTesting());
        glClear(GL_COLOR_BUFFER_BIT);
        ASSERT_TRUE(WaitForSplitApplyHoldForTesting()) << "server did not enter the scheduling hold";
        held = PeekSplitRuntime();
    }
    void ExpectQueued(unsigned int minimumRecords) {
        const auto now = PeekSplitRuntime();
        EXPECT_TRUE(SplitApplyHoldIsActiveForTesting()) << "client waited for apply instead of running ahead";
        EXPECT_EQ(now.appliedSeq, held.appliedSeq) << "held server advanced unexpectedly";
        EXPECT_GE(now.emitSeq, held.emitSeq + minimumRecords);
        EXPECT_GT(now.emitSeq, now.appliedSeq) << "no real client lead over the apply watermark";
        RecordProperty("observed_queued_records", std::to_string(now.emitSeq - now.appliedSeq));
    }
    void DrawTile(int tile) {
        glViewport(tile * 8,0,8,kHeight);
        glDrawArrays(GL_TRIANGLES,0,3);
    }
    void ExpectPixel(int x, int y, Pixel expected, const char* why) {
        Pixel actual{};
        glReadPixels(x,y,1,1,GL_RGBA,GL_UNSIGNED_BYTE,actual.data());
        EXPECT_EQ(actual,expected) << why;
        EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)) << why;
    }
};

TEST_F(MagmaRunAheadScenario, QueuedBufferVersionsAndDeletedNameReuseKeepTheirPixels) {
    if (!Ready() || IsSkipped()) return;
    const char* vs = R"(#version 430 core
layout(location=0) in vec4 color; out vec4 v;
void main(){vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));gl_Position=vec4(p[gl_VertexID],0,1);v=color;})";
    const char* fs = "#version 430 core\nin vec4 v;layout(location=0) out vec4 c;void main(){c=v;}";
    glUseProgram(Link({{GL_VERTEX_SHADER,vs},{GL_FRAGMENT_SHADER,fs}}));
    const auto colors = [](float r,float g,float b) { return std::array<GLfloat,12>{r,g,b,1,r,g,b,1,r,g,b,1}; };
    const auto r=colors(1,0,0), g=colors(0,1,0), b=colors(0,0,1), y=colors(1,1,0);
    const GLuint buffer=NewBuffer(GL_ARRAY_BUFFER,sizeof(r),r.data());
    glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,16,nullptr); glEnableVertexAttribArray(0);
    HeldApply hold; ASSERT_NO_FATAL_FAILURE(HoldNextBatch());
    DrawTile(0);
    glBufferSubData(GL_ARRAY_BUFFER,0,sizeof(g),g.data()); DrawTile(1);
    ExpectQueued(2); hold.Release();
    // Resource creation requires a reply, and storage definitions may require an
    // allocation acknowledgement. Do not hold the apply owner across those legal
    // waits. There is still no GPU readback/Finish before all four draws, so the
    // old GPU store must survive respecify and deletion until its draws complete.
    glBufferData(GL_ARRAY_BUFFER,sizeof(b),b.data(),GL_STREAM_DRAW); DrawTile(2);
    glDeleteBuffers(1,&buffer);
    // Request the recycled name through the public allocator. Record reuse must
    // have a distinct lifetime even though the application-visible name is equal.
    GLuint replacement=0;
    glGenBuffers(1,&replacement);
    EXPECT_EQ(replacement,buffer) << "the intended GL-name reuse did not occur";
    if (replacement!=buffer) buffers.push_back(replacement);
    glBindBuffer(GL_ARRAY_BUFFER,replacement);
    glBufferData(GL_ARRAY_BUFFER,sizeof(y),y.data(),GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,16,nullptr); DrawTile(3);
    ExpectPixel(4,4,red,"original buffer version"); ExpectPixel(12,4,green,"SubData version");
    ExpectPixel(20,4,blue,"respecified store"); ExpectPixel(28,4,yellow,"recreated GL name");
}

TEST_F(MagmaRunAheadScenario, QueuedProgramRebindsKeepEachUniformSnapshot) {
    if (!Ready() || IsSkipped()) return;
    const char* a="#version 430 core\nuniform vec4 tint;layout(location=0) out vec4 c;void main(){c=tint;}";
    const char* b="#version 430 core\nuniform vec4 tint;layout(location=0) out vec4 c;void main(){c=tint.bgra;}";
    const GLuint pa=Link({{GL_VERTEX_SHADER,kVertexId},{GL_FRAGMENT_SHADER,a}}), pb=Link({{GL_VERTEX_SHADER,kVertexId},{GL_FRAGMENT_SHADER,b}});
    const GLint la=glGetUniformLocation(pa,"tint"), lb=glGetUniformLocation(pb,"tint");
    ASSERT_GE(la,0); ASSERT_GE(lb,0);
    HeldApply hold; ASSERT_NO_FATAL_FAILURE(HoldNextBatch());
    glUseProgram(pa); glUniform4f(la,1,0,0,1); DrawTile(0);
    glUseProgram(pb); glUniform4f(lb,0,1,0,1); DrawTile(1);
    glUseProgram(pa); glUniform4f(la,0,0,1,1); DrawTile(2);
    glUseProgram(pb); glUniform4f(lb,0,1,1,1); DrawTile(3);
    ExpectQueued(4); hold.Release();
    ExpectPixel(4,4,red,"program A first uniform"); ExpectPixel(12,4,green,"program B first uniform");
    ExpectPixel(20,4,blue,"program A rebound"); ExpectPixel(28,4,yellow,"program B swizzled second uniform");
}

TEST_F(MagmaRunAheadScenario, QueuedGpuWritesReadBackTheLastDispatchBytes) {
    if (!Ready() || IsSkipped()) return;
    const char* cs=R"(#version 430 core
layout(local_size_x=1) in;layout(std430,binding=0) buffer Out{uint value[];};uniform uint seed;
void main(){uint i=gl_GlobalInvocationID.x;value[i]=seed+i;})";
    const GLuint program=Link({{GL_COMPUTE_SHADER,cs}});
    const GLint seed=glGetUniformLocation(program,"seed"); ASSERT_GE(seed,0);
    std::array<GLuint,32> data{};
    const GLuint buffer=NewBuffer(GL_SHADER_STORAGE_BUFFER,sizeof(data),data.data());
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER,0,buffer); glUseProgram(program);
    HeldApply hold; ASSERT_NO_FATAL_FAILURE(HoldNextBatch());
    glUniform1ui(seed,0x11110000u); glDispatchCompute(32,1,1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    glUniform1ui(seed,0x22220000u); glDispatchCompute(32,1,1);
    ExpectQueued(2); hold.Release();
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER,0,sizeof(data),data.data());
    for (GLuint i=0;i<data.size();++i) EXPECT_EQ(data[i],0x22220000u+i) << i;
    EXPECT_EQ(FirstGLError(), GLenum(GL_NO_ERROR)); glBindBufferBase(GL_SHADER_STORAGE_BUFFER,0,0);
}

TEST_F(MagmaRunAheadScenario, MixedNativeAndConvertedGpuVertexStreamsKeepTheirReservation) {
    if (!Ready() || IsSkipped()) return;
    const char* cs=R"(#version 430 core
layout(local_size_x=1) in;layout(std430,binding=0) buffer Out{uint words[];};
void main(){vec2 p[3]=vec2[3](vec2(-1,-1),vec2(0,-1),vec2(-1,1));
for(uint i=0u;i<3u;++i){words[4u*i]=floatBitsToUint(p[i].x);words[4u*i+1u]=floatBitsToUint(p[i].y);
words[4u*i+2u]=0u;words[4u*i+3u]=0x3ff00000u;}})";
    const char* vs=R"(#version 430 core
layout(location=0) in vec2 position;layout(location=1) in double code;out vec4 v;
void main(){gl_Position=vec4(position,0,1);v=vec4(1.0-float(code),float(code),0,1);})";
    const char* fs="#version 430 core\nin vec4 v;layout(location=0) out vec4 c;void main(){c=v;}";
    const GLuint compute=Link({{GL_COMPUTE_SHADER,cs}}), graphics=Link({{GL_VERTEX_SHADER,vs},{GL_FRAGMENT_SHADER,fs}});
    struct Vertex { GLfloat x,y; GLdouble code; };
    static_assert(sizeof(Vertex)==16);
    std::array<Vertex,3> vertices{};
    const GLuint buffer=NewBuffer(GL_ARRAY_BUFFER,sizeof(vertices),vertices.data());
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,16,nullptr); glEnableVertexAttribArray(0);
    glVertexAttribLPointer(1,1,GL_DOUBLE,16,reinterpret_cast<const void*>(8)); glEnableVertexAttribArray(1);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER,0,buffer);
    HeldApply hold; ASSERT_NO_FATAL_FAILURE(HoldNextBatch());
    glUseProgram(compute); glDispatchCompute(1,1,1);
    glMemoryBarrier(GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    glUseProgram(graphics); glDrawArrays(GL_TRIANGLES,0,3);
    vertices={Vertex{0,-1,0},Vertex{1,-1,0},Vertex{1,1,0}};
    glBufferSubData(GL_ARRAY_BUFFER,0,sizeof(vertices),vertices.data());
    glDrawArrays(GL_TRIANGLES,0,3);
    ExpectQueued(3); hold.Release();
    ExpectPixel(2,1,green,"native position reservation survives the other attribute's Double conversion read");
    ExpectPixel(29,1,red,"later SubData reaches only the later draw");
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER,0,0);
}

TEST_F(MagmaRunAheadScenario, TextureUploadCannotOvertakeThePreviouslyQueuedDraw) {
    if (!Ready() || IsSkipped()) return;
    const char* fs="#version 430 core\nuniform sampler2D src;layout(location=0) out vec4 c;void main(){c=texture(src,vec2(0.5));}";
    glUseProgram(Link({{GL_VERTEX_SHADER,kVertexId},{GL_FRAGMENT_SHADER,fs}}));
    const std::array<GLubyte,16> oldPixels{255,0,0,255,255,0,0,255,255,0,0,255,255,0,0,255};
    const std::array<GLubyte,16> newPixels{0,255,0,255,0,255,0,255,0,255,0,255,0,255,0,255};
    const GLuint source=NewTexture(2,2,oldPixels.data());
    HeldApply hold; ASSERT_NO_FATAL_FAILURE(HoldNextBatch());
    DrawTile(0); ExpectQueued(1); hold.Release();
    // Texture uploads have a wire reply. That is not a GPU completion barrier for
    // the prior draw; no ReadPixels/Finish is permitted before the second draw.
    glBindTexture(GL_TEXTURE_2D,source);
    glTexSubImage2D(GL_TEXTURE_2D,0,0,0,2,2,GL_RGBA,GL_UNSIGNED_BYTE,newPixels.data());
    DrawTile(1);
    ExpectPixel(4,4,red,"draw issued before texture update");
    ExpectPixel(12,4,green,"draw issued after texture update");
}

TEST_F(MagmaRunAheadScenario, GpuClearSurvivesTheFirstStorageImageUpgrade) {
    if (!Ready() || IsSkipped()) return;
    std::array<GLubyte,4*4*4> bytes{};
    for (size_t i=0;i<bytes.size();i+=4) { bytes[i]=255; bytes[i+3]=255; }
    const GLuint source=NewTexture(4,4,bytes.data()), sourceFbo=NewFbo(source);
    const char* cs=R"(#version 430 core
layout(local_size_x=1) in;layout(rgba8,binding=0) uniform writeonly image2D img;
void main(){imageStore(img,ivec2(0),vec4(0,0,1,1));})";
    const GLuint compute=Link({{GL_COMPUTE_SHADER,cs}});
    HeldApply hold; ASSERT_NO_FATAL_FAILURE(HoldNextBatch());
    glClearColor(0,1,0,1); glClear(GL_COLOR_BUFFER_BIT); ExpectQueued(1); hold.Release();
    glBindFramebuffer(GL_FRAMEBUFFER,target);
    glBindImageTexture(0,source,0,GL_FALSE,0,GL_WRITE_ONLY,GL_RGBA8);
    glUseProgram(compute); glDispatchCompute(1,1,1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,sourceFbo);
    ExpectPixel(0,0,blue,"compute writes the new STORAGE image");
    ExpectPixel(2,2,green,"usage upgrade preserves the earlier GPU clear, not the old red CPU upload");
    glBindImageTexture(0,0,0,GL_FALSE,0,GL_WRITE_ONLY,GL_RGBA8);
}

TEST_F(MagmaRunAheadScenario, PresentCreditActuallyParksTheClientAtItsConfiguredLimit) {
    if (!Ready() || IsSkipped()) return;
    glBindFramebuffer(GL_FRAMEBUFFER,0);
    HeldApply hold; ASSERT_NO_FATAL_FAILURE(HoldNextBatch());
    const auto before=PeekSplitRuntime();
    ASSERT_EQ(before.presentAckSerial,0u) << "this isolated case has not presented yet";
    for (unsigned i=0;i<before.presentCredit;++i) { glClear(GL_COLOR_BUFFER_BIT); Gl().EndFrame(); }
    ExpectQueued(before.presentCredit);
    const auto full=PeekSplitRuntime();
    EXPECT_EQ(full.presentAckSerial,before.presentAckSerial);
    EXPECT_EQ(full.presentCreditWaits,before.presentCreditWaits);
    std::atomic<bool> sawPark{false}, stop{false};
    std::thread observer([&] {
        const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(1);
        while (!stop.load() && std::chrono::steady_clock::now()<until) {
            if (SplitProducerIsParkedForTesting()) { sawPark.store(true); break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ReleaseSplitApplyHoldForTesting();
    });
    Gl().EndFrame(); // credit + 1 must park before it can publish its present.
    stop.store(true); observer.join(); hold.Release();
    EXPECT_TRUE(sawPark.load()) << "no real producer park while the present window was full";
    glFinish();
    const auto after=PeekSplitRuntime();
    EXPECT_EQ(after.presentCreditWaits,before.presentCreditWaits+1);
    EXPECT_EQ(after.presentAckSerial,before.presentCredit+1);
    EXPECT_EQ(after.appliedSeq,after.emitSeq);
    RecordProperty("observed_present_credit",std::to_string(before.presentCredit));
    RecordProperty("observed_producer_park",sawPark.load() ? "yes" : "no");
}
} // namespace
} // namespace MGITest
