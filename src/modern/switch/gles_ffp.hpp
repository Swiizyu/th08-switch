// gles_ffp.hpp — fixed-function OpenGL поверх GLES 3 для d3d8_compat.cpp.
//
// Идея (из отчёта о порте): НЕ объявлять функции с именами glBegin/glEnd и т.д. —
// это конфликтовало бы с реальными ES-функциями или давало рекурсию. Вместо этого
// подключаем GLES3/gl3.h и МАКРОСАМИ перенаправляем glBegin -> ffp::Begin.
// Реализация в gles_ffp.cpp вызывает настоящие функции как ::glEnable — там,
// где макросы уже сняты #undef.
//
// Подключать ВМЕСТО <GL/gl.h> + <GL/glext.h> (в d3d8_compat.cpp под __SWITCH__).
#pragma once

#include "gles_ffp_tokens.hpp"

#include <GLES3/gl3.h>

// GLdouble в gl3.h нет, а подписи десктопного GL его используют.
#ifndef GLdouble
typedef double GLdouble;
#endif

namespace ffp
{

// ---- immediate mode ----
void Begin(GLenum mode);
void End();
void Vertex2f(GLfloat x, GLfloat y);
void Vertex3f(GLfloat x, GLfloat y, GLfloat z);
void Color4ub(GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha);
void TexCoord2f(GLfloat s, GLfloat t);
void FogCoordf(GLfloat coordinate);

// ---- матрицы ----
void MatrixMode(GLenum mode);
void LoadIdentity();
void Ortho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
           GLdouble nearVal, GLdouble farVal);
void PushMatrix();
void PopMatrix();

// ---- attrib-стек ----
void PushAttrib(GLbitfield mask);
void PopAttrib();

// ---- texture environment ----
void TexEnvi(GLenum target, GLenum pname, GLint param);
void TexEnvfv(GLenum target, GLenum pname, const GLfloat *params);

// ---- альфа-тест ----
void AlphaFunc(GLenum func, GLclampf ref);

// ---- фог ----
void Fogi(GLenum pname, GLint param);
void Fogf(GLenum pname, GLfloat param);
void Fogfv(GLenum pname, const GLfloat *params);

// ---- всякое десктопное, чего нет в ES ----
void ClearDepth(GLdouble depth);
void DrawBuffer(GLenum mode);
void DepthMask(GLboolean flag);

// ---- enable/disable (TEXTURE_2D/ALPHA_TEST/FOG — внутренние, LIGHTING — no-op) ----
void Enable(GLenum cap);
void Disable(GLenum cap);

// ---- GL_CLAMP -> GL_CLAMP_TO_EDGE ----
void TexParameteri(GLenum target, GLenum pname, GLint param);

// ---- пейсер 60 Гц (вызывается из Present() после SwapWindow) ----
void SwitchPace();

} // namespace ffp

// ---- перенаправление десктопных имён на шим ----
#define glBegin ffp::Begin
#define glEnd ffp::End
#define glVertex2f ffp::Vertex2f
#define glVertex3f ffp::Vertex3f
#define glColor4ub ffp::Color4ub
#define glTexCoord2f ffp::TexCoord2f
#define glFogCoordf ffp::FogCoordf

#define glMatrixMode ffp::MatrixMode
#define glLoadIdentity ffp::LoadIdentity
#define glOrtho ffp::Ortho
#define glPushMatrix ffp::PushMatrix
#define glPopMatrix ffp::PopMatrix

#define glPushAttrib ffp::PushAttrib
#define glPopAttrib ffp::PopAttrib

#define glTexEnvi ffp::TexEnvi
#define glTexEnvfv ffp::TexEnvfv

#define glAlphaFunc ffp::AlphaFunc

#define glFogi ffp::Fogi
#define glFogf ffp::Fogf
#define glFogfv ffp::Fogfv

#define glClearDepth ffp::ClearDepth
#define glDrawBuffer ffp::DrawBuffer
#define glDepthMask ffp::DepthMask

#define glEnable ffp::Enable
#define glDisable ffp::Disable

#define glTexParameteri ffp::TexParameteri
