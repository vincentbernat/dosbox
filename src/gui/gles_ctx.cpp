#include "dosbox.h"
#if C_GLES_RPI
#include "gles_ctx.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <bcm_host.h>

#define	SHOW_ERROR	// gles_show_error();

static const char* vertex_shader_custom =
	"uniform mat4 MVPMatrix;"
	"uniform mediump vec2 OutputSize;"
	"uniform mediump vec2 TextureSize;"
	"uniform mediump vec2 InputSize;"

	"attribute vec4 VertexCoord;"
	"attribute vec4 TexCoord;"
	"varying vec4 TEX0;"
	"varying vec4 TEX2;"
	"varying     vec2 _omega;"

	"struct sine_coord {"
	"    vec2 _omega;"
	"};"

	"vec4 _oPosition1;"
	"vec4 _r0006;"
	 
	"void main()"
	"{"
	"    vec2 _oTex;"
	"    sine_coord _coords;"
	"    _r0006 = VertexCoord.x*MVPMatrix[0];"
	"    _r0006 = _r0006 + VertexCoord.y*MVPMatrix[1];"
	"    _r0006 = _r0006 + VertexCoord.z*MVPMatrix[2];"
	"    _r0006 = _r0006 + VertexCoord.w*MVPMatrix[3];"
	"    _oPosition1 = _r0006;"
	"    _oTex = TexCoord.xy;"
	"    _coords._omega = vec2((3.14150000E+00*OutputSize.x*TextureSize.x)/InputSize.x, 6.28299999E+00*TextureSize.y);"
	"    gl_Position = _r0006;"
	"    TEX0.xy = TexCoord.xy;"
	"    TEX2.xy = _coords._omega;"
	"}";

static const char* fragment_shader_custom_scanlines_16bit =
	"precision mediump float;"

	"uniform mediump vec2 OutputSize;"
	"uniform mediump vec2 TextureSize;"
	"uniform mediump vec2 InputSize;"
	"uniform sampler2D Texture;"

	"varying     vec2 _omega;"
	"varying vec4 TEX2;"
	"varying vec4 TEX0;"

	"struct sine_coord {"
	"    vec2 _omega;"
	"};"
	"vec4 _ret_0;"
	"float _TMP2;"
	"vec2 _TMP1;"
	"float _TMP4;"
	"float _TMP3;"
	"vec4 _TMP0;"
	"vec2 _x0009;"
	"vec2 _a0015;"

	"void main()"
	"{"
	"    vec3 _scanline;"
	"    _TMP0 = texture2D(Texture, TEX0.xy);"
	"    _x0009 = TEX0.xy*TEX2.xy;"
	"    _TMP3 = sin(_x0009.x);"
	"    _TMP4 = sin(_x0009.y);"
	"    _TMP1 = vec2(_TMP3, _TMP4);"
	"    _a0015 = vec2( 5.00000007E-02, 1.50000006E-01)*_TMP1;"
	"    _TMP2 = dot(_a0015, vec2( 1.00000000E+00, 1.00000000E+00));"
	"    _scanline = _TMP0.xyz*(9.49999988E-01 + _TMP2);"
	"    _ret_0 = vec4(_scanline.x, _scanline.y, _scanline.z, 1.00000000E+00);"
	"    gl_FragColor = _ret_0;"
	"    return;"
	"}"
;

static const GLfloat vertices[] =
{
	-0.5f, -0.5f, 0.0f,
	+0.5f, -0.5f, 0.0f,
	+0.5f, +0.5f, 0.0f,
	-0.5f, +0.5f, 0.0f,
};

static GLfloat uvs[8];

static const GLushort indices[] =
{
	0, 1, 2,
	0, 2, 3,
};

