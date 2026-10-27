// GL ES 2 access: direct on Emscripten, loaded through SDL on desktop.
#pragma once
#ifdef __EMSCRIPTEN__
#include <GLES2/gl2.h>
#define GL_LOAD_ALL() 1
#else
#define GL_GLES_PROTOTYPES 1
#define SDL_USE_BUILTIN_OPENGL_DEFINITIONS 1
#include <SDL_opengles2.h>

#define GL_FUNCS(X) \
    X(glGenTextures) X(glDeleteTextures) X(glBindTexture) X(glTexImage2D) X(glTexSubImage2D) \
    X(glTexParameteri) X(glActiveTexture) X(glGenBuffers) X(glDeleteBuffers) X(glBindBuffer) \
    X(glBufferData) X(glBufferSubData) X(glGenFramebuffers) X(glDeleteFramebuffers) X(glBindFramebuffer) \
    X(glFramebufferTexture2D) X(glFramebufferRenderbuffer) X(glGenRenderbuffers) X(glBindRenderbuffer) \
    X(glRenderbufferStorage) X(glDeleteRenderbuffers) X(glCheckFramebufferStatus) X(glCreateShader) \
    X(glShaderSource) X(glCompileShader) X(glGetShaderiv) X(glGetShaderInfoLog) X(glCreateProgram) \
    X(glAttachShader) X(glLinkProgram) X(glGetProgramiv) X(glGetProgramInfoLog) X(glUseProgram) \
    X(glBindAttribLocation) X(glGetUniformLocation) X(glUniform1i) X(glUniform1f) X(glUniform4f) \
    X(glUniform4fv) X(glUniformMatrix4fv) X(glVertexAttribPointer) X(glEnableVertexAttribArray) \
    X(glDisableVertexAttribArray) X(glVertexAttrib4f) X(glDrawArrays) X(glDrawElements) X(glViewport) \
    X(glScissor) X(glEnable) X(glDisable) X(glBlendFunc) X(glBlendFuncSeparate) X(glBlendEquation) \
    X(glDepthFunc) X(glDepthMask) X(glClearColor) X(glClear) X(glColorMask) X(glReadPixels) \
    X(glPixelStorei) X(glGetError) X(glGetString) X(glFinish) X(glDeleteShader) X(glDeleteProgram) \
    X(glGenerateMipmap) X(glGetIntegerv)

#define GL_TYPEDEF(n) typedef __typeof__(n) n##_fn;
GL_FUNCS(GL_TYPEDEF)
#undef GL_TYPEDEF
#define GL_DECL(n) extern n##_fn *p_##n;
GL_FUNCS(GL_DECL)
#undef GL_DECL
extern void (GL_APIENTRY *p_glClearDepthf)(GLfloat);
int gl_load_all(void);
#define GL_LOAD_ALL() gl_load_all()

#define glGenTextures p_glGenTextures
#define glDeleteTextures p_glDeleteTextures
#define glBindTexture p_glBindTexture
#define glTexImage2D p_glTexImage2D
#define glTexSubImage2D p_glTexSubImage2D
#define glTexParameteri p_glTexParameteri
#define glActiveTexture p_glActiveTexture
#define glGenBuffers p_glGenBuffers
#define glDeleteBuffers p_glDeleteBuffers
#define glBindBuffer p_glBindBuffer
#define glBufferData p_glBufferData
#define glBufferSubData p_glBufferSubData
#define glGenFramebuffers p_glGenFramebuffers
#define glDeleteFramebuffers p_glDeleteFramebuffers
#define glBindFramebuffer p_glBindFramebuffer
#define glFramebufferTexture2D p_glFramebufferTexture2D
#define glFramebufferRenderbuffer p_glFramebufferRenderbuffer
#define glGenRenderbuffers p_glGenRenderbuffers
#define glBindRenderbuffer p_glBindRenderbuffer
#define glRenderbufferStorage p_glRenderbufferStorage
#define glDeleteRenderbuffers p_glDeleteRenderbuffers
#define glCheckFramebufferStatus p_glCheckFramebufferStatus
#define glCreateShader p_glCreateShader
#define glShaderSource p_glShaderSource
#define glCompileShader p_glCompileShader
#define glGetShaderiv p_glGetShaderiv
#define glGetShaderInfoLog p_glGetShaderInfoLog
#define glCreateProgram p_glCreateProgram
#define glAttachShader p_glAttachShader
#define glLinkProgram p_glLinkProgram
#define glGetProgramiv p_glGetProgramiv
#define glGetProgramInfoLog p_glGetProgramInfoLog
#define glUseProgram p_glUseProgram
#define glBindAttribLocation p_glBindAttribLocation
#define glGetUniformLocation p_glGetUniformLocation
#define glUniform1i p_glUniform1i
#define glUniform1f p_glUniform1f
#define glUniform4f p_glUniform4f
#define glUniform4fv p_glUniform4fv
#define glUniformMatrix4fv p_glUniformMatrix4fv
#define glVertexAttribPointer p_glVertexAttribPointer
#define glEnableVertexAttribArray p_glEnableVertexAttribArray
#define glDisableVertexAttribArray p_glDisableVertexAttribArray
#define glVertexAttrib4f p_glVertexAttrib4f
#define glDrawArrays p_glDrawArrays
#define glDrawElements p_glDrawElements
#define glViewport p_glViewport
#define glScissor p_glScissor
#define glEnable p_glEnable
#define glDisable p_glDisable
#define glBlendFunc p_glBlendFunc
#define glBlendFuncSeparate p_glBlendFuncSeparate
#define glBlendEquation p_glBlendEquation
#define glDepthFunc p_glDepthFunc
#define glDepthMask p_glDepthMask
#define glClearColor p_glClearColor
#define glClear p_glClear
#define glColorMask p_glColorMask
#define glReadPixels p_glReadPixels
#define glPixelStorei p_glPixelStorei
#define glGetError p_glGetError
#define glGetString p_glGetString
#define glFinish p_glFinish
#define glDeleteShader p_glDeleteShader
#define glDeleteProgram p_glDeleteProgram
#define glGenerateMipmap p_glGenerateMipmap
#define glGetIntegerv p_glGetIntegerv
#define glClearDepthf p_glClearDepthf
#endif
