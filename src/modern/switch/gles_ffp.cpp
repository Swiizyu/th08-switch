// gles_ffp.cpp — реализация fixed-function шима поверх GLES 3.
//
// Архитектура (по отчёту о порте + фиксы аудита r2):
//  • Один шейдер на всё: uMvp * vec4(pos,1); источники текстуры
//    (0=texture, 1=цвет вершины, 2=константа TFACTOR), операции
//    replace/modulate на RGB и альфу отдельно, discard по альфа-тесту,
//    линейный фог mix(clamp((end-coord)/(end-start))).
//  • Вершины копятся в glBegin/glEnd в массив структур по 28 байт,
//    при glEnd — glBufferData(STREAM_DRAW) + glVertexAttribPointer×4 +
//    glDrawArrays.
//  • Матрицы свои column-major float[16], стеки глубиной 32, glOrtho
//    постумножает, MVP = P*MV.
//  • Аудит-фикс: dirty-флаги на uniform'ы (uMvp меняется только при
//    смене матриц), glUseProgram не выключается никогда, атрибут-массивы
//    включаются один раз.
#include "gles_ffp.hpp"

// ВАЖНО: здесь, внутри реализации ffp::, макросы шима должны быть сняты —
// иначе ::glEnable/::glDisable/::glDepthMask/::glTexParameteri в телах
// функций развернутся в ::ffp::Enable/... (бесконечная рекурсия).
// Макросы нужны ТОЛЬКО потребителям шима (d3d8_compat.cpp и т.п.).
#undef glBegin
#undef glEnd
#undef glVertex2f
#undef glVertex3f
#undef glColor4ub
#undef glTexCoord2f
#undef glFogCoordf
#undef glMatrixMode
#undef glLoadIdentity
#undef glOrtho
#undef glPushMatrix
#undef glPopMatrix
#undef glPushAttrib
#undef glPopAttrib
#undef glTexEnvi
#undef glTexEnvfv
#undef glAlphaFunc
#undef glFogi
#undef glFogf
#undef glFogfv
#undef glClearDepth
#undef glDrawBuffer
#undef glDepthMask
#undef glEnable
#undef glDisable
#undef glTexParameteri

#include <SDL.h>

#include <string.h>

