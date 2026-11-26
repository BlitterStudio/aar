/*
 * AAR GL group: the table of GL 1.1 entry points that aar_gl_bind() returns,
 * and the gl*() wrappers that libaar defines over it.
 *
 * The table is an in-process structure: the app and the runtime are built
 * from this header for the same profile. It starts with two uint32_t words
 * (size, version) so the first pointer is at offset 8 on both profiles (A32
 * and A64) with no padding. Every later field is a pointer, so the offset of
 * field n is 8 + n * sizeof(void *).
 *
 * The table is append-only. `size` is the compatibility version: test every
 * field past Build2DMipmaps with AAR_GL_COVERS before use. `version` is the
 * GL ABI revision the runtime implements (AAR_GL_VERSION_*). A wrapper whose
 * field is past gl->size, or is NULL, is a no-op or returns 0.
 *
 * Integer parameters are int32_t / uint32_t. Perspective, Ortho, Frustum and
 * DepthRange take double. There is no stdcall and no GL header from Khronos
 * or any other vendor. Classic GL types that match: GLenum, GLbitfield and
 * GLuint are unsigned int, GLint and GLsizei are int, GLboolean and GLubyte
 * are unsigned char, GLfloat and GLclampf are float, GLdouble and GLclampd
 * are double. gluPerspective takes four doubles.
 *
 * An isolated runtime validates client-array pointers at draw time. For a bad
 * range it does not draw and the next glGetError returns GL_INVALID_VALUE; it
 * does not fault or stop the app. AAR_E_FAULT is the runtime's own status for
 * the failed call.
 *
 * Debug and platform bind flags are not part of the standard. The ZZ9000
 * keeps them in its extension table (aar/ext_zz9000.h).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AAR_GL_H
#define AAR_GL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GL ABI revisions, stored in aar_gl.version. */
#define AAR_GL_VERSION_V0     0u /* fields through Build2DMipmaps */
#define AAR_GL_VERSION_ARRAYS 1u /* adds the array and client-state fields */
#define AAR_GL_VERSION_11     2u /* adds the GL 1.1 completion fields */

