/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2018 Baldur Karlsson
 * Copyright (c) 2014 Crytek
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include <float.h>
#include <algorithm>
#include "common/common.h"
#include "data/glsl_shaders.h"
#include "maths/camera.h"
#include "maths/formatpacking.h"
#include "maths/matrix.h"
#include "strings/string_utils.h"
#include "gl_driver.h"
#include "gl_replay.h"
#include "gl_resources.h"

#define OPENGL 1
#include "data/glsl/debuguniforms.h"

GLuint GLReplay::CreateShader(GLenum shaderType, const std::vector<std::string> &sources)
{
  const GLHookSet &gl = m_pDriver->GetHookset();

  GLuint ret = gl.glCreateShader(shaderType);

  std::vector<const char *> srcs;
  srcs.reserve(sources.size());
  for(size_t i = 0; i < sources.size(); i++)
    srcs.push_back(sources[i].c_str());

  gl.glShaderSource(ret, (GLsizei)srcs.size(), &srcs[0], NULL);

  gl.glCompileShader(ret);

  char buffer[1024] = {};
  GLint status = 0;
  gl.glGetShaderiv(ret, eGL_COMPILE_STATUS, &status);
  if(status == 0)
  {
    gl.glGetShaderInfoLog(ret, 1024, NULL, buffer);
    RDCERR("%s compile error: %s", ToStr(shaderType).c_str(), buffer);
    return 0;
  }

  return ret;
}

GLuint GLReplay::CreateCShaderProgram(const std::vector<std::string> &csSources)
{
  if(m_pDriver == NULL)
    return 0;

  MakeCurrentReplayContext(m_DebugCtx);

  const GLHookSet &gl = m_pDriver->GetHookset();

  GLuint cs = CreateShader(eGL_COMPUTE_SHADER, csSources);
  if(cs == 0)
    return 0;

  GLuint ret = gl.glCreateProgram();

  gl.glAttachShader(ret, cs);

  gl.glLinkProgram(ret);

  char buffer[1024] = {};
  GLint status = 0;
  gl.glGetProgramiv(ret, eGL_LINK_STATUS, &status);
  if(status == 0)
  {
    gl.glGetProgramInfoLog(ret, 1024, NULL, buffer);
    RDCERR("Link error: %s", buffer);
  }

  gl.glDetachShader(ret, cs);

  gl.glDeleteShader(cs);

  return ret;
}

GLuint GLReplay::CreateShaderProgram(const std::vector<std::string> &vs,
                                     const std::vector<std::string> &fs)
{
  std::vector<std::string> empty;
  return CreateShaderProgram(vs, fs, empty);
}

GLuint GLReplay::CreateShaderProgram(const std::vector<std::string> &vsSources,
                                     const std::vector<std::string> &fsSources,
                                     const std::vector<std::string> &gsSources)
{
  if(m_pDriver == NULL)
    return 0;

  MakeCurrentReplayContext(m_DebugCtx);

  const GLHookSet &gl = m_pDriver->GetHookset();

  GLuint vs = 0;
  GLuint fs = 0;
  GLuint gs = 0;

  if(vsSources.empty())
  {
    RDCERR("Must have vertex shader - no separable programs supported.");
    return 0;
  }

  if(fsSources.empty())
  {
    RDCERR("Must have fragment shader - no separable programs supported.");
    return 0;
  }

  vs = CreateShader(eGL_VERTEX_SHADER, vsSources);
  if(vs == 0)
    return 0;

  fs = CreateShader(eGL_FRAGMENT_SHADER, fsSources);
  if(fs == 0)
    return 0;

  if(!gsSources.empty())
  {
    gs = CreateShader(eGL_GEOMETRY_SHADER, gsSources);
    if(gs == 0)
      return 0;
  }

  GLuint ret = gl.glCreateProgram();

  gl.glAttachShader(ret, vs);
  gl.glAttachShader(ret, fs);
  if(gs)
    gl.glAttachShader(ret, gs);

  gl.glLinkProgram(ret);

  char buffer[1024] = {};
  GLint status = 0;
  gl.glGetProgramiv(ret, eGL_LINK_STATUS, &status);
  if(status == 0)
  {
    gl.glGetProgramInfoLog(ret, 1024, NULL, buffer);
    RDCERR("Shader error: %s", buffer);
  }

  gl.glDetachShader(ret, vs);
  gl.glDetachShader(ret, fs);
  if(gs)
    gl.glDetachShader(ret, gs);

  gl.glDeleteShader(vs);
  gl.glDeleteShader(fs);
  if(gs)
    gl.glDeleteShader(gs);

  return ret;
}

void GLReplay::CheckGLSLVersion(const char *sl, int &glslVersion)
{
  // GL_SHADING_LANGUAGE_VERSION for OpenGL ES:
  //   "OpenGL ES GLSL ES N.M vendor-specific information"
  static const char *const GLSL_ES_STR = "OpenGL ES GLSL ES";
  if(strncmp(sl, GLSL_ES_STR, 17) == 0)
    sl += 18;

  if(sl[0] >= '0' && sl[0] <= '9' && sl[1] == '.' && sl[2] >= '0' && sl[2] <= '9')
  {
    int major = int(sl[0] - '0');
    int minor = int(sl[2] - '0');
    int ver = major * 100 + minor * 10;

    if(ver > glslVersion)
      glslVersion = ver;
  }

  if(sl[0] >= '0' && sl[0] <= '9' && sl[1] >= '0' && sl[1] <= '9' && sl[2] == '0')
  {
    int major = int(sl[0] - '0');
    int minor = int(sl[1] - '0');
    int ver = major * 100 + minor * 10;

    if(ver > glslVersion)
      glslVersion = ver;
  }
}