namespace ffp
{
namespace
{

const int kMatrixStackDepth = 32;
const int kAttribStackDepth = 8;
const int kMaxTrackedCaps = 64;
const int kMaxVertices = 1 << 20;

// Источники для комбинатора: 0=текстура, 1=цвет вершины, 2=константа.
enum CombineSource { SOURCE_TEXTURE = 0, SOURCE_VERTEX = 1, SOURCE_CONSTANT = 2 };
// Операции: 0=replace (только source0), 1=modulate (source0*source1).
enum CombineOp { OP_REPLACE = 0, OP_MODULATE = 1 };

#pragma pack(push, 1)
struct FfpVertex
{
    float position[3];
    unsigned char color[4];
    float texCoord[2];
    float fog;
};
#pragma pack(pop)

struct TexEnvChannel
{
    int source0;
    int source1;
    int op;
};

struct AttribSnapshot
{
    bool texture2dEnabled;
    bool alphaTestEnabled;
    bool fogEnabled;
    GLenum alphaFunc;
    GLfloat alphaRef;
    GLfloat fogColor[4];
    GLfloat fogStart;
    GLfloat fogEnd;
    TexEnvChannel color;
    TexEnvChannel alpha;
    GLfloat envColor[4];
    GLboolean depthMask;
    int capCount;
    GLenum capNames[kMaxTrackedCaps];
    GLboolean capEnabled[kMaxTrackedCaps];
};

GLuint g_program;
GLuint g_vertexBuffer;
bool g_programReady;

GLint g_locMvp;
GLint g_locTextureEnabled;
GLint g_locColorSrcA, g_locColorSrcB, g_locColorOp;
GLint g_locAlphaSrcA, g_locAlphaSrcB, g_locAlphaOp;
GLint g_locEnvColor;
GLint g_locAlphaTestFunc, g_locAlphaTestRef;
GLint g_locFogEnabled, g_locFogColor, g_locFogStart, g_locFogEnd;
GLint g_locTexture;
GLint g_attrPosition, g_attrColor, g_attrTexCoord, g_attrFog;

GLenum g_beginMode;
FfpVertex *g_vertices;
int g_vertexCount;
unsigned char g_currentColor[4];
float g_currentTexCoord[2];
float g_currentFog;

float g_projection[16];
float g_modelview[16];
float g_projectionStack[kMatrixStackDepth][16];
float g_modelviewStack[kMatrixStackDepth][16];
int g_projectionDepth;
int g_modelviewDepth;
GLenum g_matrixMode;
float g_mvp[16];
bool g_mvpDirty;
bool g_mvpUniformDirty;

bool g_texture2dEnabled;
bool g_alphaTestEnabled;
bool g_fogEnabled;
bool g_textureEnabledDirty;

GLenum g_alphaFunc;
GLfloat g_alphaRef;
bool g_alphaTestDirty;

GLfloat g_fogColor[4];
GLfloat g_fogStart;
GLfloat g_fogEnd;
bool g_fogDirty;

TexEnvChannel g_colorEnv;
TexEnvChannel g_alphaEnv;
GLfloat g_envColor[4];
bool g_texEnvDirty;

GLboolean g_depthMaskValue = GL_TRUE;

int g_capCount;
GLenum g_capNames[kMaxTrackedCaps];
GLboolean g_capEnabled[kMaxTrackedCaps];

int g_attribDepth;
AttribSnapshot g_attribStack[kAttribStackDepth];

const char *kVertexShader = R"GLSL(#version 300 es
in vec3 aPosition;
in vec4 aColor;
in vec2 aTexCoord;
in float aFogCoord;
uniform mat4 uMvp;
out vec4 vColor;
out vec2 vTexCoord;
out float vFogCoord;
void main()
{
    gl_Position = uMvp * vec4(aPosition, 1.0);
    vColor = aColor;
    vTexCoord = aTexCoord;
    vFogCoord = aFogCoord;
}
)GLSL";

const char *kFragmentShader = R"GLSL(#version 300 es
precision mediump float;
uniform sampler2D uTexture;
uniform lowp int uTextureEnabled;
uniform lowp int uColorSrcA;
uniform lowp int uColorSrcB;
uniform lowp int uColorOp;
uniform lowp int uAlphaSrcA;
uniform lowp int uAlphaSrcB;
uniform lowp int uAlphaOp;
uniform vec4 uEnvColor;
uniform lowp int uAlphaTestFunc;
uniform float uAlphaTestRef;
uniform lowp int uFogEnabled;
uniform vec4 uFogColor;
uniform float uFogStart;
uniform float uFogEnd;
in vec4 vColor;
in vec2 vTexCoord;
in float vFogCoord;
out vec4 fragColor;
vec3 PickColor(int source)
{
    if (source == 0) return texture(uTexture, vTexCoord).rgb;
    if (source == 1) return vColor.rgb;
    return uEnvColor.rgb;
}
float PickAlpha(int source)
{
    if (source == 0) return texture(uTexture, vTexCoord).a;
    if (source == 1) return vColor.a;
    return uEnvColor.a;
}
void main()
{
    vec4 result;
    if (uTextureEnabled == 0)
    {
        result = vColor;
    }
    else
    {
        float alpha = uAlphaOp == 1 ? PickAlpha(uAlphaSrcA) * PickAlpha(uAlphaSrcB)
                                    : PickAlpha(uAlphaSrcA);
        vec3 rgb = uColorOp == 1 ? PickColor(uColorSrcA) * PickColor(uColorSrcB)
                                 : PickColor(uColorSrcA);
        result = vec4(rgb, alpha);
    }
    if (uAlphaTestFunc == 0) discard;
    else if (uAlphaTestFunc == 1) { if (!(result.a < uAlphaTestRef)) discard; }
    else if (uAlphaTestFunc == 2) { if (abs(result.a - uAlphaTestRef) > 0.002) discard; }
    else if (uAlphaTestFunc == 3) { if (!(result.a <= uAlphaTestRef)) discard; }
    else if (uAlphaTestFunc == 4) { if (!(result.a > uAlphaTestRef)) discard; }
    else if (uAlphaTestFunc == 5) { if (abs(result.a - uAlphaTestRef) <= 0.002) discard; }
    else if (uAlphaTestFunc == 6) { if (!(result.a >= uAlphaTestRef)) discard; }
    if (uFogEnabled != 0 && uFogEnd > uFogStart)
    {
        float factor = clamp((uFogEnd - vFogCoord) / (uFogEnd - uFogStart), 0.0, 1.0);
        result.rgb = mix(uFogColor.rgb, result.rgb, factor);
    }
    fragColor = result;
}
)GLSL";

