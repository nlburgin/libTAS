/*
    Copyright 2015-2018 Clément Gallet <clement.gallet@ens-lyon.org>

    This file is part of libTAS.

    libTAS is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    libTAS is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with libTAS.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef LIBTAS_ENABLE_HUD

#include "RenderHUD_GL.h"
#include "../logging.h"
#include "../hook.h"
#include "../ScreenCapture.h"

namespace libtas {

DEFINE_ORIG_POINTER(glGetIntegerv)
DEFINE_ORIG_POINTER(glGenTextures)
DEFINE_ORIG_POINTER(glDeleteTextures)
DEFINE_ORIG_POINTER(glBindTexture)
DEFINE_ORIG_POINTER(glTexParameteri)
DEFINE_ORIG_POINTER(glTexImage2D)
DEFINE_ORIG_POINTER(glActiveTexture)

DECLARE_ORIG_POINTER(glGenFramebuffers)
DECLARE_ORIG_POINTER(glBindFramebuffer)
DEFINE_ORIG_POINTER(glFramebufferTexture2D)
DECLARE_ORIG_POINTER(glDeleteFramebuffers)
DECLARE_ORIG_POINTER(glBlitFramebuffer)

DEFINE_ORIG_POINTER(glUseProgram)
DEFINE_ORIG_POINTER(glPixelStorei)

GLuint RenderHUD_GL::texture = 0;
GLuint RenderHUD_GL::fbo = 0;

RenderHUD_GL::RenderHUD_GL() : RenderHUD() {}

RenderHUD_GL::~RenderHUD_GL() {
    fini();
}

void RenderHUD_GL::init()
{
    if (texture == 0) {
        LINK_NAMESPACE(glGenTextures, "GL");
        LINK_NAMESPACE(glGetIntegerv, "GL");
        LINK_NAMESPACE(glActiveTexture, "GL");
        LINK_NAMESPACE(glDeleteTextures, "GL");
        LINK_NAMESPACE(glBindTexture, "GL");

        LINK_NAMESPACE(glGenFramebuffers, "GL");
        LINK_NAMESPACE(glBindFramebuffer, "GL");
        LINK_NAMESPACE(glFramebufferTexture2D, "GL");
        LINK_NAMESPACE(glDeleteFramebuffers, "GL");


        /* Get previous active texture */
        GLint oldTex;
        orig::glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTex);
        GLint oldActiveTex;
        orig::glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTex);

        orig::glActiveTexture(GL_TEXTURE0);
        orig::glGenTextures(1, &texture);

        orig::glGenFramebuffers(1, &fbo);

        if (oldTex != 0) {
            orig::glBindTexture(GL_TEXTURE_2D, oldTex);
        }
        /* Restore previous active texture */
        if (oldActiveTex != 0) {
            orig::glActiveTexture(oldActiveTex);
        }
    }
}

void RenderHUD_GL::fini()
{
    if (texture != 0) {
        orig::glDeleteTextures(1, &texture);
        texture = 0;
    }
    if (fbo != 0) {
        orig::glDeleteFramebuffers(1, &fbo);
        fbo = 0;
    }
}

void RenderHUD_GL::renderText(const char* text, Color fg_color, Color bg_color, int x, int y)
{
    LINK_NAMESPACE(glBindTexture, "GL");
    LINK_NAMESPACE(glTexImage2D, "GL");
    LINK_NAMESPACE(glTexParameteri, "GL");

    LINK_NAMESPACE(glBlitFramebuffer, "GL");
    LINK_NAMESPACE(glUseProgram, "GL");
    LINK_NAMESPACE(glGetIntegerv, "GL");
    LINK_NAMESPACE(glPixelStorei, "GL");

    LINK_NAMESPACE(glActiveTexture, "GL");
    LINK_NAMESPACE(glBindFramebuffer, "GL");
    LINK_NAMESPACE(glFramebufferTexture2D, "GL");

    /* Save the previous program */
    GLint oldProgram;
    orig::glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
    if (oldProgram != 0) {
        orig::glUseProgram(0);
    }

    /* Save previous unpack row length */
    GLint oldUnpackRow;
    orig::glGetIntegerv(GL_UNPACK_ROW_LENGTH, &oldUnpackRow);
    if (oldUnpackRow != 0) {
        orig::glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }

    /* Save previous binded texture and active texture unit */
    GLint oldTex;
    orig::glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTex);
    GLint oldActiveTex;
    orig::glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTex);

    /* Create our text as a texture */
    orig::glActiveTexture(GL_TEXTURE0);
    orig::glBindTexture(GL_TEXTURE_2D, texture);

    orig::glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    std::unique_ptr<SurfaceARGB> surf = createTextSurface(text, fg_color, bg_color);

    orig::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    orig::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    orig::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, surf->w, surf->h, 0, GL_BGRA, GL_UNSIGNED_BYTE, surf->pixels.data());
    orig::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

    /* Blit the textured framebuffer into the screen and flip y-coord */
    int width, height;
    ScreenCapture::getDimensions(width, height);

    /* Change the coords so that the text fills on screen */
    x = (x + surf->w + 5) > width ? (width - surf->w - 5) : x;
    y = (y + surf->h + 5) > height ? (height - surf->h - 5) : y;

    orig::glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    orig::glBlitFramebuffer(0, 0, surf->w, surf->h, x, height-y, x+surf->w, height-(y+surf->h),
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    orig::glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    /* Restore previous binded texture and active texture unit */
    if (oldTex != 0) {
        orig::glBindTexture(GL_TEXTURE_2D, oldTex);
    }
    if (oldActiveTex != 0) {
        orig::glActiveTexture(oldActiveTex);
    }

    /* Restore unpack row length */
    if (oldUnpackRow != 0) {
        orig::glPixelStorei(GL_UNPACK_ROW_LENGTH, oldUnpackRow);
    }

    /* Restore the previous program */
    orig::glUseProgram(oldProgram);
}

}

#endif