void GLReplay::InitDebugData()
{
  if(m_pDriver == NULL)
    return;

  m_HighlightCache.driver = m_pDriver->GetReplay();

  RenderDoc::Inst().SetProgress(LoadProgress::DebugManagerInit, 0.0f);

  {
    WindowingData window = {WindowingSystem::Unknown};
    uint64_t id = MakeOutputWindow(window, true);

    m_DebugID = id;
    m_DebugCtx = &m_OutputWindows[id];

    MakeCurrentReplayContext(m_DebugCtx);

    m_pDriver->RegisterDebugCallback();
  }

  WrappedOpenGL &gl = *m_pDriver;

  DebugData.outWidth = 0.0f;
  DebugData.outHeight = 0.0f;

  std::vector<std::string> vs;
  std::vector<std::string> fs;
  std::vector<std::string> gs;
  std::vector<std::string> cs;

  int glslVersion;
  int glslBaseVer;
  int glslCSVer;    // compute shader
  ShaderType shaderType;

  if(IsGLES)
  {
    glslVersion = glslBaseVer = glslCSVer = 310;
    shaderType = eShaderGLSLES;
  }
  else
  {
    glslVersion = glslBaseVer = 150;
    glslCSVer = 420;
    shaderType = eShaderGLSL;
  }

  // TODO In case of GLES some currently unused shaders, which are guarded by HasExt[..] checks,
  // still contain compile errors (e.g. array2ms.comp, ms2array.comp, quad*, etc.).
  bool glesShadersAreComplete = !IsGLES;

  GenerateGLSLShader(vs, shaderType, "", GetEmbeddedResource(glsl_blit_vert), glslBaseVer);

  // used to combine with custom shaders.
  DebugData.texDisplayVertexShader = CreateShader(eGL_VERTEX_SHADER, vs);

  for(int i = 0; i < 3; i++)
  {
    string defines = string("#define UINT_TEX ") + (i == 1 ? "1" : "0") + "\n";
    defines += string("#define SINT_TEX ") + (i == 2 ? "1" : "0") + "\n";

    GenerateGLSLShader(fs, shaderType, defines, GetEmbeddedResource(glsl_texdisplay_frag),
                       glslBaseVer);

    DebugData.texDisplayProg[i] = CreateShaderProgram(vs, fs);
  }

  RenderDoc::Inst().SetProgress(LoadProgress::DebugManagerInit, 0.2f);

  if(GLCoreVersion >= 43 && !IsGLES)
  {
    GLint numsl = 0;
    gl.glGetIntegerv(eGL_NUM_SHADING_LANGUAGE_VERSIONS, &numsl);

    for(GLint i = 0; i < numsl; i++)
    {
      const char *sl = (const char *)gl.glGetStringi(eGL_SHADING_LANGUAGE_VERSION, (GLuint)i);

      CheckGLSLVersion(sl, glslVersion);
    }
  }
  else
  {
    const char *sl = (const char *)gl.glGetString(eGL_SHADING_LANGUAGE_VERSION);

    CheckGLSLVersion(sl, glslVersion);
  }

  DebugData.glslVersion = glslVersion;

  RDCLOG("GLSL version %d", glslVersion);

  GenerateGLSLShader(vs, shaderType, "", GetEmbeddedResource(glsl_blit_vert), glslBaseVer);

  DebugData.fixedcolFragShader = DebugData.quadoverdrawFragShader = 0;

  if(glesShadersAreComplete && HasExt[ARB_shader_image_load_store] && HasExt[ARB_gpu_shader5])
  {
    GenerateGLSLShader(fs, shaderType, "", GetEmbeddedResource(glsl_quadresolve_frag), glslBaseVer);

    DebugData.quadoverdrawResolveProg = CreateShaderProgram(vs, fs);
  }
  else
  {
    RDCWARN(
        "GL_ARB_shader_image_load_store/GL_ARB_gpu_shader5 not supported, disabling quad overdraw "
        "feature.");
    m_pDriver->AddDebugMessage(MessageCategory::Portability, MessageSeverity::Medium,
                               MessageSource::RuntimeWarning,
                               "GL_ARB_shader_image_load_store/GL_ARB_gpu_shader5 not supported, "
                               "disabling quad overdraw feature.");
    DebugData.quadoverdrawResolveProg = 0;
  }

  GenerateGLSLShader(fs, shaderType, "", GetEmbeddedResource(glsl_checkerboard_frag), glslBaseVer);
  DebugData.checkerProg = CreateShaderProgram(vs, fs);

  if(HasExt[ARB_geometry_shader4])
  {
    GenerateGLSLShader(vs, shaderType, "", GetEmbeddedResource(glsl_mesh_vert), glslBaseVer);
    GenerateGLSLShader(fs, shaderType, "", GetEmbeddedResource(glsl_mesh_frag), glslBaseVer);
    GenerateGLSLShader(gs, shaderType, "", GetEmbeddedResource(glsl_mesh_geom), glslBaseVer);

    DebugData.meshProg = CreateShaderProgram(vs, fs);
    DebugData.meshgsProg = CreateShaderProgram(vs, fs, gs);

    GenerateGLSLShader(fs, shaderType, "", GetEmbeddedResource(glsl_trisize_frag), glslBaseVer);
    GenerateGLSLShader(gs, shaderType, "", GetEmbeddedResource(glsl_trisize_geom), glslBaseVer);

    DebugData.trisizeProg = CreateShaderProgram(vs, fs, gs);
  }
  else
  {
    GenerateGLSLShader(vs, shaderType, "", GetEmbeddedResource(glsl_mesh_vert), glslBaseVer);
    GenerateGLSLShader(fs, shaderType, "", GetEmbeddedResource(glsl_mesh_frag), glslBaseVer);

    DebugData.meshProg = CreateShaderProgram(vs, fs);
    DebugData.meshgsProg = 0;
    DebugData.trisizeProg = 0;

    const char *warning_msg =
        "GL_ARB_geometry_shader4/GL_EXT_geometry_shader not supported, disabling triangle size and "
        "lit solid shading feature.";
    RDCWARN(warning_msg);
    m_pDriver->AddDebugMessage(MessageCategory::Portability, MessageSeverity::Medium,
                               MessageSource::RuntimeWarning, warning_msg);
  }

  RenderDoc::Inst().SetProgress(LoadProgress::DebugManagerInit, 0.4f);

  gl.glGenSamplers(1, &DebugData.linearSampler);
  gl.glSamplerParameteri(DebugData.linearSampler, eGL_TEXTURE_MIN_FILTER, eGL_LINEAR);
  gl.glSamplerParameteri(DebugData.linearSampler, eGL_TEXTURE_MAG_FILTER, eGL_LINEAR);
  gl.glSamplerParameteri(DebugData.linearSampler, eGL_TEXTURE_WRAP_S, eGL_CLAMP_TO_EDGE);
  gl.glSamplerParameteri(DebugData.linearSampler, eGL_TEXTURE_WRAP_T, eGL_CLAMP_TO_EDGE);

  gl.glGenSamplers(1, &DebugData.pointSampler);
  gl.glSamplerParameteri(DebugData.pointSampler, eGL_TEXTURE_MIN_FILTER, eGL_NEAREST_MIPMAP_NEAREST);
  gl.glSamplerParameteri(DebugData.pointSampler, eGL_TEXTURE_MAG_FILTER, eGL_NEAREST);
  gl.glSamplerParameteri(DebugData.pointSampler, eGL_TEXTURE_WRAP_S, eGL_CLAMP_TO_EDGE);
  gl.glSamplerParameteri(DebugData.pointSampler, eGL_TEXTURE_WRAP_T, eGL_CLAMP_TO_EDGE);

  gl.glGenSamplers(1, &DebugData.pointNoMipSampler);
  gl.glSamplerParameteri(DebugData.pointNoMipSampler, eGL_TEXTURE_MIN_FILTER, eGL_NEAREST);
  gl.glSamplerParameteri(DebugData.pointNoMipSampler, eGL_TEXTURE_MAG_FILTER, eGL_NEAREST);
  gl.glSamplerParameteri(DebugData.pointNoMipSampler, eGL_TEXTURE_WRAP_S, eGL_CLAMP_TO_EDGE);
  gl.glSamplerParameteri(DebugData.pointNoMipSampler, eGL_TEXTURE_WRAP_T, eGL_CLAMP_TO_EDGE);

  gl.glGenBuffers(ARRAY_COUNT(DebugData.UBOs), DebugData.UBOs);
  for(size_t i = 0; i < ARRAY_COUNT(DebugData.UBOs); i++)
  {
    gl.glBindBuffer(eGL_UNIFORM_BUFFER, DebugData.UBOs[i]);
    gl.glNamedBufferDataEXT(DebugData.UBOs[i], 2048, NULL, eGL_DYNAMIC_DRAW);
    RDCCOMPILE_ASSERT(sizeof(TexDisplayUBOData) <= 2048, "UBO too small");
    RDCCOMPILE_ASSERT(sizeof(FontUBOData) <= 2048, "UBO too small");
    RDCCOMPILE_ASSERT(sizeof(HistogramUBOData) <= 2048, "UBO too small");
    RDCCOMPILE_ASSERT(sizeof(overdrawRamp) <= 2048, "UBO too small");
  }

  DebugData.overlayTexWidth = DebugData.overlayTexHeight = DebugData.overlayTexSamples = 0;
  DebugData.overlayTex = DebugData.overlayFBO = 0;

  DebugData.overlayProg = 0;

  gl.glGenFramebuffers(1, &DebugData.customFBO);
  gl.glBindFramebuffer(eGL_FRAMEBUFFER, DebugData.customFBO);
  DebugData.customTex = 0;

  gl.glGenFramebuffers(1, &DebugData.pickPixelFBO);
  gl.glBindFramebuffer(eGL_FRAMEBUFFER, DebugData.pickPixelFBO);

  gl.glGenTextures(1, &DebugData.pickPixelTex);
  gl.glBindTexture(eGL_TEXTURE_2D, DebugData.pickPixelTex);

  gl.glTextureImage2DEXT(DebugData.pickPixelTex, eGL_TEXTURE_2D, 0, eGL_RGBA32F, 1, 1, 0, eGL_RGBA,
                         eGL_FLOAT, NULL);
  gl.glTexParameteri(eGL_TEXTURE_2D, eGL_TEXTURE_MAX_LEVEL, 0);
  gl.glTexParameteri(eGL_TEXTURE_2D, eGL_TEXTURE_MIN_FILTER, eGL_NEAREST);
  gl.glTexParameteri(eGL_TEXTURE_2D, eGL_TEXTURE_MAG_FILTER, eGL_NEAREST);
  gl.glTexParameteri(eGL_TEXTURE_2D, eGL_TEXTURE_WRAP_S, eGL_CLAMP_TO_EDGE);
  gl.glTexParameteri(eGL_TEXTURE_2D, eGL_TEXTURE_WRAP_T, eGL_CLAMP_TO_EDGE);
  gl.glFramebufferTexture(eGL_FRAMEBUFFER, eGL_COLOR_ATTACHMENT0, DebugData.pickPixelTex, 0);

  gl.glGenVertexArrays(1, &DebugData.emptyVAO);
  gl.glBindVertexArray(DebugData.emptyVAO);

  RenderDoc::Inst().SetProgress(LoadProgress::DebugManagerInit, 0.6f);

  // histogram/minmax data
  {
    RDCEraseEl(DebugData.minmaxTileProgram);
    RDCEraseEl(DebugData.histogramProgram);
    RDCEraseEl(DebugData.minmaxResultProgram);

    RDCCOMPILE_ASSERT(
        ARRAY_COUNT(DebugData.minmaxTileProgram) >= (TEXDISPLAY_SINT_TEX | TEXDISPLAY_TYPEMASK) + 1,
        "not enough programs");

    string extensions =
        "#extension GL_ARB_compute_shader : require\n"
        "#extension GL_ARB_shader_storage_buffer_object : require\n";

    for(int t = 1; glesShadersAreComplete && HasExt[ARB_compute_shader] && t <= RESTYPE_TEXTYPEMAX;
        t++)
    {
      // float, uint, sint
      for(int i = 0; i < 3; i++)
      {
        int idx = t;
        if(i == 1)
          idx |= TEXDISPLAY_UINT_TEX;
        if(i == 2)
          idx |= TEXDISPLAY_SINT_TEX;

        {
          string defines = extensions;
          defines += string("#define SHADER_RESTYPE ") + ToStr(t) + "\n";
          defines += string("#define UINT_TEX ") + (i == 1 ? "1" : "0") + "\n";
          defines += string("#define SINT_TEX ") + (i == 2 ? "1" : "0") + "\n";

          GenerateGLSLShader(cs, shaderType, defines, GetEmbeddedResource(glsl_minmaxtile_comp),
                             glslCSVer);

          DebugData.minmaxTileProgram[idx] = CreateCShaderProgram(cs);
        }

        {
          string defines = extensions;
          defines += string("#define SHADER_RESTYPE ") + ToStr(t) + "\n";
          defines += string("#define UINT_TEX ") + (i == 1 ? "1" : "0") + "\n";
          defines += string("#define SINT_TEX ") + (i == 2 ? "1" : "0") + "\n";

          GenerateGLSLShader(cs, shaderType, defines, GetEmbeddedResource(glsl_histogram_comp),
                             glslCSVer);

          DebugData.histogramProgram[idx] = CreateCShaderProgram(cs);
        }

        if(t == 1)
        {
          string defines = extensions;
          defines += string("#define SHADER_RESTYPE ") + ToStr(t) + "\n";
          defines += string("#define UINT_TEX ") + (i == 1 ? "1" : "0") + "\n";
          defines += string("#define SINT_TEX ") + (i == 2 ? "1" : "0") + "\n";

          GenerateGLSLShader(cs, shaderType, defines, GetEmbeddedResource(glsl_minmaxresult_comp),
                             glslCSVer);

          DebugData.minmaxResultProgram[i] = CreateCShaderProgram(cs);
        }
      }
    }

    if(!HasExt[ARB_compute_shader])
    {
      RDCWARN("GL_ARB_compute_shader not supported, disabling min/max and histogram features.");
      m_pDriver->AddDebugMessage(
          MessageCategory::Portability, MessageSeverity::Medium, MessageSource::RuntimeWarning,
          "GL_ARB_compute_shader not supported, disabling min/max and histogram features.");
    }

    gl.glGenBuffers(1, &DebugData.minmaxTileResult);
    gl.glGenBuffers(1, &DebugData.minmaxResult);
    gl.glGenBuffers(1, &DebugData.histogramBuf);

    const uint32_t maxTexDim = 16384;
    const uint32_t blockPixSize = HGRAM_PIXELS_PER_TILE * HGRAM_TILES_PER_BLOCK;
    const uint32_t maxBlocksNeeded = (maxTexDim * maxTexDim) / (blockPixSize * blockPixSize);

    const size_t byteSize =
        2 * sizeof(Vec4f) * HGRAM_TILES_PER_BLOCK * HGRAM_TILES_PER_BLOCK * maxBlocksNeeded;

    gl.glNamedBufferDataEXT(DebugData.minmaxTileResult, byteSize, NULL, eGL_DYNAMIC_DRAW);
    gl.glNamedBufferDataEXT(DebugData.minmaxResult, sizeof(Vec4f) * 2, NULL, eGL_DYNAMIC_READ);
    gl.glNamedBufferDataEXT(DebugData.histogramBuf, sizeof(uint32_t) * 4 * HGRAM_NUM_BUCKETS, NULL,
                            eGL_DYNAMIC_READ);
  }

  if(glesShadersAreComplete && HasExt[ARB_compute_shader])
  {
    GenerateGLSLShader(cs, shaderType, "", GetEmbeddedResource(glsl_ms2array_comp), glslCSVer);
    DebugData.MS2Array = CreateCShaderProgram(cs);

    GenerateGLSLShader(cs, shaderType, "", GetEmbeddedResource(glsl_array2ms_comp), glslCSVer);
    DebugData.Array2MS = CreateCShaderProgram(cs);
  }
  else
  {
    DebugData.MS2Array = 0;
    DebugData.Array2MS = 0;
    RDCWARN("GL_ARB_compute_shader not supported, disabling 2DMS save/load.");
    m_pDriver->AddDebugMessage(MessageCategory::Portability, MessageSeverity::Medium,
                               MessageSource::RuntimeWarning,
                               "GL_ARB_compute_shader not supported, disabling 2DMS save/load.");
  }

  if(glesShadersAreComplete && HasExt[ARB_compute_shader])
  {
    string defines =
        "#extension GL_ARB_compute_shader : require\n"
        "#extension GL_ARB_shader_storage_buffer_object : require";
    GenerateGLSLShader(cs, shaderType, defines, GetEmbeddedResource(glsl_mesh_comp), glslCSVer);
    DebugData.meshPickProgram = CreateCShaderProgram(cs);
  }
  else
  {
    DebugData.meshPickProgram = 0;
    RDCWARN("GL_ARB_compute_shader not supported, disabling mesh picking.");
    m_pDriver->AddDebugMessage(MessageCategory::Portability, MessageSeverity::Medium,
                               MessageSource::RuntimeWarning,
                               "GL_ARB_compute_shader not supported, disabling mesh picking.");
  }

  RenderDoc::Inst().SetProgress(LoadProgress::DebugManagerInit, 0.8f);

  DebugData.pickResultBuf = 0;

  if(DebugData.meshPickProgram)
  {
    gl.glGenBuffers(1, &DebugData.pickResultBuf);
    gl.glBindBuffer(eGL_SHADER_STORAGE_BUFFER, DebugData.pickResultBuf);
    gl.glNamedBufferDataEXT(DebugData.pickResultBuf,
                            sizeof(Vec4f) * DebugRenderData::maxMeshPicks + sizeof(uint32_t) * 4,
                            NULL, eGL_DYNAMIC_READ);

    // sized/created on demand
    DebugData.pickVBBuf = DebugData.pickIBBuf = 0;
    DebugData.pickVBSize = DebugData.pickIBSize = 0;
  }

  gl.glGenVertexArrays(1, &DebugData.meshVAO);
  gl.glBindVertexArray(DebugData.meshVAO);

  gl.glGenBuffers(1, &DebugData.axisFrustumBuffer);
  gl.glBindBuffer(eGL_ARRAY_BUFFER, DebugData.axisFrustumBuffer);

  Vec3f TLN = Vec3f(-1.0f, 1.0f, 0.0f);    // TopLeftNear, etc...
  Vec3f TRN = Vec3f(1.0f, 1.0f, 0.0f);
  Vec3f BLN = Vec3f(-1.0f, -1.0f, 0.0f);
  Vec3f BRN = Vec3f(1.0f, -1.0f, 0.0f);

  Vec3f TLF = Vec3f(-1.0f, 1.0f, 1.0f);
  Vec3f TRF = Vec3f(1.0f, 1.0f, 1.0f);
  Vec3f BLF = Vec3f(-1.0f, -1.0f, 1.0f);
  Vec3f BRF = Vec3f(1.0f, -1.0f, 1.0f);

  Vec3f axisFrustum[] = {
      // axis marker vertices
      Vec3f(0.0f, 0.0f, 0.0f), Vec3f(1.0f, 0.0f, 0.0f), Vec3f(0.0f, 0.0f, 0.0f),
      Vec3f(0.0f, 1.0f, 0.0f), Vec3f(0.0f, 0.0f, 0.0f), Vec3f(0.0f, 0.0f, 1.0f),

      // frustum vertices
      TLN, TRN, TRN, BRN, BRN, BLN, BLN, TLN,

      TLN, TLF, TRN, TRF, BLN, BLF, BRN, BRF,

      TLF, TRF, TRF, BRF, BRF, BLF, BLF, TLF,
  };

  gl.glNamedBufferDataEXT(DebugData.axisFrustumBuffer, sizeof(axisFrustum), axisFrustum,
                          eGL_STATIC_DRAW);

  gl.glGenVertexArrays(1, &DebugData.axisVAO);
  gl.glBindVertexArray(DebugData.axisVAO);
  gl.glVertexAttribPointer(0, 3, eGL_FLOAT, GL_FALSE, sizeof(Vec3f), NULL);
  gl.glEnableVertexAttribArray(0);

  gl.glGenVertexArrays(1, &DebugData.frustumVAO);
  gl.glBindVertexArray(DebugData.frustumVAO);
  gl.glVertexAttribPointer(0, 3, eGL_FLOAT, GL_FALSE, sizeof(Vec3f),
                           (const void *)(sizeof(Vec3f) * 6));
  gl.glEnableVertexAttribArray(0);

  gl.glGenVertexArrays(1, &DebugData.triHighlightVAO);
  gl.glBindVertexArray(DebugData.triHighlightVAO);

  gl.glGenBuffers(1, &DebugData.triHighlightBuffer);
  gl.glBindBuffer(eGL_ARRAY_BUFFER, DebugData.triHighlightBuffer);

  gl.glNamedBufferDataEXT(DebugData.triHighlightBuffer, sizeof(Vec4f) * 24, NULL, eGL_DYNAMIC_DRAW);

  gl.glVertexAttribPointer(0, 4, eGL_FLOAT, GL_FALSE, sizeof(Vec4f), NULL);
  gl.glEnableVertexAttribArray(0);

  GenerateGLSLShader(vs, shaderType, "", GetEmbeddedResource(glsl_blit_vert), glslBaseVer);
  GenerateGLSLShader(fs, shaderType, "", GetEmbeddedResource(glsl_outline_frag), glslBaseVer);

  DebugData.outlineQuadProg = CreateShaderProgram(vs, fs);

  MakeCurrentReplayContext(&m_ReplayCtx);

  // try to identify the GPU we're running on.
  {
    const char *vendor = (const char *)gl.glGetString(eGL_VENDOR);
    const char *renderer = (const char *)gl.glGetString(eGL_RENDERER);

    // we're just doing substring searches, so combine both for ease.
    std::string combined = (vendor ? vendor : "");
    combined += " ";
    combined += (renderer ? renderer : "");

    // make lowercase, for case-insensitive matching, and add preceding/trailing space for easier
    // 'word' matching
    combined = " " + strlower(combined) + " ";

    RDCDEBUG("Identifying vendor from '%s'", combined.c_str());

    struct pattern
    {
      const char *search;
      GPUVendor vendor;
    } patterns[] = {
        {" arm ", GPUVendor::ARM},
        {" mali ", GPUVendor::ARM},
        {" mali-", GPUVendor::ARM},
        {" amd ", GPUVendor::AMD},
        {"advanced micro devices", GPUVendor::AMD},
        {"ati technologies", GPUVendor::AMD},
        {"radeon", GPUVendor::AMD},
        {"broadcom", GPUVendor::Broadcom},
        {"imagination", GPUVendor::Imagination},
        {"powervr", GPUVendor::Imagination},
        {"intel", GPUVendor::Intel},
        {"geforce", GPUVendor::nVidia},
        {"quadro", GPUVendor::nVidia},
        {"nouveau", GPUVendor::nVidia},
        {"nvidia", GPUVendor::nVidia},
        {"adreno", GPUVendor::Qualcomm},
        {"qualcomm", GPUVendor::Qualcomm},
        {"vivante", GPUVendor::Verisilicon},
        {"llvmpipe", GPUVendor::Software},
        {"softpipe", GPUVendor::Software},
        {"bluestacks", GPUVendor::Software},
    };

    for(const pattern &p : patterns)
    {
      if(combined.find(p.search) != std::string::npos)
      {
        if(m_Vendor == GPUVendor::Unknown)
        {
          m_Vendor = p.vendor;
        }
        else
        {
          // either we already found this with another pattern, or we've identified two patterns and
          // it's ambiguous. Keep the first one we found, arbitrarily, but print a warning.
          if(m_Vendor != p.vendor)
          {
            RDCWARN("Already identified '%s' as %s, but now identified as %s", combined.c_str(),
                    ToStr(m_Vendor).c_str(), ToStr(p.vendor).c_str());
          }
        }
      }
    }

    RDCDEBUG("Identified GPU vendor '%s'", ToStr(m_Vendor).c_str());
  }

  // these below need to be made on the replay context, as they are context-specific (not shared)
  // and will be used on the replay context.

  gl.glGenTransformFeedbacks(1, &DebugData.feedbackObj);
  gl.glGenBuffers(1, &DebugData.feedbackBuffer);
  DebugData.feedbackQueries.push_back(0);
  gl.glGenQueries(1, &DebugData.feedbackQueries[0]);

  gl.glBindTransformFeedback(eGL_TRANSFORM_FEEDBACK, DebugData.feedbackObj);
  gl.glBindBuffer(eGL_TRANSFORM_FEEDBACK_BUFFER, DebugData.feedbackBuffer);
  gl.glNamedBufferDataEXT(DebugData.feedbackBuffer, DebugData.feedbackBufferSize, NULL,
                          eGL_DYNAMIC_READ);
  gl.glBindBufferBase(eGL_TRANSFORM_FEEDBACK_BUFFER, 0, DebugData.feedbackBuffer);
  gl.glBindTransformFeedback(eGL_TRANSFORM_FEEDBACK, 0);

  RenderDoc::Inst().SetProgress(LoadProgress::DebugManagerInit, 1.0f);

  if(!HasExt[ARB_gpu_shader5])
  {
    RDCWARN(
        "ARB_gpu_shader5 not supported, pixel picking and saving of integer textures may be "
        "inaccurate.");
    m_pDriver->AddDebugMessage(MessageCategory::Portability, MessageSeverity::Medium,
                               MessageSource::RuntimeWarning,
                               "ARB_gpu_shader5 not supported, pixel picking and saving of integer "
                               "textures may be inaccurate.");

    m_Degraded = true;
  }

  if(!HasExt[ARB_stencil_texturing])
  {
    RDCWARN("ARB_stencil_texturing not supported, stencil values will not be displayed or picked.");
    m_pDriver->AddDebugMessage(
        MessageCategory::Portability, MessageSeverity::Medium, MessageSource::RuntimeWarning,
        "ARB_stencil_texturing not supported, stencil values will not be displayed or picked.");

    m_Degraded = true;
  }

  if(!HasExt[ARB_shader_image_load_store] || !HasExt[ARB_compute_shader])
  {
    m_Degraded = true;
  }
}