typedef struct aar_gl {
    uint32_t size;    /* sizeof of the table the runtime built */
    uint32_t version; /* AAR_GL_VERSION_* */

    void     (*Begin)(uint32_t mode);
    void     (*End)(void);
    void     (*Vertex2f)(float x, float y);
    void     (*Vertex3f)(float x, float y, float z);
    void     (*Vertex3fv)(const float *v);
    void     (*TexCoord2f)(float s, float t);
    void     (*TexCoord2fv)(const float *v);
    void     (*Color3f)(float r, float g, float b);
    void     (*Color3fv)(const float *v);
    void     (*Color3ubv)(const uint8_t *v);
    void     (*Color4f)(float r, float g, float b, float a);
    void     (*Color4fv)(const float *v);
    void     (*Color4ub)(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void     (*Color4ubv)(const uint8_t *v);
    void     (*Enable)(uint32_t cap);
    void     (*Disable)(uint32_t cap);
    uint8_t  (*IsEnabled)(uint32_t cap);
    void     (*AlphaFunc)(uint32_t func, float ref);
    void     (*BlendFunc)(uint32_t sfactor, uint32_t dfactor);
    void     (*DepthFunc)(uint32_t func);
    void     (*DepthMask)(uint8_t flag);
    void     (*DepthRange)(double n, double f);
    void     (*CullFace)(uint32_t mode);
    void     (*FrontFace)(uint32_t mode);
    void     (*ShadeModel)(uint32_t mode);
    void     (*PolygonMode)(uint32_t face, uint32_t mode);
    void     (*Clear)(uint32_t mask);
    void     (*ClearColor)(float r, float g, float b, float a);
    void     (*Viewport)(int32_t x, int32_t y, int32_t width, int32_t height);
    void     (*Scissor)(int32_t x, int32_t y, int32_t width, int32_t height);
    void     (*MatrixMode)(uint32_t mode);
    void     (*LoadIdentity)(void);
    void     (*LoadMatrixf)(const float *m);
    void     (*PushMatrix)(void);
    void     (*PopMatrix)(void);
    void     (*Rotatef)(float angle, float x, float y, float z);
    void     (*Translatef)(float x, float y, float z);
    void     (*Scalef)(float x, float y, float z);
    void     (*Ortho)(double left, double right, double bottom, double top,
                      double zNear, double zFar);
    void     (*Frustum)(double left, double right, double bottom, double top,
                        double zNear, double zFar);
    void     (*TexImage2D)(uint32_t target, int32_t level, int32_t internalformat,
                           int32_t width, int32_t height, int32_t border,
                           uint32_t format, uint32_t type, const void *pixels);
    void     (*TexSubImage2D)(uint32_t target, int32_t level,
                              int32_t xoffset, int32_t yoffset,
                              int32_t width, int32_t height,
                              uint32_t format, uint32_t type, const void *pixels);
    void     (*TexParameteri)(uint32_t target, uint32_t pname, int32_t param);
    void     (*TexParameterf)(uint32_t target, uint32_t pname, float param);
    void     (*TexEnvi)(uint32_t target, uint32_t pname, int32_t param);
    void     (*TexEnvf)(uint32_t target, uint32_t pname, float param);
    void     (*GenTextures)(int32_t n, uint32_t *textures);
    void     (*DeleteTextures)(int32_t n, const uint32_t *textures);
    void     (*BindTexture)(uint32_t target, uint32_t texture);
    void     (*Finish)(void);
    void     (*Flush)(void);
    void     (*DrawBuffer)(uint32_t mode);
    void     (*ReadBuffer)(uint32_t mode);
    void     (*ReadPixels)(int32_t x, int32_t y, int32_t width, int32_t height,
                           uint32_t format, uint32_t type, void *pixels);
    const uint8_t *(*GetString)(uint32_t name);
    void     (*GetIntegerv)(uint32_t pname, int32_t *params);
    void     (*GetFloatv)(uint32_t pname, float *params);
    uint32_t (*GetError)(void);
    void     (*Hint)(uint32_t target, uint32_t mode);
    void     (*PixelStorei)(uint32_t pname, int32_t param);
    void     (*Perspective)(double fovy, double aspect, double zNear, double zFar);
    int32_t  (*Build2DMipmaps)(uint32_t target, int32_t internalFormat,
                               int32_t width, int32_t height,
                               uint32_t format, uint32_t type, const void *data);
    /* AAR_GL_VERSION_ARRAYS additions. */
    void     (*VertexPointer)(int32_t size, uint32_t type, int32_t stride,
                              const void *pointer);
    void     (*TexCoordPointer)(int32_t size, uint32_t type, int32_t stride,
                                const void *pointer);
    void     (*ColorPointer)(int32_t size, uint32_t type, int32_t stride,
                             const void *pointer);
    void     (*EnableClientState)(uint32_t cap);
    void     (*DisableClientState)(uint32_t cap);
    void     (*ClientActiveTextureARB)(uint32_t texture);
    void     (*DrawArrays)(uint32_t mode, int32_t first, int32_t count);
    void     (*DrawElements)(uint32_t mode, int32_t count, uint32_t type,
                             const void *indices);
    void     (*LockArrays)(uint32_t first, int32_t count);
    void     (*UnlockArrays)(void);
    /* AAR_GL_VERSION_11 additions. A platform whose renderer lacks a feature
     * leaves its field NULL. */
    void     (*ClearDepth)(double depth);
    void     (*ColorMask)(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void     (*PolygonOffset)(float factor, float units);
    void     (*StencilFunc)(uint32_t func, int32_t ref, uint32_t mask);
    void     (*StencilOp)(uint32_t fail, uint32_t zfail, uint32_t zpass);
    void     (*StencilMask)(uint32_t mask);
    void     (*ClearStencil)(int32_t s);
    void     (*MultMatrixf)(const float *m);
    void     (*LoadMatrixd)(const double *m);
    void     (*MultMatrixd)(const double *m);
    void     (*GetDoublev)(uint32_t pname, double *params);
    void     (*GetBooleanv)(uint32_t pname, uint8_t *params);
    void     (*ActiveTextureARB)(uint32_t texture);
    void     (*MultiTexCoord2fARB)(uint32_t target, float s, float t);
    void     (*MultiTexCoord2fvARB)(uint32_t target, const float *v);
    void     (*Normal3f)(float nx, float ny, float nz);
    void     (*Normal3fv)(const float *v);
    void     (*NormalPointer)(uint32_t type, int32_t stride, const void *pointer);
    void     (*Lightf)(uint32_t light, uint32_t pname, float param);
    void     (*Lightfv)(uint32_t light, uint32_t pname, const float *params);
    void     (*LightModelf)(uint32_t pname, float param);
    void     (*LightModelfv)(uint32_t pname, const float *params);
    void     (*LightModeli)(uint32_t pname, int32_t param);
    void     (*Materialf)(uint32_t face, uint32_t pname, float param);
    void     (*Materialfv)(uint32_t face, uint32_t pname, const float *params);
    void     (*ColorMaterial)(uint32_t face, uint32_t mode);
    void     (*Fogf)(uint32_t pname, float param);
    void     (*Fogfv)(uint32_t pname, const float *params);
    void     (*Fogi)(uint32_t pname, int32_t param);
    void     (*LineWidth)(float width);
    void     (*PointSize)(float size);
    void     (*Color3ub)(uint8_t r, uint8_t g, uint8_t b);
    void     (*Vertex2i)(int32_t x, int32_t y);
    void     (*Vertex2fv)(const float *v);
    void     (*TexCoord2i)(int32_t s, int32_t t);
    void     (*Rectf)(float x1, float y1, float x2, float y2);
    void     (*GetTexLevelParameteriv)(uint32_t target, int32_t level,
                                       uint32_t pname, int32_t *params);
    uint8_t  (*IsTexture)(uint32_t texture);
    void     (*CopyTexImage2D)(uint32_t target, int32_t level,
                               uint32_t internalformat,
                               int32_t x, int32_t y, int32_t width,
                               int32_t height, int32_t border);
    void     (*CopyTexSubImage2D)(uint32_t target, int32_t level,
                                  int32_t xoffset, int32_t yoffset,
                                  int32_t x, int32_t y,
                                  int32_t width, int32_t height);
    void     (*ClipPlane)(uint32_t plane, const double *equation);
    uint32_t (*GenLists)(int32_t range);
    void     (*NewList)(uint32_t list, uint32_t mode);
    void     (*EndList)(void);
    void     (*CallList)(uint32_t list);
    void     (*CallLists)(int32_t n, uint32_t type, const void *lists);
    void     (*DeleteLists)(uint32_t list, int32_t range);
    void     (*ListBase)(uint32_t base);
    void     (*TexEnvfv)(uint32_t target, uint32_t pname, const float *params);
    void     (*PushAttrib)(uint32_t mask);
    void     (*PopAttrib)(void);
} aar_gl;

/* True when size covers field. A NULL function pointer still means absent. */
#define AAR_GL_COVERS(gl, field) \
    ((gl) != 0 && (uint32_t)(gl)->size >= \
        (uint32_t)(offsetof(aar_gl, field) + sizeof((gl)->field)))

/* Number of pointer fields in the table (the layout test checks it). */
#define AAR_GL_SLOT_COUNT 123u

#if defined(__cplusplus)
static_assert(offsetof(aar_gl, Begin) == 8, "aar_gl first slot at offset 8");
static_assert(sizeof(aar_gl) == 8 + AAR_GL_SLOT_COUNT * sizeof(void *),
              "aar_gl has no padding");
#else
_Static_assert(offsetof(aar_gl, Begin) == 8, "aar_gl first slot at offset 8");
_Static_assert(sizeof(aar_gl) == 8 + AAR_GL_SLOT_COUNT * sizeof(void *),
               "aar_gl has no padding");
#endif

/*
 * libaar defines these over the table that aar_gl_bind() returned. Do not
 * define them in the app.
 */
void     glBegin(uint32_t mode);
void     glEnd(void);
void     glVertex2f(float x, float y);
void     glVertex3f(float x, float y, float z);
void     glVertex3fv(const float *v);
void     glTexCoord2f(float s, float t);
void     glTexCoord2fv(const float *v);
void     glColor3f(float r, float g, float b);
void     glColor3fv(const float *v);
void     glColor3ubv(const uint8_t *v);
void     glColor4f(float r, float g, float b, float a);
void     glColor4fv(const float *v);
void     glColor4ub(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void     glColor4ubv(const uint8_t *v);
void     glEnable(uint32_t cap);
void     glDisable(uint32_t cap);
uint8_t  glIsEnabled(uint32_t cap);
void     glAlphaFunc(uint32_t func, float ref);
void     glBlendFunc(uint32_t sfactor, uint32_t dfactor);
void     glDepthFunc(uint32_t func);
void     glDepthMask(uint8_t flag);
void     glDepthRange(double n, double f);
void     glCullFace(uint32_t mode);
void     glFrontFace(uint32_t mode);
void     glShadeModel(uint32_t mode);
void     glPolygonMode(uint32_t face, uint32_t mode);
void     glClear(uint32_t mask);
void     glClearColor(float r, float g, float b, float a);
void     glViewport(int32_t x, int32_t y, int32_t width, int32_t height);
void     glScissor(int32_t x, int32_t y, int32_t width, int32_t height);
void     glMatrixMode(uint32_t mode);
void     glLoadIdentity(void);
void     glLoadMatrixf(const float *m);
void     glPushMatrix(void);
void     glPopMatrix(void);
void     glRotatef(float angle, float x, float y, float z);
void     glTranslatef(float x, float y, float z);
void     glScalef(float x, float y, float z);
void     glOrtho(double left, double right, double bottom, double top,
                 double zNear, double zFar);
void     glFrustum(double left, double right, double bottom, double top,
                   double zNear, double zFar);
void     glTexImage2D(uint32_t target, int32_t level, int32_t internalformat,
                      int32_t width, int32_t height, int32_t border,
                      uint32_t format, uint32_t type, const void *pixels);
void     glTexSubImage2D(uint32_t target, int32_t level,
                         int32_t xoffset, int32_t yoffset,
                         int32_t width, int32_t height,
                         uint32_t format, uint32_t type, const void *pixels);
void     glTexParameteri(uint32_t target, uint32_t pname, int32_t param);
void     glTexParameterf(uint32_t target, uint32_t pname, float param);
void     glTexEnvi(uint32_t target, uint32_t pname, int32_t param);
void     glTexEnvf(uint32_t target, uint32_t pname, float param);
void     glGenTextures(int32_t n, uint32_t *textures);
void     glDeleteTextures(int32_t n, const uint32_t *textures);
void     glBindTexture(uint32_t target, uint32_t texture);
void     glFinish(void);
void     glFlush(void);
void     glDrawBuffer(uint32_t mode);
void     glReadBuffer(uint32_t mode);
void     glReadPixels(int32_t x, int32_t y, int32_t width, int32_t height,
                      uint32_t format, uint32_t type, void *pixels);
const uint8_t *glGetString(uint32_t name);
void     glGetIntegerv(uint32_t pname, int32_t *params);
void     glGetFloatv(uint32_t pname, float *params);
uint32_t glGetError(void);
void     glHint(uint32_t target, uint32_t mode);
void     glPixelStorei(uint32_t pname, int32_t param);
void     gluPerspective(double fovy, double aspect, double zNear, double zFar);
int32_t  gluBuild2DMipmaps(uint32_t target, int32_t internalFormat,
                           int32_t width, int32_t height,
                           uint32_t format, uint32_t type, const void *data);
void     glVertexPointer(int32_t size, uint32_t type, int32_t stride,
                         const void *pointer);
void     glTexCoordPointer(int32_t size, uint32_t type, int32_t stride,
                           const void *pointer);
void     glColorPointer(int32_t size, uint32_t type, int32_t stride,
                        const void *pointer);
void     glEnableClientState(uint32_t cap);
void     glDisableClientState(uint32_t cap);
void     glClientActiveTextureARB(uint32_t texture);
void     glDrawArrays(uint32_t mode, int32_t first, int32_t count);
void     glDrawElements(uint32_t mode, int32_t count, uint32_t type,
                        const void *indices);
void     glLockArrays(uint32_t first, int32_t count);
void     glUnlockArrays(void);
/* AAR_GL_VERSION_11. Absent or NULL: a no-op, and 0 for a value. The stencil
 * buffer is not promised: glGetIntegerv(GL_STENCIL_BITS) reports its depth, 0
 * for none. */
void     glClearDepth(double depth);
void     glColorMask(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void     glPolygonOffset(float factor, float units);
void     glStencilFunc(uint32_t func, int32_t ref, uint32_t mask);
void     glStencilOp(uint32_t fail, uint32_t zfail, uint32_t zpass);
void     glStencilMask(uint32_t mask);
void     glClearStencil(int32_t s);
void     glMultMatrixf(const float *m);
void     glLoadMatrixd(const double *m);
void     glMultMatrixd(const double *m);
void     glGetDoublev(uint32_t pname, double *params);
void     glGetBooleanv(uint32_t pname, uint8_t *params);
void     glActiveTextureARB(uint32_t texture);
void     glMultiTexCoord2fARB(uint32_t target, float s, float t);
void     glMultiTexCoord2fvARB(uint32_t target, const float *v);
void     glNormal3f(float nx, float ny, float nz);
void     glNormal3fv(const float *v);
void     glNormalPointer(uint32_t type, int32_t stride, const void *pointer);
void     glLightf(uint32_t light, uint32_t pname, float param);
void     glLightfv(uint32_t light, uint32_t pname, const float *params);
void     glLightModelf(uint32_t pname, float param);
void     glLightModelfv(uint32_t pname, const float *params);
void     glLightModeli(uint32_t pname, int32_t param);
void     glMaterialf(uint32_t face, uint32_t pname, float param);
void     glMaterialfv(uint32_t face, uint32_t pname, const float *params);
void     glColorMaterial(uint32_t face, uint32_t mode);
void     glFogf(uint32_t pname, float param);
void     glFogfv(uint32_t pname, const float *params);
void     glFogi(uint32_t pname, int32_t param);
void     glLineWidth(float width);
void     glPointSize(float size);
void     glColor3ub(uint8_t r, uint8_t g, uint8_t b);
void     glVertex2i(int32_t x, int32_t y);
void     glVertex2fv(const float *v);
void     glTexCoord2i(int32_t s, int32_t t);
void     glRectf(float x1, float y1, float x2, float y2);
void     glGetTexLevelParameteriv(uint32_t target, int32_t level,
                                  uint32_t pname, int32_t *params);
uint8_t  glIsTexture(uint32_t texture);
void     glCopyTexImage2D(uint32_t target, int32_t level,
                          uint32_t internalformat, int32_t x, int32_t y,
                          int32_t width, int32_t height, int32_t border);
void     glCopyTexSubImage2D(uint32_t target, int32_t level,
                             int32_t xoffset, int32_t yoffset,
                             int32_t x, int32_t y,
                             int32_t width, int32_t height);
void     glClipPlane(uint32_t plane, const double *equation);
uint32_t glGenLists(int32_t range);
void     glNewList(uint32_t list, uint32_t mode);
void     glEndList(void);
void     glCallList(uint32_t list);
void     glCallLists(int32_t n, uint32_t type, const void *lists);
void     glDeleteLists(uint32_t list, int32_t range);
void     glListBase(uint32_t base);
void     glTexEnvfv(uint32_t target, uint32_t pname, const float *params);
void     glPushAttrib(uint32_t mask);
void     glPopAttrib(void);

#ifdef __cplusplus
}
#endif

#endif