void LoadIdentityMatrix(float *matrix)
{
    memset(matrix, 0, 16 * sizeof(float));
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
}

// result = left * right (column-major: m[column * 4 + row]).
void MultiplyMatrix(float *result, const float *left, const float *right)
{
    float product[16];
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            product[column * 4 + row] =
                left[row] * right[column * 4] + left[4 + row] * right[column * 4 + 1] +
                left[8 + row] * right[column * 4 + 2] + left[12 + row] * right[column * 4 + 3];
    memcpy(result, product, sizeof(product));
}

float *CurrentMatrix()
{
    return g_matrixMode == GL_PROJECTION ? g_projection : g_modelview;
}

GLuint CompileShader(GLenum kind, const char *source)
{
    GLuint shader = ::glCreateShader(kind);
    ::glShaderSource(shader, 1, &source, NULL);
    ::glCompileShader(shader);
    GLint compiled = GL_FALSE;
    ::glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_FALSE)
    {
        char log[1024];
        GLsizei length = 0;
        ::glGetShaderInfoLog(shader, sizeof(log), &length, log);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "th08-switch: shader compile failed: %.*s",
                     static_cast<int>(length), log);
    }
    return shader;
}

bool EnsureProgram()
{
    if (g_programReady)
        return true;

    GLuint vertex = CompileShader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    GLuint program = ::glCreateProgram();
    ::glAttachShader(program, vertex);
    ::glAttachShader(program, fragment);
    ::glLinkProgram(program);
    ::glDeleteShader(vertex);
    ::glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    ::glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked == GL_FALSE)
    {
        char log[1024];
        GLsizei length = 0;
        ::glGetProgramInfoLog(program, sizeof(log), &length, log);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "th08-switch: program link failed: %.*s",
                     static_cast<int>(length), log);
        return false;
    }

    g_locMvp = ::glGetUniformLocation(program, "uMvp");
    g_locTextureEnabled = ::glGetUniformLocation(program, "uTextureEnabled");
    g_locColorSrcA = ::glGetUniformLocation(program, "uColorSrcA");
    g_locColorSrcB = ::glGetUniformLocation(program, "uColorSrcB");
    g_locColorOp = ::glGetUniformLocation(program, "uColorOp");
    g_locAlphaSrcA = ::glGetUniformLocation(program, "uAlphaSrcA");
    g_locAlphaSrcB = ::glGetUniformLocation(program, "uAlphaSrcB");
    g_locAlphaOp = ::glGetUniformLocation(program, "uAlphaOp");
    g_locEnvColor = ::glGetUniformLocation(program, "uEnvColor");
    g_locAlphaTestFunc = ::glGetUniformLocation(program, "uAlphaTestFunc");
    g_locAlphaTestRef = ::glGetUniformLocation(program, "uAlphaTestRef");
    g_locFogEnabled = ::glGetUniformLocation(program, "uFogEnabled");
    g_locFogColor = ::glGetUniformLocation(program, "uFogColor");
    g_locFogStart = ::glGetUniformLocation(program, "uFogStart");
    g_locFogEnd = ::glGetUniformLocation(program, "uFogEnd");
    g_locTexture = ::glGetUniformLocation(program, "uTexture");

    g_attrPosition = ::glGetAttribLocation(program, "aPosition");
    g_attrColor = ::glGetAttribLocation(program, "aColor");
    g_attrTexCoord = ::glGetAttribLocation(program, "aTexCoord");
    g_attrFog = ::glGetAttribLocation(program, "aFogCoord");

    // Программа включается ОДИН раз и никогда не выключается (аудит-фикс).
    ::glUseProgram(program);
    ::glUniform1i(g_locTexture, 0);

    // Атрибут-массивы включаются один раз.
    ::glEnableVertexAttribArray(g_attrPosition);
    ::glEnableVertexAttribArray(g_attrColor);
    ::glEnableVertexAttribArray(g_attrTexCoord);
    ::glEnableVertexAttribArray(g_attrFog);

    ::glGenBuffers(1, &g_vertexBuffer);

    g_program = program;
    g_programReady = true;
    g_mvpDirty = true;
    g_mvpUniformDirty = true;
    g_textureEnabledDirty = true;
    g_alphaTestDirty = true;
    g_fogDirty = true;
    g_texEnvDirty = true;
    return true;
}