void GLReplay::DeleteDebugData()
{
  WrappedOpenGL &gl = *m_pDriver;

  MakeCurrentReplayContext(&m_ReplayCtx);

  if(DebugData.overlayProg != 0)
    gl.glDeleteProgram(DebugData.overlayProg);

  gl.glDeleteTransformFeedbacks(1, &DebugData.feedbackObj);
  gl.glDeleteBuffers(1, &DebugData.feedbackBuffer);
  gl.glDeleteQueries((GLsizei)DebugData.feedbackQueries.size(), DebugData.feedbackQueries.data());

  MakeCurrentReplayContext(m_DebugCtx);

  ClearPostVSCache();

  gl.glDeleteFramebuffers(1, &DebugData.overlayFBO);
  gl.glDeleteTextures(1, &DebugData.overlayTex);

  gl.glDeleteShader(DebugData.quadoverdrawFragShader);
  gl.glDeleteProgram(DebugData.quadoverdrawResolveProg);

  gl.glDeleteShader(DebugData.texDisplayVertexShader);
  for(int i = 0; i < 3; i++)
    gl.glDeleteProgram(DebugData.texDisplayProg[i]);

  gl.glDeleteProgram(DebugData.checkerProg);
  if(DebugData.fixedcolFragShader)
    gl.glDeleteShader(DebugData.fixedcolFragShader);
  gl.glDeleteProgram(DebugData.meshProg);
  gl.glDeleteProgram(DebugData.meshgsProg);
  gl.glDeleteProgram(DebugData.trisizeProg);

  gl.glDeleteSamplers(1, &DebugData.linearSampler);
  gl.glDeleteSamplers(1, &DebugData.pointSampler);
  gl.glDeleteSamplers(1, &DebugData.pointNoMipSampler);
  gl.glDeleteBuffers(ARRAY_COUNT(DebugData.UBOs), DebugData.UBOs);
  gl.glDeleteFramebuffers(1, &DebugData.pickPixelFBO);
  gl.glDeleteTextures(1, &DebugData.pickPixelTex);

  gl.glDeleteBuffers(1, &DebugData.genericUBO);

  gl.glDeleteFramebuffers(1, &DebugData.customFBO);
  gl.glDeleteTextures(1, &DebugData.customTex);

  gl.glDeleteVertexArrays(1, &DebugData.emptyVAO);

  for(int t = 1; t <= RESTYPE_TEXTYPEMAX; t++)
  {
    // float, uint, sint
    for(int i = 0; i < 3; i++)
    {
      int idx = t;
      if(i == 1)
        idx |= TEXDISPLAY_UINT_TEX;
      if(i == 2)
        idx |= TEXDISPLAY_SINT_TEX;

      gl.glDeleteProgram(DebugData.minmaxTileProgram[idx]);
      gl.glDeleteProgram(DebugData.histogramProgram[idx]);

      gl.glDeleteProgram(DebugData.minmaxResultProgram[i]);
      DebugData.minmaxResultProgram[i] = 0;
    }
  }

  gl.glDeleteProgram(DebugData.meshPickProgram);
  gl.glDeleteBuffers(1, &DebugData.pickIBBuf);
  gl.glDeleteBuffers(1, &DebugData.pickVBBuf);
  gl.glDeleteBuffers(1, &DebugData.pickResultBuf);

  gl.glDeleteProgram(DebugData.Array2MS);
  gl.glDeleteProgram(DebugData.MS2Array);

  gl.glDeleteBuffers(1, &DebugData.minmaxTileResult);
  gl.glDeleteBuffers(1, &DebugData.minmaxResult);
  gl.glDeleteBuffers(1, &DebugData.histogramBuf);

  gl.glDeleteVertexArrays(1, &DebugData.meshVAO);
  gl.glDeleteVertexArrays(1, &DebugData.axisVAO);
  gl.glDeleteVertexArrays(1, &DebugData.frustumVAO);
  gl.glDeleteVertexArrays(1, &DebugData.triHighlightVAO);

  gl.glDeleteBuffers(1, &DebugData.axisFrustumBuffer);
  gl.glDeleteBuffers(1, &DebugData.triHighlightBuffer);

  gl.glDeleteProgram(DebugData.outlineQuadProg);
}