typedef struct {
	EGLDisplay egl_display;
	EGLConfig  egl_config;
	EGLContext egl_context;
	EGLSurface egl_surface;

	unsigned display_width;
	unsigned display_height;

	unsigned texture_width;
	unsigned texture_height;

	float ratio_x;
	float ratio_y;
	unsigned output_width;
	unsigned output_height;

	void *pixels;

#if C_GLES_RPI
	DISPMANX_ELEMENT_HANDLE_T dispman_element;
	DISPMANX_ELEMENT_HANDLE_T dispman_element_bg;
	DISPMANX_DISPLAY_HANDLE_T dispman_display;
	DISPMANX_UPDATE_HANDLE_T dispman_update;
#endif
} dispvars_t;

dispvars_t dispvars;
dispvars_t* _dispvars = &dispvars;

static const unsigned kVertexCount = 4;
static const unsigned kIndexCount = 6;

void gles_show_error()
{
	GLenum error = GL_NO_ERROR;
	error = glGetError();
	if (GL_NO_ERROR != error) {
		printf("GL Error %x encountered!\n", error);
		exit(0);
	}
}

static GLuint CreateShader(GLenum type, const char *shader_src)
{
	GLuint shader = glCreateShader(type);
	if(!shader)
		return 0;

	// Load and compile the shader source
	glShaderSource(shader, 1, &shader_src, NULL);
	glCompileShader(shader);

	// Check the compile status
	GLint compiled = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if(!compiled)
	{
		GLint info_len = 0;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &info_len);
		if(info_len > 1)
		{
			char* info_log = (char *)malloc(sizeof(char) * info_len);
			glGetShaderInfoLog(shader, info_len, NULL, info_log);
			// TODO(dspringer): We could really use a logging API.
			printf("Error compiling shader:\n%s\n", info_log);
			free(info_log);
		}
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

// Function to load both vertex and fragment shaders, and create the program
static GLuint CreateProgram(const char *vertex_shader_src, const char *fragment_shader_src)
{
	GLuint vertex_shader = CreateShader(GL_VERTEX_SHADER, vertex_shader_src);
	if(!vertex_shader)
		return 0;
	GLuint fragment_shader = CreateShader(GL_FRAGMENT_SHADER, fragment_shader_src);
	if(!fragment_shader)
	{
		glDeleteShader(vertex_shader);
		return 0;
	}

	GLuint program_object = glCreateProgram();
	if(!program_object)
		return 0;
	glAttachShader(program_object, vertex_shader);
	glAttachShader(program_object, fragment_shader);

	// Link the program
	glLinkProgram(program_object);

	// Check the link status
	GLint linked = 0;
	glGetProgramiv(program_object, GL_LINK_STATUS, &linked);
	if(!linked)
	{
		GLint info_len = 0;
		glGetProgramiv(program_object, GL_INFO_LOG_LENGTH, &info_len);
		if(info_len > 1)
		{
			char* info_log = (char *)malloc(info_len);
			glGetProgramInfoLog(program_object, info_len, NULL, info_log);
			// TODO(dspringer): We could really use a logging API.
			printf("Error linking program:\n%s\n", info_log);
			free(info_log);
		}
		glDeleteProgram(program_object);
		return 0;
	}
	// Delete these here because they are attached to the program object.
	glDeleteShader(vertex_shader);
	glDeleteShader(fragment_shader);
	return program_object;
}

static void SetOrtho(float m[4][4], float left, float right, float bottom, float top, float near, float far, float scale_x, float scale_y)
{
	memset(m, 0, 4*4*sizeof(float));
	m[0][0] = 2.0f/(right - left)*scale_x;
	m[1][1] = 2.0f/(top - bottom)*scale_y;
	m[2][2] = -2.0f/(far - near);
	m[3][0] = -(right + left)/(right - left);
	m[3][1] = -(top + bottom)/(top - bottom);
	m[3][2] = -(far + near)/(far - near);
	m[3][3] = 1;
}

typedef struct ShaderInfo
{
   GLuint program;
   GLint u_vp_matrix;
   GLint u_texture;
   GLint a_position; // vertex_coord;
   GLint a_texcoord; //	tex_coord;
   GLint a_color;    // color
   
   GLint lut_tex_coord;

   GLint input_size;
   GLint output_size;
   GLint texture_size;
} ShaderInfo;

static ShaderInfo shader;
static GLuint buffers[3];
static GLuint texture;

static float proj[4][4];

void gles2_init_shaders () {
	memset(&shader, 0, sizeof(ShaderInfo));

	// Load custom shaders
   	float input_size[2], output_size[2], texture_size[2];
	
	shader.program = CreateProgram(vertex_shader_custom, fragment_shader_custom_scanlines_16bit);

	if(shader.program)
	{
		shader.u_vp_matrix   = glGetUniformLocation(shader.program, "MVPMatrix");
  	 	shader.a_texcoord    = glGetAttribLocation(shader.program, "TexCoord");
		shader.a_position    = glGetAttribLocation(shader.program, "VertexCoord");
		
		shader.input_size    = glGetUniformLocation(shader.program, "InputSize");
		shader.output_size   = glGetUniformLocation(shader.program, "OutputSize");
		shader.texture_size  = glGetUniformLocation(shader.program, "TextureSize");
	}

	input_size [0]  = _dispvars->texture_width;
   	input_size [1]  = _dispvars->texture_height;
	output_size[0]  = _dispvars->output_width;
	output_size[1]  = _dispvars->output_height;
	texture_size[0] = _dispvars->texture_width;
	texture_size[1] = _dispvars->texture_height;

	if(!shader.program) 
		exit(0);
	
	glUseProgram(shader.program); SHOW_ERROR

	glUniform2fv(shader.input_size, 1, input_size);
	glUniform2fv(shader.output_size, 1, output_size);
	glUniform2fv(shader.texture_size, 1, texture_size);
}

void gles2_init_texture () {
	//create bitmap texture
	glGenTextures(1, &texture);	SHOW_ERROR

	glBindTexture(GL_TEXTURE_2D, texture); SHOW_ERROR

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR); SHOW_ERROR
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); SHOW_ERROR

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); SHOW_ERROR
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); SHOW_ERROR

	//glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, _dispvars->texture_width, _dispvars->texture_height, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL); SHOW_ERROR
	
	// For 32 bpp textures
	glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, _dispvars->texture_width, _dispvars->texture_height, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, NULL); SHOW_ERROR

	glActiveTexture(GL_TEXTURE0); SHOW_ERROR
}