int AlphaTestFuncIndex(GLenum func)
{
    switch (func)
    {
    case GL_NEVER: return 0;
    case GL_LESS: return 1;
    case GL_EQUAL: return 2;
    case GL_LEQUAL: return 3;
    case GL_GREATER: return 4;
    case GL_NOTEQUAL: return 5;
    case GL_GEQUAL: return 6;
    default: return 7; // GL_ALWAYS
    }
}

int CombineSourceFromGl(GLenum source)
{
    switch (source)
    {
    case GL_TEXTURE: return SOURCE_TEXTURE;
    case GL_CONSTANT: return SOURCE_CONSTANT;
    default: return SOURCE_VERTEX; // GL_PRIMARY_COLOR, GL_PREVIOUS и прочее
    }
}

int TrackedCapIndex(GLenum cap)
{
    for (int index = 0; index < g_capCount; ++index)
        if (g_capNames[index] == cap)
            return index;
    return -1;
}

void SetCap(GLenum cap, GLboolean enabled)
{
    const int index = TrackedCapIndex(cap);
    if (index >= 0)
    {
        g_capEnabled[index] = enabled;
        return;
    }
    if (g_capCount >= kMaxTrackedCaps)
        return;
    g_capNames[g_capCount] = cap;
    g_capEnabled[g_capCount] = enabled;
    ++g_capCount;
}

void ApplyRealCap(GLenum cap, GLboolean enabled)
{
    if (enabled) ::glEnable(cap);
    else ::glDisable(cap);
}

} // namespace

void Begin(GLenum mode)
{
    g_beginMode = mode;
    g_vertexCount = 0;
    g_currentColor[0] = g_currentColor[1] = g_currentColor[2] = g_currentColor[3] = 255;
    g_currentTexCoord[0] = g_currentTexCoord[1] = 0.0f;
    g_currentFog = 0.0f;
}