bool GLReplay::GetMinMax(ResourceId texid, uint32_t sliceFace, uint32_t mip, uint32_t sample,
                         CompType typeHint, float *minval, float *maxval)
{
  if(texid == ResourceId() || m_pDriver->m_Textures.find(texid) == m_pDriver->m_Textures.end())
    return false;

  if(!HasExt[ARB_compute_shader])
    return false;

  auto &texDetails = m_pDriver->m_Textures[texid];

  TextureDescription details = GetTexture(texid);

  const GLHookSet &gl = m_pDriver->GetHookset();

  int texSlot = 0;
  int intIdx = 0;

  bool renderbuffer = false;

  switch(texDetails.curType)
  {
    case eGL_RENDERBUFFER:
      texSlot = RESTYPE_TEX2D;
      renderbuffer = true;
      break;
    case eGL_TEXTURE_1D: texSlot = RESTYPE_TEX1D; break;
    default:
      RDCWARN("Unexpected texture type");
    // fall through
    case eGL_TEXTURE_2D: texSlot = RESTYPE_TEX2D; break;
    case eGL_TEXTURE_2D_MULTISAMPLE: texSlot = RESTYPE_TEX2DMS; break;
    case eGL_TEXTURE_RECTANGLE: texSlot = RESTYPE_TEXRECT; break;
    case eGL_TEXTURE_BUFFER: texSlot = RESTYPE_TEXBUFFER; break;
    case eGL_TEXTURE_3D: texSlot = RESTYPE_TEX3D; break;
    case eGL_TEXTURE_CUBE_MAP: texSlot = RESTYPE_TEXCUBE; break;
    case eGL_TEXTURE_1D_ARRAY: texSlot = RESTYPE_TEX1DARRAY; break;
    case eGL_TEXTURE_2D_ARRAY: texSlot = RESTYPE_TEX2DARRAY; break;
    case eGL_TEXTURE_CUBE_MAP_ARRAY: texSlot = RESTYPE_TEXCUBEARRAY; break;
  }

  GLenum target = texDetails.curType;
  GLuint texname = texDetails.resource.name;

  // do blit from renderbuffer to texture, then sample from texture
  if(renderbuffer)
  {
    // need replay context active to do blit (as FBOs aren't shared)
    MakeCurrentReplayContext(&m_ReplayCtx);

    GLuint curDrawFBO = 0;
    GLuint curReadFBO = 0;
    gl.glGetIntegerv(eGL_DRAW_FRAMEBUFFER_BINDING, (GLint *)&curDrawFBO);
    gl.glGetIntegerv(eGL_READ_FRAMEBUFFER_BINDING, (GLint *)&curReadFBO);

    gl.glBindFramebuffer(eGL_DRAW_FRAMEBUFFER, texDetails.renderbufferFBOs[1]);
    gl.glBindFramebuffer(eGL_READ_FRAMEBUFFER, texDetails.renderbufferFBOs[0]);

    gl.glBlitFramebuffer(
        0, 0, texDetails.width, texDetails.height, 0, 0, texDetails.width, texDetails.height,
        GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, eGL_NEAREST);

    gl.glBindFramebuffer(eGL_DRAW_FRAMEBUFFER, curDrawFBO);
    gl.glBindFramebuffer(eGL_READ_FRAMEBUFFER, curReadFBO);

    texname = texDetails.renderbufferReadTex;
    target = eGL_TEXTURE_2D;
  }

  MakeCurrentReplayContext(m_DebugCtx);

  gl.glBindBufferBase(eGL_UNIFORM_BUFFER, 2, DebugData.UBOs[0]);
  HistogramUBOData *cdata =
      (HistogramUBOData *)gl.glMapBufferRange(eGL_UNIFORM_BUFFER, 0, sizeof(HistogramUBOData),
                                              GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);

  cdata->HistogramTextureResolution.x = (float)RDCMAX(details.width >> mip, 1U);
  cdata->HistogramTextureResolution.y = (float)RDCMAX(details.height >> mip, 1U);
  cdata->HistogramTextureResolution.z = (float)RDCMAX(details.depth >> mip, 1U);
  if(texDetails.curType != eGL_TEXTURE_3D)
    cdata->HistogramSlice = (float)sliceFace + 0.001f;
  else
    cdata->HistogramSlice = (float)(sliceFace >> mip);
  cdata->HistogramMip = (int)mip;
  cdata->HistogramNumSamples = texDetails.samples;
  cdata->HistogramSample = (int)RDCCLAMP(sample, 0U, details.msSamp - 1);
  if(sample == ~0U)
    cdata->HistogramSample = -int(details.msSamp);
  cdata->HistogramMin = 0.0f;
  cdata->HistogramMax = 1.0f;
  cdata->HistogramChannels = 0xf;

  int progIdx = texSlot;

  if(details.format.compType == CompType::UInt)
  {
    progIdx |= TEXDISPLAY_UINT_TEX;
    intIdx = 1;
  }
  if(details.format.compType == CompType::SInt)
  {
    progIdx |= TEXDISPLAY_SINT_TEX;
    intIdx = 2;
  }

  int blocksX = (int)ceil(cdata->HistogramTextureResolution.x /
                          float(HGRAM_PIXELS_PER_TILE * HGRAM_TILES_PER_BLOCK));
  int blocksY = (int)ceil(cdata->HistogramTextureResolution.y /
                          float(HGRAM_PIXELS_PER_TILE * HGRAM_TILES_PER_BLOCK));

  gl.glUnmapBuffer(eGL_UNIFORM_BUFFER);

  gl.glActiveTexture((RDCGLenum)(eGL_TEXTURE0 + texSlot));
  gl.glBindTexture(target, texname);
  if(texSlot == RESTYPE_TEXRECT || texSlot == RESTYPE_TEXBUFFER)
    gl.glBindSampler(texSlot, DebugData.pointNoMipSampler);
  else
    gl.glBindSampler(texSlot, DebugData.pointSampler);

  int maxlevel = -1;

  int clampmaxlevel = details.mips - 1;

  gl.glGetTextureParameterivEXT(texname, target, eGL_TEXTURE_MAX_LEVEL, (GLint *)&maxlevel);

  // need to ensure texture is mipmap complete by clamping TEXTURE_MAX_LEVEL.
  if(clampmaxlevel != maxlevel)
  {
    gl.glTextureParameterivEXT(texname, target, eGL_TEXTURE_MAX_LEVEL, (GLint *)&clampmaxlevel);
  }
  else
  {
    maxlevel = -1;
  }

  gl.glBindBufferBase(eGL_SHADER_STORAGE_BUFFER, 0, DebugData.minmaxTileResult);

  gl.glUseProgram(DebugData.minmaxTileProgram[progIdx]);
  gl.glDispatchCompute(blocksX, blocksY, 1);

  gl.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

  gl.glBindBufferBase(eGL_SHADER_STORAGE_BUFFER, 0, DebugData.minmaxResult);
  gl.glBindBufferBase(eGL_SHADER_STORAGE_BUFFER, 1, DebugData.minmaxTileResult);

  gl.glUseProgram(DebugData.minmaxResultProgram[intIdx]);
  gl.glDispatchCompute(1, 1, 1);

  gl.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

  Vec4f minmax[2];
  gl.glBindBuffer(eGL_COPY_READ_BUFFER, DebugData.minmaxResult);
  gl.glGetBufferSubData(eGL_COPY_READ_BUFFER, 0, sizeof(minmax), minmax);

  if(maxlevel >= 0)
    gl.glTextureParameterivEXT(texname, target, eGL_TEXTURE_MAX_LEVEL, (GLint *)&maxlevel);

  minval[0] = minmax[0].x;
  minval[1] = minmax[0].y;
  minval[2] = minmax[0].z;
  minval[3] = minmax[0].w;

  maxval[0] = minmax[1].x;
  maxval[1] = minmax[1].y;
  maxval[2] = minmax[1].z;
  maxval[3] = minmax[1].w;

  return true;
}