void gles2_init_geometry () {
	// Setup texture coordinates
	float min_u=0;
	float max_u=1.0f;
	float min_v=0;
	float max_v=1.0f;

	uvs[0] = min_u;
	uvs[1] = min_v;
	uvs[2] = max_u;
	uvs[3] = min_v;
	uvs[4] = max_u;
	uvs[5] = max_v;
	uvs[6] = min_u;
	uvs[7] = max_v;
	// 

	glGenBuffers(3, buffers); SHOW_ERROR
	glBindBuffer(GL_ARRAY_BUFFER, buffers[0]); SHOW_ERROR
	glBufferData(GL_ARRAY_BUFFER, kVertexCount * sizeof(GLfloat) * 3, vertices, GL_STATIC_DRAW); SHOW_ERROR
	glBindBuffer(GL_ARRAY_BUFFER, buffers[1]); SHOW_ERROR
	glBufferData(GL_ARRAY_BUFFER, kVertexCount * sizeof(GLfloat) * 2, uvs, GL_STATIC_DRAW); SHOW_ERROR
	glBindBuffer(GL_ARRAY_BUFFER, 0); SHOW_ERROR
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[2]); SHOW_ERROR
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, kIndexCount * sizeof(GL_UNSIGNED_SHORT), indices, GL_STATIC_DRAW); SHOW_ERROR
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0); SHOW_ERROR

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f); SHOW_ERROR
	
	glDisable(GL_DEPTH_TEST); SHOW_ERROR
	glDisable(GL_DITHER); SHOW_ERROR
	glDisable(GL_BLEND); SHOW_ERROR

	SetOrtho(proj, -0.5f, +0.5f, +0.5f, -0.5f, -1.0f, 1.0f, _dispvars->ratio_x, _dispvars->ratio_y);

	// We activate the position and texture coord attribs for the vertcices
	glBindBuffer(GL_ARRAY_BUFFER, buffers[0]); SHOW_ERROR
	glVertexAttribPointer(shader.a_position, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(GLfloat), NULL); SHOW_ERROR
	glEnableVertexAttribArray(shader.a_position); SHOW_ERROR

	glBindBuffer(GL_ARRAY_BUFFER, buffers[1]); SHOW_ERROR
	glVertexAttribPointer(shader.a_texcoord, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), NULL); SHOW_ERROR
	glEnableVertexAttribArray(shader.a_texcoord); SHOW_ERROR

	// Viewport is configured to the size of the screen. Scaling is done by GLES instead of the 2D API.
	// This is for shaders dependant on the final size, like the scanlines one.
	glViewport(0, 0, _dispvars->display_width, _dispvars->display_height); SHOW_ERROR

	// We upload the projection matrix
	glUniformMatrix4fv(shader.u_vp_matrix, 1, GL_FALSE, &proj[0][0]); SHOW_ERROR

	// We leave the element buffer bound for the glDrawElements() call in gles2_draw so we don¡t have to
	// do it every frame.
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[2]); SHOW_ERROR
}