void End()
{
    if (g_vertexCount == 0)
        return;
    if (!EnsureProgram())
        return;

    // uMvp перезаливается только при смене матриц (аудит-фикс).
    if (g_mvpDirty)
    {
        MultiplyMatrix(g_mvp, g_projection, g_modelview);
        g_mvpDirty = false;
        g_mvpUniformDirty = true;
    }
    if (g_mvpUniformDirty)
    {
        ::glUniformMatrix4fv(g_locMvp, 1, GL_FALSE, g_mvp);
        g_mvpUniformDirty = false;
    }
    if (g_textureEnabledDirty)
    {
        ::glUniform1i(g_locTextureEnabled, g_texture2dEnabled ? 1 : 0);
        g_textureEnabledDirty = false;
    }
    if (g_alphaTestDirty)
    {
        ::glUniform1i(g_locAlphaTestFunc, g_alphaTestEnabled ? AlphaTestFuncIndex(g_alphaFunc) : 7);
        ::glUniform1f(g_locAlphaTestRef, g_alphaRef);
        g_alphaTestDirty = false;
    }
    if (g_fogDirty)
    {
        ::glUniform1i(g_locFogEnabled, g_fogEnabled ? 1 : 0);
        ::glUniform4fv(g_locFogColor, 1, g_fogColor);
        ::glUniform1f(g_locFogStart, g_fogStart);
        ::glUniform1f(g_locFogEnd, g_fogEnd);
        g_fogDirty = false;
    }
    if (g_texEnvDirty)
    {
        ::glUniform1i(g_locColorSrcA, g_colorEnv.source0);
        ::glUniform1i(g_locColorSrcB, g_colorEnv.source1);
        ::glUniform1i(g_locColorOp, g_colorEnv.op);
        ::glUniform1i(g_locAlphaSrcA, g_alphaEnv.source0);
        ::glUniform1i(g_locAlphaSrcB, g_alphaEnv.source1);
        ::glUniform1i(g_locAlphaOp, g_alphaEnv.op);
        ::glUniform4fv(g_locEnvColor, 1, g_envColor);
        g_texEnvDirty = false;
    }

    ::glBindBuffer(GL_ARRAY_BUFFER, g_vertexBuffer);
    ::glBufferData(GL_ARRAY_BUFFER, g_vertexCount * static_cast<GLsizeiptr>(sizeof(FfpVertex)),
                   g_vertices, GL_STREAM_DRAW);
    ::glVertexAttribPointer(g_attrPosition, 3, GL_FLOAT, GL_FALSE, sizeof(FfpVertex),
                            reinterpret_cast<const GLvoid *>(0));
    ::glVertexAttribPointer(g_attrColor, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(FfpVertex),
                            reinterpret_cast<const GLvoid *>(12));
    ::glVertexAttribPointer(g_attrTexCoord, 2, GL_FLOAT, GL_FALSE, sizeof(FfpVertex),
                            reinterpret_cast<const GLvoid *>(16));
    ::glVertexAttribPointer(g_attrFog, 1, GL_FLOAT, GL_FALSE, sizeof(FfpVertex),
                            reinterpret_cast<const GLvoid *>(24));
    ::glDrawArrays(g_beginMode, 0, g_vertexCount);
    g_vertexCount = 0;
}

void Vertex2f(GLfloat x, GLfloat y)
{
    if (g_vertexCount >= kMaxVertices)
        return;
    FfpVertex &vertex = g_vertices[g_vertexCount++];
    vertex.position[0] = x;
    vertex.position[1] = y;
    vertex.position[2] = 0.0f;
    memcpy(vertex.color, g_currentColor, 4);
    vertex.texCoord[0] = g_currentTexCoord[0];
    vertex.texCoord[1] = g_currentTexCoord[1];
    vertex.fog = g_currentFog;
}

void Vertex3f(GLfloat x, GLfloat y, GLfloat z)
{
    if (g_vertexCount >= kMaxVertices)
        return;
    FfpVertex &vertex = g_vertices[g_vertexCount++];
    vertex.position[0] = x;
    vertex.position[1] = y;
    vertex.position[2] = z;
    memcpy(vertex.color, g_currentColor, 4);
    vertex.texCoord[0] = g_currentTexCoord[0];
    vertex.texCoord[1] = g_currentTexCoord[1];
    vertex.fog = g_currentFog;
}

void Color4ub(GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha)
{
    g_currentColor[0] = red;
    g_currentColor[1] = green;
    g_currentColor[2] = blue;
    g_currentColor[3] = alpha;
}

void TexCoord2f(GLfloat s, GLfloat t)
{
    g_currentTexCoord[0] = s;
    g_currentTexCoord[1] = t;
}

void FogCoordf(GLfloat coordinate)
{
    g_currentFog = coordinate;
}

void MatrixMode(GLenum mode)
{
    g_matrixMode = mode;
}

void LoadIdentity()
{
    LoadIdentityMatrix(CurrentMatrix());
    g_mvpDirty = true;
}