bool GLReplay::GetHistogram(ResourceId texid, uint32_t sliceFace, uint32_t mip, uint32_t sample,
                            CompType typeHint, float minval, float maxval, bool channels[4],
                            vector<uint32_t> &histogram)
{
  if(minval >= maxval || texid == ResourceId())
    return false;

  if(m_pDriver->m_Textures.find(texid) == m_pDriver->m_Textures.end())
    return false;

  if(!HasExt[ARB_compute_shader])
    return false;

  auto &texDetails = m_pDriver->m_Textures[texid];

  TextureDescription details = GetTexture(texid);

  const GLHookSet &gl = m_pDriver->GetHookset();

  int texSlot = 0;
  int intIdx = 0;

  bool renderbuffer = false;

  switch(texDetails.curType)
  {
    case eGL_RENDERBUFFER:
      texSlot = RESTYPE_TEX2D;
      renderbuffer = true;
      break;
    case eGL_TEXTURE_1D: texSlot = RESTYPE_TEX1D; break;
    default:
      RDCWARN("Unexpected texture type");
    // fall through
    case eGL_TEXTURE_2D: texSlot = RESTYPE_TEX2D; break;
    case eGL_TEXTURE_2D_MULTISAMPLE: texSlot = RESTYPE_TEX2DMS; break;
    case eGL_TEXTURE_RECTANGLE: texSlot = RESTYPE_TEXRECT; break;
    case eGL_TEXTURE_BUFFER: texSlot = RESTYPE_TEXBUFFER; break;
    case eGL_TEXTURE_3D: texSlot = RESTYPE_TEX3D; break;
    case eGL_TEXTURE_CUBE_MAP: texSlot = RESTYPE_TEXCUBE; break;
    case eGL_TEXTURE_1D_ARRAY: texSlot = RESTYPE_TEX1DARRAY; break;
    case eGL_TEXTURE_2D_ARRAY: texSlot = RESTYPE_TEX2DARRAY; break;
    case eGL_TEXTURE_CUBE_MAP_ARRAY: texSlot = RESTYPE_TEXCUBEARRAY; break;
  }

  GLenum target = texDetails.curType;
  GLuint texname = texDetails.resource.name;

  // do blit from renderbuffer to texture, then sample from texture
  if(renderbuffer)
  {
    // need replay context active to do blit (as FBOs aren't shared)
    MakeCurrentReplayContext(&m_ReplayCtx);

    GLuint curDrawFBO = 0;
    GLuint curReadFBO = 0;
    gl.glGetIntegerv(eGL_DRAW_FRAMEBUFFER_BINDING, (GLint *)&curDrawFBO);
    gl.glGetIntegerv(eGL_READ_FRAMEBUFFER_BINDING, (GLint *)&curReadFBO);

    gl.glBindFramebuffer(eGL_DRAW_FRAMEBUFFER, texDetails.renderbufferFBOs[1]);
    gl.glBindFramebuffer(eGL_READ_FRAMEBUFFER, texDetails.renderbufferFBOs[0]);

    gl.glBlitFramebuffer(
        0, 0, texDetails.width, texDetails.height, 0, 0, texDetails.width, texDetails.height,
        GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, eGL_NEAREST);

    gl.glBindFramebuffer(eGL_DRAW_FRAMEBUFFER, curDrawFBO);
    gl.glBindFramebuffer(eGL_READ_FRAMEBUFFER, curReadFBO);

    texname = texDetails.renderbufferReadTex;
    target = eGL_TEXTURE_2D;
  }

  MakeCurrentReplayContext(m_DebugCtx);

  gl.glBindBufferBase(eGL_UNIFORM_BUFFER, 2, DebugData.UBOs[0]);
  HistogramUBOData *cdata =
      (HistogramUBOData *)gl.glMapBufferRange(eGL_UNIFORM_BUFFER, 0, sizeof(HistogramUBOData),
                                              GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);

  cdata->HistogramTextureResolution.x = (float)RDCMAX(details.width >> mip, 1U);
  cdata->HistogramTextureResolution.y = (float)RDCMAX(details.height >> mip, 1U);
  cdata->HistogramTextureResolution.z = (float)RDCMAX(details.depth >> mip, 1U);
  if(texDetails.curType != eGL_TEXTURE_3D)
    cdata->HistogramSlice = (float)sliceFace + 0.001f;
  else
    cdata->HistogramSlice = (float)(sliceFace >> mip);
  cdata->HistogramMip = mip;
  cdata->HistogramNumSamples = texDetails.samples;
  cdata->HistogramSample = (int)RDCCLAMP(sample, 0U, details.msSamp - 1);
  if(sample == ~0U)
    cdata->HistogramSample = -int(details.msSamp);
  cdata->HistogramMin = minval;

  // The calculation in the shader normalises each value between min and max, then multiplies by the
  // number of buckets.
  // But any value equal to HistogramMax must go into NUM_BUCKETS-1, so add a small delta.
  cdata->HistogramMax = maxval + maxval * 1e-6f;

  cdata->HistogramChannels = 0;
  if(channels[0])
    cdata->HistogramChannels |= 0x1;
  if(channels[1])
    cdata->HistogramChannels |= 0x2;
  if(channels[2])
    cdata->HistogramChannels |= 0x4;
  if(channels[3])
    cdata->HistogramChannels |= 0x8;
  cdata->HistogramFlags = 0;

  int progIdx = texSlot;

  if(details.format.compType == CompType::UInt)
  {
    progIdx |= TEXDISPLAY_UINT_TEX;
    intIdx = 1;
  }
  if(details.format.compType == CompType::SInt)
  {
    progIdx |= TEXDISPLAY_SINT_TEX;
    intIdx = 2;
  }

  int blocksX = (int)ceil(cdata->HistogramTextureResolution.x /
                          float(HGRAM_PIXELS_PER_TILE * HGRAM_TILES_PER_BLOCK));
  int blocksY = (int)ceil(cdata->HistogramTextureResolution.y /
                          float(HGRAM_PIXELS_PER_TILE * HGRAM_TILES_PER_BLOCK));

  gl.glUnmapBuffer(eGL_UNIFORM_BUFFER);

  gl.glActiveTexture((RDCGLenum)(eGL_TEXTURE0 + texSlot));
  gl.glBindTexture(target, texname);
  if(texSlot == RESTYPE_TEXRECT || texSlot == RESTYPE_TEXBUFFER)
    gl.glBindSampler(texSlot, DebugData.pointNoMipSampler);
  else
    gl.glBindSampler(texSlot, DebugData.pointSampler);

  int maxlevel = -1;

  int clampmaxlevel = details.mips - 1;

  gl.glGetTextureParameterivEXT(texname, target, eGL_TEXTURE_MAX_LEVEL, (GLint *)&maxlevel);

  // need to ensure texture is mipmap complete by clamping TEXTURE_MAX_LEVEL.
  if(clampmaxlevel != maxlevel)
  {
    gl.glTextureParameterivEXT(texname, target, eGL_TEXTURE_MAX_LEVEL, (GLint *)&clampmaxlevel);
  }
  else
  {
    maxlevel = -1;
  }

  gl.glBindBufferBase(eGL_SHADER_STORAGE_BUFFER, 0, DebugData.histogramBuf);

  GLuint zero = 0;
  gl.glClearBufferData(eGL_SHADER_STORAGE_BUFFER, eGL_R32UI, eGL_RED_INTEGER, eGL_UNSIGNED_INT,
                       &zero);

  gl.glUseProgram(DebugData.histogramProgram[progIdx]);
  gl.glDispatchCompute(blocksX, blocksY, 1);

  gl.glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

  histogram.clear();
  histogram.resize(HGRAM_NUM_BUCKETS * 4);

  gl.glBindBuffer(eGL_COPY_READ_BUFFER, DebugData.histogramBuf);
  gl.glGetBufferSubData(eGL_COPY_READ_BUFFER, 0, sizeof(uint32_t) * 4 * HGRAM_NUM_BUCKETS,
                        &histogram[0]);

  // compress down from uvec4, then resize down
  for(size_t i = 1; i < HGRAM_NUM_BUCKETS; i++)
    histogram[i] = histogram[i * 4];

  histogram.resize(HGRAM_NUM_BUCKETS);

  if(maxlevel >= 0)
    gl.glTextureParameterivEXT(texname, target, eGL_TEXTURE_MAX_LEVEL, (GLint *)&maxlevel);

  return true;
}