void gles2_init_dimensions (unsigned texture_width, unsigned texture_height, bool maintain_aspect_ratio) {
	// _dispvars->display_width and _dispvars->display_height must be provided by the
	// egl init function, where native display dimensions are detected.
	_dispvars->texture_width = (float)texture_width;
	_dispvars->texture_height = (float)texture_height;

	// In case we apply aspect ratio correction, we have to pass the aspect-corrected texture dimensions
	// to the scanlines shader so it compensates internally for the correction and scanlines look right.  
	_dispvars->ratio_x = 1.0f;
	_dispvars->ratio_y = 1.0f;
	_dispvars->output_width = _dispvars->display_width;
	_dispvars->output_height = _dispvars->display_height;
        
	if (maintain_aspect_ratio) {
		float display_ratio = (float)_dispvars->display_height/(float)_dispvars->display_width;
		float texture_ratio = (float)_dispvars->texture_height/(float)_dispvars->texture_width;

		if(texture_ratio > display_ratio) 
			_dispvars->ratio_x = display_ratio/texture_ratio;
		else
			_dispvars->ratio_y = texture_ratio/display_ratio;
		
		_dispvars->output_width  = _dispvars->display_width  * _dispvars->ratio_x;
		_dispvars->output_height = _dispvars->display_height * _dispvars->ratio_y;
	}
}

void gles2_init(int texture_width, int texture_height, int depth, bool maintain_aspect_ratio)
{
	gles2_init_dimensions(texture_width, texture_height, maintain_aspect_ratio);
	gles2_init_shaders();
	gles2_init_texture();
	gles2_init_geometry();
}

void gles2_destroy()
{
	if(!shader.program) return;
	if(shader.program) glDeleteProgram(shader.program);
	glDeleteBuffers(3, buffers); SHOW_ERROR
	glDeleteTextures(1, &texture); SHOW_ERROR
}