void Ortho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
           GLdouble nearVal, GLdouble farVal)
{
    float ortho[16];
    memset(ortho, 0, sizeof(ortho));
    ortho[0] = static_cast<float>(2.0 / (right - left));
    ortho[5] = static_cast<float>(2.0 / (top - bottom));
    ortho[10] = static_cast<float>(-2.0 / (farVal - nearVal));
    ortho[12] = static_cast<float>(-(right + left) / (right - left));
    ortho[13] = static_cast<float>(-(top + bottom) / (top - bottom));
    ortho[14] = static_cast<float>(-(farVal + nearVal) / (farVal - nearVal));
    ortho[15] = 1.0f;
    // glOrtho ПОСТумножает текущую матрицу.
    float product[16];
    MultiplyMatrix(product, CurrentMatrix(), ortho);
    memcpy(CurrentMatrix(), product, sizeof(product));
    g_mvpDirty = true;
}

void PushMatrix()
{
    if (g_matrixMode == GL_PROJECTION)
    {
        if (g_projectionDepth >= kMatrixStackDepth)
            return;
        memcpy(g_projectionStack[g_projectionDepth++], g_projection, sizeof(g_projection));
    }
    else
    {
        if (g_modelviewDepth >= kMatrixStackDepth)
            return;
        memcpy(g_modelviewStack[g_modelviewDepth++], g_modelview, sizeof(g_modelview));
    }
}

void PopMatrix()
{
    if (g_matrixMode == GL_PROJECTION)
    {
        if (g_projectionDepth == 0)
            return;
        memcpy(g_projection, g_projectionStack[--g_projectionDepth], sizeof(g_projection));
    }
    else
    {
        if (g_modelviewDepth == 0)
            return;
        memcpy(g_modelview, g_modelviewStack[--g_modelviewDepth], sizeof(g_modelview));
    }
    g_mvpDirty = true;
}

void PushAttrib(GLbitfield)
{
    if (g_attribDepth >= kAttribStackDepth)
        return;
    AttribSnapshot &snapshot = g_attribStack[g_attribDepth++];
    snapshot.texture2dEnabled = g_texture2dEnabled;
    snapshot.alphaTestEnabled = g_alphaTestEnabled;
    snapshot.fogEnabled = g_fogEnabled;
    snapshot.alphaFunc = g_alphaFunc;
    snapshot.alphaRef = g_alphaRef;
    memcpy(snapshot.fogColor, g_fogColor, sizeof(g_fogColor));
    snapshot.fogStart = g_fogStart;
    snapshot.fogEnd = g_fogEnd;
    snapshot.color = g_colorEnv;
    snapshot.alpha = g_alphaEnv;
    memcpy(snapshot.envColor, g_envColor, sizeof(g_envColor));
    snapshot.depthMask = g_depthMaskValue;
    snapshot.capCount = g_capCount;
    for (int index = 0; index < g_capCount; ++index)
    {
        snapshot.capNames[index] = g_capNames[index];
        snapshot.capEnabled[index] = g_capEnabled[index];
    }
}

void PopAttrib()
{
    if (g_attribDepth == 0)
        return;
    const AttribSnapshot &snapshot = g_attribStack[--g_attribDepth];
    g_texture2dEnabled = snapshot.texture2dEnabled;
    g_alphaTestEnabled = snapshot.alphaTestEnabled;
    g_fogEnabled = snapshot.fogEnabled;
    g_alphaFunc = snapshot.alphaFunc;
    g_alphaRef = snapshot.alphaRef;
    memcpy(g_fogColor, snapshot.fogColor, sizeof(g_fogColor));
    g_fogStart = snapshot.fogStart;
    g_fogEnd = snapshot.fogEnd;
    g_colorEnv = snapshot.color;
    g_alphaEnv = snapshot.alpha;
    memcpy(g_envColor, snapshot.envColor, sizeof(g_envColor));
    g_textureEnabledDirty = true;
    g_alphaTestDirty = true;
    g_fogDirty = true;
    g_texEnvDirty = true;
    ::glDepthMask(g_depthMaskValue = snapshot.depthMask);
    // Реальные caps повторяем из снапшота.
    for (int index = 0; index < snapshot.capCount; ++index)
    {
        ApplyRealCap(snapshot.capNames[index], snapshot.capEnabled[index]);
        SetCap(snapshot.capNames[index], snapshot.capEnabled[index]);
    }
    g_capCount = snapshot.capCount;
}