uint32_t GLReplay::PickVertex(uint32_t eventId, int32_t width, int32_t height,
                              const MeshDisplay &cfg, uint32_t x, uint32_t y)
{
  WrappedOpenGL &gl = *m_pDriver;

  if(!HasExt[ARB_compute_shader])
    return ~0U;

  MakeCurrentReplayContext(m_DebugCtx);

  gl.glUseProgram(DebugData.meshPickProgram);

  Matrix4f projMat = Matrix4f::Perspective(90.0f, 0.1f, 100000.0f, float(width) / float(height));

  Matrix4f camMat = cfg.cam ? ((Camera *)cfg.cam)->GetMatrix() : Matrix4f::Identity();
  Matrix4f pickMVP = projMat.Mul(camMat);

  Matrix4f pickMVPProj;
  if(cfg.position.unproject)
  {
    // the derivation of the projection matrix might not be right (hell, it could be an
    // orthographic projection). But it'll be close enough likely.
    Matrix4f guessProj =
        cfg.position.farPlane != FLT_MAX
            ? Matrix4f::Perspective(cfg.fov, cfg.position.nearPlane, cfg.position.farPlane, cfg.aspect)
            : Matrix4f::ReversePerspective(cfg.fov, cfg.position.nearPlane, cfg.aspect);

    if(cfg.ortho)
      guessProj = Matrix4f::Orthographic(cfg.position.nearPlane, cfg.position.farPlane);

    pickMVPProj = projMat.Mul(camMat.Mul(guessProj.Inverse()));
  }

  vec3 rayPos;
  vec3 rayDir;
  // convert mouse pos to world space ray
  {
    Matrix4f inversePickMVP = pickMVP.Inverse();

    float pickX = ((float)x) / ((float)width);
    float pickXCanonical = RDCLERP(-1.0f, 1.0f, pickX);

    float pickY = ((float)y) / ((float)height);
    // flip the Y axis
    float pickYCanonical = RDCLERP(1.0f, -1.0f, pickY);

    vec3 cameraToWorldNearPosition =
        inversePickMVP.Transform(Vec3f(pickXCanonical, pickYCanonical, -1), 1);

    vec3 cameraToWorldFarPosition =
        inversePickMVP.Transform(Vec3f(pickXCanonical, pickYCanonical, 1), 1);

    vec3 testDir = (cameraToWorldFarPosition - cameraToWorldNearPosition);
    testDir.Normalise();

    // Calculate the ray direction first in the regular way (above), so we can use the
    // the output for testing if the ray we are picking is negative or not. This is similar
    // to checking against the forward direction of the camera, but more robust
    if(cfg.position.unproject)
    {
      Matrix4f inversePickMVPGuess = pickMVPProj.Inverse();

      vec3 nearPosProj = inversePickMVPGuess.Transform(Vec3f(pickXCanonical, pickYCanonical, -1), 1);

      vec3 farPosProj = inversePickMVPGuess.Transform(Vec3f(pickXCanonical, pickYCanonical, 1), 1);

      rayDir = (farPosProj - nearPosProj);
      rayDir.Normalise();

      if(testDir.z < 0)
      {
        rayDir = -rayDir;
      }
      rayPos = nearPosProj;
    }
    else
    {
      rayDir = testDir;
      rayPos = cameraToWorldNearPosition;
    }
  }

  gl.glBindBufferBase(eGL_UNIFORM_BUFFER, 0, DebugData.UBOs[0]);
  MeshPickUBOData *cdata =
      (MeshPickUBOData *)gl.glMapBufferRange(eGL_UNIFORM_BUFFER, 0, sizeof(MeshPickUBOData),
                                             GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);

  cdata->rayPos = rayPos;
  cdata->rayDir = rayDir;
  cdata->use_indices = cfg.position.indexByteStride ? 1U : 0U;
  cdata->numVerts = cfg.position.numIndices;
  bool isTriangleMesh = true;
  switch(cfg.position.topology)
  {
    case Topology::TriangleList:
    {
      cdata->meshMode = MESH_TRIANGLE_LIST;
      break;
    }
    case Topology::TriangleStrip:
    {
      cdata->meshMode = MESH_TRIANGLE_STRIP;
      break;
    }
    case Topology::TriangleFan:
    {
      cdata->meshMode = MESH_TRIANGLE_FAN;
      break;
    }
    case Topology::TriangleList_Adj:
    {
      cdata->meshMode = MESH_TRIANGLE_LIST_ADJ;
      break;
    }
    case Topology::TriangleStrip_Adj:
    {
      cdata->meshMode = MESH_TRIANGLE_STRIP_ADJ;
      break;
    }
    default:    // points, lines, patchlists, unknown
    {
      cdata->meshMode = MESH_OTHER;
      isTriangleMesh = false;
    }
  }

  // line/point data
  cdata->unproject = cfg.position.unproject;
  cdata->mvp = cfg.position.unproject ? pickMVPProj : pickMVP;
  cdata->coords = Vec2f((float)x, (float)y);
  cdata->viewport = Vec2f((float)width, (float)height);

  gl.glUnmapBuffer(eGL_UNIFORM_BUFFER);

  GLuint ib = 0;

  uint32_t minIndex = 0;
  uint32_t maxIndex = cfg.position.numIndices;

  uint32_t idxclamp = 0;
  if(cfg.position.baseVertex < 0)
    idxclamp = uint32_t(-cfg.position.baseVertex);

  if(cfg.position.indexByteStride && cfg.position.indexResourceId != ResourceId())
    ib = m_pDriver->GetResourceManager()->GetCurrentResource(cfg.position.indexResourceId).name;

  // We copy into our own buffers to promote to the target type (uint32) that the shader expects.
  // Most IBs will be 16-bit indices, most VBs will not be float4. We also apply baseVertex here

  if(ib)
  {
    // resize up on demand
    if(DebugData.pickIBBuf == 0 || DebugData.pickIBSize < cfg.position.numIndices * sizeof(uint32_t))
    {
      gl.glDeleteBuffers(1, &DebugData.pickIBBuf);

      gl.glGenBuffers(1, &DebugData.pickIBBuf);
      gl.glBindBuffer(eGL_SHADER_STORAGE_BUFFER, DebugData.pickIBBuf);
      gl.glNamedBufferDataEXT(DebugData.pickIBBuf, cfg.position.numIndices * sizeof(uint32_t), NULL,
                              eGL_STREAM_DRAW);

      DebugData.pickIBSize = cfg.position.numIndices * sizeof(uint32_t);
    }

    byte *idxs = new byte[cfg.position.numIndices * cfg.position.indexByteStride];
    memset(idxs, 0, cfg.position.numIndices * cfg.position.indexByteStride);

    std::vector<uint32_t> outidxs;
    outidxs.resize(cfg.position.numIndices);

    gl.glBindBuffer(eGL_COPY_READ_BUFFER, ib);

    GLint bufsize = 0;
    gl.glGetBufferParameteriv(eGL_COPY_READ_BUFFER, eGL_BUFFER_SIZE, &bufsize);

    gl.glGetBufferSubData(eGL_COPY_READ_BUFFER, (GLintptr)cfg.position.indexByteOffset,
                          RDCMIN(uint32_t(bufsize) - uint32_t(cfg.position.indexByteOffset),
                                 cfg.position.numIndices * cfg.position.indexByteStride),
                          idxs);

    uint8_t *idxs8 = (uint8_t *)idxs;
    uint16_t *idxs16 = (uint16_t *)idxs;

    if(cfg.position.indexByteStride == 1)
    {
      for(uint32_t i = 0; i < cfg.position.numIndices; i++)
      {
        uint32_t idx = idxs8[i];

        if(idx < idxclamp)
          idx = 0;
        else if(cfg.position.baseVertex < 0)
          idx -= idxclamp;
        else if(cfg.position.baseVertex > 0)
          idx += cfg.position.baseVertex;

        if(i == 0)
        {
          minIndex = maxIndex = idx;
        }
        else
        {
          minIndex = RDCMIN(idx, minIndex);
          maxIndex = RDCMAX(idx, maxIndex);
        }

        outidxs[i] = idx;
      }

      gl.glBindBuffer(eGL_SHADER_STORAGE_BUFFER, DebugData.pickIBBuf);
      gl.glBufferSubData(eGL_SHADER_STORAGE_BUFFER, 0, cfg.position.numIndices * sizeof(uint32_t),
                         outidxs.data());
    }
    else if(cfg.position.indexByteStride == 2)
    {
      for(uint32_t i = 0; i < cfg.position.numIndices; i++)
      {
        uint32_t idx = idxs16[i];

        if(idx < idxclamp)
          idx = 0;
        else if(cfg.position.baseVertex < 0)
          idx -= idxclamp;
        else if(cfg.position.baseVertex > 0)
          idx += cfg.position.baseVertex;

        if(i == 0)
        {
          minIndex = maxIndex = idx;
        }
        else
        {
          minIndex = RDCMIN(idx, minIndex);
          maxIndex = RDCMAX(idx, maxIndex);
        }

        outidxs[i] = idx;
      }

      gl.glBindBuffer(eGL_SHADER_STORAGE_BUFFER, DebugData.pickIBBuf);
      gl.glBufferSubData(eGL_SHADER_STORAGE_BUFFER, 0, cfg.position.numIndices * sizeof(uint32_t),
                         outidxs.data());
    }
    else
    {
      for(uint32_t i = 0; i < cfg.position.numIndices; i++)
      {
        uint32_t idx = idxs[i];

        if(idx < idxclamp)
          idx = 0;
        else if(cfg.position.baseVertex < 0)
          idx -= idxclamp;
        else if(cfg.position.baseVertex > 0)
          idx += cfg.position.baseVertex;

        if(i == 0)
        {
          minIndex = maxIndex = idx;
        }
        else
        {
          minIndex = RDCMIN(idx, minIndex);
          maxIndex = RDCMAX(idx, maxIndex);
        }

        outidxs[i] = idx;
      }

      gl.glBindBuffer(eGL_SHADER_STORAGE_BUFFER, DebugData.pickIBBuf);
      gl.glBufferSubData(eGL_SHADER_STORAGE_BUFFER, 0, cfg.position.numIndices * sizeof(uint32_t),
                         outidxs.data());
    }
  }

  // unpack and linearise the data
  {
    bytebuf oldData;
    GetBufferData(cfg.position.vertexResourceId, cfg.position.vertexByteOffset, 0, oldData);

    // clamp maxIndex to upper bound in case we got invalid indices or primitive restart indices
    maxIndex = RDCMIN(maxIndex, uint32_t(oldData.size() / cfg.position.vertexByteStride));

    if(DebugData.pickVBBuf == 0 || DebugData.pickVBSize < (maxIndex + 1) * sizeof(Vec4f))
    {
      gl.glDeleteBuffers(1, &DebugData.pickVBBuf);

      gl.glGenBuffers(1, &DebugData.pickVBBuf);
      gl.glBindBuffer(eGL_SHADER_STORAGE_BUFFER, DebugData.pickVBBuf);
      gl.glNamedBufferDataEXT(DebugData.pickVBBuf, (maxIndex + 1) * sizeof(Vec4f), NULL,
                              eGL_DYNAMIC_DRAW);

      DebugData.pickVBSize = (maxIndex + 1) * sizeof(Vec4f);
    }

    std::vector<FloatVector> vbData;
    vbData.resize(maxIndex + 1);

    byte *data = &oldData[0];
    byte *dataEnd = data + oldData.size();

    bool valid;

    // the index buffer may refer to vertices past the start of the vertex buffer, so we can't just
    // conver the first N vertices we'll need.
    // Instead we grab min and max above, and convert every vertex in that range. This might
    // slightly over-estimate but not as bad as 0-max or the whole buffer.
    for(uint32_t idx = minIndex; idx <= maxIndex; idx++)
      vbData[idx] = HighlightCache::InterpretVertex(data, idx, cfg, dataEnd, valid);

    gl.glBindBuffer(eGL_SHADER_STORAGE_BUFFER, DebugData.pickVBBuf);
    gl.glBufferSubData(eGL_SHADER_STORAGE_BUFFER, 0, (maxIndex + 1) * sizeof(Vec4f), vbData.data());
  }

  uint32_t reset[4] = {};
  gl.glBindBufferBase(eGL_SHADER_STORAGE_BUFFER, 0, DebugData.pickResultBuf);
  gl.glBufferSubData(eGL_SHADER_STORAGE_BUFFER, 0, sizeof(uint32_t) * 4, &reset);

  gl.glBindBufferBase(eGL_SHADER_STORAGE_BUFFER, 1, DebugData.pickVBBuf);
  gl.glBindBufferRange(eGL_SHADER_STORAGE_BUFFER, 2, DebugData.pickIBBuf, (GLintptr)0,
                       (GLsizeiptr)(sizeof(uint32_t) * cfg.position.numIndices));
  gl.glBindBufferBase(eGL_SHADER_STORAGE_BUFFER, 3, DebugData.pickResultBuf);

  gl.glDispatchCompute(GLuint((cfg.position.numIndices) / 128 + 1), 1, 1);
  gl.glMemoryBarrier(GL_ATOMIC_COUNTER_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

  uint32_t numResults = 0;

  gl.glBindBuffer(eGL_COPY_READ_BUFFER, DebugData.pickResultBuf);
  gl.glGetBufferSubData(eGL_COPY_READ_BUFFER, 0, sizeof(uint32_t), &numResults);

  if(numResults > 0)
  {
    if(isTriangleMesh)
    {
      struct PickResult
      {
        uint32_t vertid;
        vec3 intersectionPoint;
      };

      byte *mapped = (byte *)gl.glMapNamedBufferEXT(DebugData.pickResultBuf, eGL_READ_ONLY);

      mapped += sizeof(uint32_t) * 4;

      PickResult *pickResults = (PickResult *)mapped;

      PickResult *closest = pickResults;
      // distance from raycast hit to nearest worldspace position of the mouse
      float closestPickDistance = (closest->intersectionPoint - rayPos).Length();

      // min with size of results buffer to protect against overflows
      for(uint32_t i = 1; i < RDCMIN((uint32_t)DebugRenderData::maxMeshPicks, numResults); i++)
      {
        float pickDistance = (pickResults[i].intersectionPoint - rayPos).Length();
        if(pickDistance < closestPickDistance)
        {
          closest = pickResults + i;
        }
      }

      gl.glUnmapNamedBufferEXT(DebugData.pickResultBuf);

      return closest->vertid;
    }
    else
    {
      struct PickResult
      {
        uint32_t vertid;
        uint32_t idx;
        float len;
        float depth;
      };

      byte *mapped = (byte *)gl.glMapNamedBufferEXT(DebugData.pickResultBuf, eGL_READ_ONLY);

      mapped += sizeof(uint32_t) * 4;

      PickResult *pickResults = (PickResult *)mapped;

      PickResult *closest = pickResults;

      // min with size of results buffer to protect against overflows
      for(uint32_t i = 1; i < RDCMIN((uint32_t)DebugRenderData::maxMeshPicks, numResults); i++)
      {
        // We need to keep the picking order consistent in the face
        // of random buffer appends, when multiple vertices have the
        // identical position (e.g. if UVs or normals are different).
        //
        // We could do something to try and disambiguate, but it's
        // never going to be intuitive, it's just going to flicker
        // confusingly.
        if(pickResults[i].len < closest->len ||
           (pickResults[i].len == closest->len && pickResults[i].depth < closest->depth) ||
           (pickResults[i].len == closest->len && pickResults[i].depth == closest->depth &&
            pickResults[i].vertid < closest->vertid))
          closest = pickResults + i;
      }

      gl.glUnmapNamedBufferEXT(DebugData.pickResultBuf);

      return closest->vertid;
    }
  }

  return ~0U;
}

void GLReplay::PickPixel(ResourceId texture, uint32_t x, uint32_t y, uint32_t sliceFace,
                         uint32_t mip, uint32_t sample, CompType typeHint, float pixel[4])
{
  WrappedOpenGL &gl = *m_pDriver;

  MakeCurrentReplayContext(m_DebugCtx);

  gl.glBindFramebuffer(eGL_FRAMEBUFFER, DebugData.pickPixelFBO);
  gl.glBindFramebuffer(eGL_READ_FRAMEBUFFER, DebugData.pickPixelFBO);

  pixel[0] = pixel[1] = pixel[2] = pixel[3] = 0.0f;
  gl.glClearBufferfv(eGL_COLOR, 0, pixel);

  DebugData.outWidth = DebugData.outHeight = 1.0f;
  gl.glViewport(0, 0, 1, 1);

  TextureDisplay texDisplay;

  texDisplay.red = texDisplay.green = texDisplay.blue = texDisplay.alpha = true;
  texDisplay.flipY = false;
  texDisplay.hdrMultiplier = -1.0f;
  texDisplay.linearDisplayAsGamma = true;
  texDisplay.mip = mip;
  texDisplay.sampleIdx = sample;
  texDisplay.customShaderId = ResourceId();
  texDisplay.sliceFace = sliceFace;
  texDisplay.rangeMin = 0.0f;
  texDisplay.rangeMax = 1.0f;
  texDisplay.scale = 1.0f;
  texDisplay.resourceId = texture;
  texDisplay.typeHint = typeHint;
  texDisplay.rawOutput = true;
  texDisplay.xOffset = -float(x);
  texDisplay.yOffset = -float(y);

  RenderTextureInternal(texDisplay, eTexDisplay_MipShift);

  gl.glReadPixels(0, 0, 1, 1, eGL_RGBA, eGL_FLOAT, (void *)pixel);

  if(!HasExt[ARB_gpu_shader5])
  {
    auto &texDetails = m_pDriver->m_Textures[texDisplay.resourceId];

    if(IsSIntFormat(texDetails.internalFormat))
    {
      int32_t casted[4] = {
          (int32_t)pixel[0], (int32_t)pixel[1], (int32_t)pixel[2], (int32_t)pixel[3],
      };

      memcpy(pixel, casted, sizeof(casted));
    }
    else if(IsUIntFormat(texDetails.internalFormat))
    {
      uint32_t casted[4] = {
          (uint32_t)pixel[0], (uint32_t)pixel[1], (uint32_t)pixel[2], (uint32_t)pixel[3],
      };

      memcpy(pixel, casted, sizeof(casted));
    }
  }

  {
    auto &texDetails = m_pDriver->m_Textures[texture];

    // need to read stencil separately as GL can't read both depth and stencil
    // at the same time.
    if(texDetails.internalFormat == eGL_DEPTH24_STENCIL8 ||
       texDetails.internalFormat == eGL_DEPTH32F_STENCIL8 ||
       texDetails.internalFormat == eGL_STENCIL_INDEX8)
    {
      texDisplay.red = texDisplay.blue = texDisplay.alpha = false;

      RenderTextureInternal(texDisplay, eTexDisplay_MipShift);

      uint32_t stencilpixel[4];
      gl.glReadPixels(0, 0, 1, 1, eGL_RGBA, eGL_FLOAT, (void *)stencilpixel);

      if(!HasExt[ARB_gpu_shader5])
      {
        // bits weren't aliased, so re-cast back to uint.
        float fpix[4];
        memcpy(fpix, stencilpixel, sizeof(fpix));

        stencilpixel[0] = (uint32_t)fpix[0];
        stencilpixel[1] = (uint32_t)fpix[1];
      }

      // not sure whether [0] or [1] will return stencil values, so use
      // max of two because other channel should be 0
      pixel[1] = float(RDCMAX(stencilpixel[0], stencilpixel[1])) / 255.0f;

      // the first depth read will have read stencil instead.
      // NULL it out so the UI sees only stencil
      if(texDetails.internalFormat == eGL_STENCIL_INDEX8)
      {
        pixel[1] = float(RDCMAX(stencilpixel[0], stencilpixel[1])) / 255.0f;
        pixel[0] = 0.0f;
      }
    }
  }
}

void GLReplay::RenderCheckerboard()
{
  MakeCurrentReplayContext(m_DebugCtx);

  WrappedOpenGL &gl = *m_pDriver;

  gl.glUseProgram(DebugData.checkerProg);

  gl.glDisable(eGL_DEPTH_TEST);

  gl.glEnable(eGL_FRAMEBUFFER_SRGB);

  gl.glBindBufferBase(eGL_UNIFORM_BUFFER, 0, DebugData.UBOs[0]);

  Vec4f *ubo = (Vec4f *)gl.glMapBufferRange(eGL_UNIFORM_BUFFER, 0, sizeof(Vec4f) * 2,
                                            GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);

  ubo[0] = RenderDoc::Inst().LightCheckerboardColor();
  ubo[1] = RenderDoc::Inst().DarkCheckerboardColor();

  gl.glUnmapBuffer(eGL_UNIFORM_BUFFER);

  gl.glBindVertexArray(DebugData.emptyVAO);
  gl.glDrawArrays(eGL_TRIANGLE_STRIP, 0, 4);
}

void GLReplay::RenderHighlightBox(float w, float h, float scale)
{
  MakeCurrentReplayContext(m_DebugCtx);

  WrappedOpenGL &gl = *m_pDriver;

  GLint sz = GLint(scale);

  struct rect
  {
    GLint x, y;
    GLint w, h;
  };

  rect tl = {GLint(w / 2.0f + 0.5f), GLint(h / 2.0f + 0.5f), 1, 1};

  rect scissors[4] = {
      {tl.x, tl.y - (GLint)sz - 1, 1, sz + 1},
      {tl.x + (GLint)sz, tl.y - (GLint)sz - 1, 1, sz + 2},
      {tl.x, tl.y, sz, 1},
      {tl.x, tl.y - (GLint)sz - 1, sz, 1},
  };

  // inner
  gl.glEnable(eGL_SCISSOR_TEST);
  gl.glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
  for(size_t i = 0; i < ARRAY_COUNT(scissors); i++)
  {
    gl.glScissor(scissors[i].x, scissors[i].y, scissors[i].w, scissors[i].h);
    gl.glClear(eGL_COLOR_BUFFER_BIT);
  }

  scissors[0].x--;
  scissors[1].x++;
  scissors[2].x--;
  scissors[3].x--;

  scissors[0].y--;
  scissors[1].y--;
  scissors[2].y++;
  scissors[3].y--;

  scissors[0].h += 2;
  scissors[1].h += 2;
  scissors[2].w += 2;
  scissors[3].w += 2;

  // outer
  gl.glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  for(size_t i = 0; i < ARRAY_COUNT(scissors); i++)
  {
    gl.glScissor(scissors[i].x, scissors[i].y, scissors[i].w, scissors[i].h);
    gl.glClear(eGL_COLOR_BUFFER_BIT);
  }

  gl.glDisable(eGL_SCISSOR_TEST);
}