void pi_init_egl (int src_width, int src_height)
{
	uint32_t display_width, display_height;
	static EGL_DISPMANX_WINDOW_T nativewindow;
	VC_RECT_T dst_rect;
	VC_RECT_T src_rect;

	bcm_host_init();

	int32_t success = graphics_get_display_size(0, &display_width, &display_height);
	assert(success >= 0);

	_dispvars->display_width = display_width;
	_dispvars->display_height = display_height;

	// get an EGL display connection
	_dispvars->egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	assert(_dispvars->egl_display != EGL_NO_DISPLAY);

	// initialize the EGL display connection
	EGLBoolean result = eglInitialize(_dispvars->egl_display, NULL, NULL);
	assert(EGL_FALSE != result);

	// get an appropriate EGL frame buffer configuration
	EGLint num_config;
	static const EGLint attribute_list[] =
	{
	    EGL_RED_SIZE, 8,
	    EGL_GREEN_SIZE, 8,
	    EGL_BLUE_SIZE, 8,
	    EGL_ALPHA_SIZE, 8,
	    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
	    EGL_NONE
	};
	result = eglChooseConfig(_dispvars->egl_display, attribute_list, &_dispvars->egl_config, 1, &num_config);
	assert(EGL_FALSE != result);

	result = eglBindAPI(EGL_OPENGL_ES_API);
	assert(EGL_FALSE != result);

	// create an EGL rendering context
	static const EGLint context_attributes[] =
	{
	    EGL_CONTEXT_CLIENT_VERSION, 2,
	    EGL_NONE
	};
	_dispvars->egl_context = eglCreateContext(_dispvars->egl_display, _dispvars->egl_config, EGL_NO_CONTEXT, context_attributes);
	assert(_dispvars->egl_context != EGL_NO_CONTEXT);

	// Setup source and destination rects
	vc_dispmanx_rect_set( &dst_rect, 0, 0, display_width, display_height);
	vc_dispmanx_rect_set( &src_rect, 0, 0, _dispvars->display_width << 16, _dispvars->display_height << 16);

	_dispvars->dispman_display = vc_dispmanx_display_open(0);
	_dispvars->dispman_update = vc_dispmanx_update_start(0);
	_dispvars->dispman_element = vc_dispmanx_element_add(_dispvars->dispman_update, _dispvars->dispman_display,
					      10, &dst_rect, 0, &src_rect,
					      DISPMANX_PROTECTION_NONE, NULL, NULL, DISPMANX_NO_ROTATE);

	vc_dispmanx_update_submit_sync(_dispvars->dispman_update);

	// Dispmanx ready, we start with egl.
	nativewindow.element = _dispvars->dispman_element;
	nativewindow.width   = _dispvars->display_width;
	nativewindow.height  = _dispvars->display_height;

	_dispvars->egl_surface = eglCreateWindowSurface(_dispvars->egl_display, _dispvars->egl_config, &nativewindow, NULL);
	assert(_dispvars->egl_surface != EGL_NO_SURFACE);

	// connect the context to the surface
	result = eglMakeCurrent(_dispvars->egl_display, _dispvars->egl_surface, _dispvars->egl_surface, _dispvars->egl_context);
	assert(EGL_FALSE != result);

	// vsync always active
	eglSwapInterval (_dispvars->egl_display, 1);
}

void pi_gles_init(int bitmap_width, int bitmap_height, int depth, bool maintain_aspect_ratio) {
	pi_init_egl(bitmap_width, bitmap_height);
	gles2_init(bitmap_width, bitmap_height, depth, maintain_aspect_ratio);
}

void pi_gles_deinit(void)
{
	// Free GLES2 stuff
	gles2_destroy();

	// Free EGL stuff
	eglMakeCurrent( _dispvars->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT );
	eglDestroySurface( _dispvars->egl_display, _dispvars->egl_surface );
	eglDestroyContext( _dispvars->egl_display, _dispvars->egl_context );
	eglTerminate( _dispvars->egl_display );

	// Free DISPMANX stuff
	_dispvars->dispman_update = vc_dispmanx_update_start( 0 );
	vc_dispmanx_element_remove( _dispvars->dispman_update, _dispvars->dispman_element );
	vc_dispmanx_element_remove( _dispvars->dispman_update, _dispvars->dispman_element_bg );
	vc_dispmanx_update_submit_sync( _dispvars->dispman_update );
	vc_dispmanx_display_close( _dispvars->dispman_display );

	bcm_host_deinit();
}

void pi_gles_videoflip()
{
	//Draw to the screen
	eglSwapBuffers(_dispvars->egl_display, _dispvars->egl_surface);
}

#endif //C_GLES_RPI