void TexEnvi(GLenum target, GLenum pname, GLint param)
{
    if (target != GL_TEXTURE_ENV)
        return;
    switch (pname)
    {
    case GL_TEXTURE_ENV_MODE:
        // MODE=REPLACE -> (текстура, replace); MODULATE -> текстура*цвет вершины;
        // COMBINE -> оставить сконфигурированные комбинированные источники.
        if (param == GL_REPLACE)
        {
            g_colorEnv.source0 = g_colorEnv.source1 = SOURCE_TEXTURE;
            g_colorEnv.op = OP_REPLACE;
            g_alphaEnv.source0 = g_alphaEnv.source1 = SOURCE_TEXTURE;
            g_alphaEnv.op = OP_REPLACE;
        }
        else if (param == GL_MODULATE)
        {
            g_colorEnv.source0 = SOURCE_TEXTURE;
            g_colorEnv.source1 = SOURCE_VERTEX;
            g_colorEnv.op = OP_MODULATE;
            g_alphaEnv.source0 = SOURCE_TEXTURE;
            g_alphaEnv.source1 = SOURCE_VERTEX;
            g_alphaEnv.op = OP_MODULATE;
        }
        break;
    case GL_COMBINE_RGB:
        g_colorEnv.op = param == GL_REPLACE ? OP_REPLACE : OP_MODULATE;
        break;
    case GL_COMBINE_ALPHA:
        g_alphaEnv.op = param == GL_REPLACE ? OP_REPLACE : OP_MODULATE;
        break;
    case GL_SOURCE0_RGB:
        g_colorEnv.source0 = CombineSourceFromGl(static_cast<GLenum>(param));
        break;
    case GL_SOURCE1_RGB:
        g_colorEnv.source1 = CombineSourceFromGl(static_cast<GLenum>(param));
        break;
    case GL_SOURCE0_ALPHA:
        g_alphaEnv.source0 = CombineSourceFromGl(static_cast<GLenum>(param));
        break;
    case GL_SOURCE1_ALPHA:
        g_alphaEnv.source1 = CombineSourceFromGl(static_cast<GLenum>(param));
        break;
    default:
        break; // OPERAND0/1 — всегда SRC_COLOR/SRC_ALPHA от d3d8_compat
    }
    g_texEnvDirty = true;
}

void TexEnvfv(GLenum target, GLenum pname, const GLfloat *params)
{
    if (target != GL_TEXTURE_ENV || pname != GL_TEXTURE_ENV_COLOR || params == NULL)
        return;
    memcpy(g_envColor, params, sizeof(g_envColor));
    g_texEnvDirty = true;
}

void AlphaFunc(GLenum func, GLclampf ref)
{
    g_alphaFunc = func;
    g_alphaRef = static_cast<GLfloat>(ref);
    g_alphaTestDirty = true;
}

void Fogi(GLenum pname, GLint)
{
    // Поддерживается только линейный фог по координате фога (то, что шлёт
    // d3d8_compat): GL_FOG_MODE/GL_FOG_COORDINATE_SOURCE не влияют.
    if (pname == GL_FOG_MODE || pname == GL_FOG_COORDINATE_SOURCE)
        return;
}

void Fogf(GLenum pname, GLfloat param)
{
    if (pname == GL_FOG_START)
        g_fogStart = param;
    else if (pname == GL_FOG_END)
        g_fogEnd = param;
    g_fogDirty = true;
}

void Fogfv(GLenum pname, const GLfloat *params)
{
    if (pname == GL_FOG_COLOR && params != NULL)
    {
        memcpy(g_fogColor, params, sizeof(g_fogColor));
        g_fogDirty = true;
    }
}

void ClearDepth(GLdouble depth)
{
    ::glClearDepthf(static_cast<GLfloat>(depth));
}

void DrawBuffer(GLenum mode)
{
    if (mode == GL_COLOR_ATTACHMENT0)
    {
        const GLenum attachment = GL_COLOR_ATTACHMENT0;
        ::glDrawBuffers(1, &attachment);
    }
    // GL_BACK/GL_FRONT — дефолтный фреймбуфер, в ES и так один draw-буфер.
}

void DepthMask(GLboolean flag)
{
    g_depthMaskValue = flag;
    ::glDepthMask(flag);
}

void Enable(GLenum cap)
{
    switch (cap)
    {
    case GL_TEXTURE_2D:
        g_texture2dEnabled = true;
        g_textureEnabledDirty = true;
        return;
    case GL_ALPHA_TEST:
        g_alphaTestEnabled = true;
        g_alphaTestDirty = true;
        return;
    case GL_FOG:
        g_fogEnabled = true;
        g_fogDirty = true;
        return;
    case GL_LIGHTING:
        return; // no-op
    default:
        break;
    }
    SetCap(cap, GL_TRUE);
    ::glEnable(cap);
}

void Disable(GLenum cap)
{
    switch (cap)
    {
    case GL_TEXTURE_2D:
        g_texture2dEnabled = false;
        g_textureEnabledDirty = true;
        return;
    case GL_ALPHA_TEST:
        g_alphaTestEnabled = false;
        g_alphaTestDirty = true;
        return;
    case GL_FOG:
        g_fogEnabled = false;
        g_fogDirty = true;
        return;
    case GL_LIGHTING:
        return; // no-op
    default:
        break;
    }
    SetCap(cap, GL_FALSE);
    ::glDisable(cap);
}

void TexParameteri(GLenum target, GLenum pname, GLint param)
{
    if (param == GL_CLAMP)
        param = GL_CLAMP_TO_EDGE;
    ::glTexParameteri(target, pname, param);
}

void SwitchPace()
{
    // Пейсер 60 Гц на абсолютном дедлайне: eglSwapBuffers на Horizon/Mesa
    // НЕ блокируется, без пейсера будет свободный бег.
    static Uint64 deadlineBase = 0;
    const Uint64 frequency = SDL_GetPerformanceFrequency();
    const Uint64 frameTicks = frequency / 60;
    if (frameTicks == 0)
        return;
    const Uint64 now = SDL_GetPerformanceCounter();
    if (deadlineBase == 0)
        deadlineBase = now;
    // Пересинк при отставании больше 4 кадров.
    if (now > deadlineBase + frameTicks * 4)
        deadlineBase = now;
    const Uint64 deadline = deadlineBase + frameTicks;
    while (SDL_GetPerformanceCounter() < deadline)
        SDL_Delay(1);
    deadlineBase += frameTicks;
}

namespace
{
struct ShimInitializer
{
    ShimInitializer()
    {
        g_vertices = new FfpVertex[kMaxVertices];
        LoadIdentityMatrix(g_projection);
        LoadIdentityMatrix(g_modelview);
        g_colorEnv.source0 = g_colorEnv.source1 = SOURCE_TEXTURE;
        g_colorEnv.op = OP_MODULATE;
        g_alphaEnv.source0 = g_alphaEnv.source1 = SOURCE_TEXTURE;
        g_alphaEnv.op = OP_MODULATE;
        g_envColor[0] = g_envColor[1] = g_envColor[2] = g_envColor[3] = 1.0f;
        g_fogColor[0] = g_fogColor[1] = g_fogColor[2] = 1.0f;
        g_fogColor[3] = 1.0f;
        g_fogStart = 0.0f;
        g_fogEnd = 1.0f;
        g_alphaFunc = GL_ALWAYS;
        g_alphaRef = 0.0f;
    }
};
const ShimInitializer g_shimInitializer;
}

} // namespace ffp
