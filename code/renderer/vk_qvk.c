/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Foobar; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
//
// vk_qvk.c -- definition and runtime resolution of the qvk* entry points.
//
#include "tr_local.h"
#include "qvk.h"
#include "vk_local.h"		// for vk.draw (client-array capture under Vulkan)

// The bootstrap pointer is owned here; the platform layer (win_vk.c) fills it
// in from vulkan-1.dll before any of the loaders below run.
PFN_vkGetInstanceProcAddr	qvkGetInstanceProcAddr = NULL;

// Define the storage for every entry point: PFN_vkXxx qvkXxx = NULL;
#define QVK_DEFINE_FUNCTION(name)	PFN_##name q##name = NULL;
QVK_GLOBAL_FUNCTION_LIST( QVK_DEFINE_FUNCTION )
QVK_INSTANCE_FUNCTION_LIST( QVK_DEFINE_FUNCTION )
QVK_DEVICE_FUNCTION_LIST( QVK_DEFINE_FUNCTION )
#undef QVK_DEFINE_FUNCTION

/*
===============
QVK_InitGlobalFunctions

Resolve the small set of entry points that exist before an instance does.
===============
*/
qboolean QVK_InitGlobalFunctions( void ) {
	qboolean ok = qtrue;

	if ( !qvkGetInstanceProcAddr ) {
		ri.Printf( PRINT_ALL, "QVK_InitGlobalFunctions: vkGetInstanceProcAddr is NULL\n" );
		return qfalse;
	}

#define QVK_LOAD_GLOBAL(name)                                                   \
	q##name = (PFN_##name) qvkGetInstanceProcAddr( VK_NULL_HANDLE, #name );     \
	if ( !q##name ) { ri.Printf( PRINT_ALL, "QVK: missing global %s\n", #name ); ok = qfalse; }
	QVK_GLOBAL_FUNCTION_LIST( QVK_LOAD_GLOBAL )
#undef QVK_LOAD_GLOBAL

	return ok;
}

/*
===============
QVK_InitInstanceFunctions
===============
*/
qboolean QVK_InitInstanceFunctions( VkInstance instance ) {
	qboolean ok = qtrue;

#define QVK_LOAD_INSTANCE(name)                                                 \
	q##name = (PFN_##name) qvkGetInstanceProcAddr( instance, #name );           \
	if ( !q##name ) { ri.Printf( PRINT_ALL, "QVK: missing instance %s\n", #name ); ok = qfalse; }
	QVK_INSTANCE_FUNCTION_LIST( QVK_LOAD_INSTANCE )
#undef QVK_LOAD_INSTANCE

	return ok;
}

/*
===============
QVK_InitDeviceFunctions

Device-level entry points are resolved with vkGetDeviceProcAddr (itself an
instance function loaded above) to skip the loader's dispatch trampoline.
===============
*/
qboolean QVK_InitDeviceFunctions( VkDevice device ) {
	qboolean ok = qtrue;

	if ( !qvkGetDeviceProcAddr ) {
		ri.Printf( PRINT_ALL, "QVK_InitDeviceFunctions: vkGetDeviceProcAddr is NULL\n" );
		return qfalse;
	}

#define QVK_LOAD_DEVICE(name)                                                   \
	q##name = (PFN_##name) qvkGetDeviceProcAddr( device, #name );               \
	if ( !q##name ) { ri.Printf( PRINT_ALL, "QVK: missing device %s\n", #name ); ok = qfalse; }
	QVK_DEVICE_FUNCTION_LIST( QVK_LOAD_DEVICE )
#undef QVK_LOAD_DEVICE

	return ok;
}

//
// Inert OpenGL stubs.
//
// The shared tr_shade.c stage iterators still issue GL client-array and a few
// immediate-mode calls (qglVertexPointer, qglEnableClientState, qglEnable, ...).
// Under Vulkan the geometry is read straight from tess and these calls carry no
// meaning -- but the qgl* pointers would be NULL (QGL_Init never ran), so we
// point exactly that set at no-ops.  Real opengl32 entry points overwrite these
// again via QGL_Init if the backend switches back to GL on a vid_restart.
//
static void APIENTRY VKstub_ClientState( GLenum a ) {}
static void APIENTRY VKstub_Cap( GLenum a ) {}

// Client-array "pointers": instead of being inert, these capture the source the
// GL path would draw from, so VK_DrawElements can read exactly that (this is what
// makes the dlight pass, fog pass and vertex-lit path render correctly).
static void APIENTRY VKcap_VertexPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *ptr ) {
	vk.draw.xyzPtr = ptr;
	vk.draw.xyzStride = stride ? stride : 3 * (int)sizeof( float );
}
static void APIENTRY VKcap_ColorPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *ptr ) {
	vk.draw.colorPtr = ptr;
	vk.draw.colorStride = stride ? stride : 4;
}
static void APIENTRY VKcap_TexCoordPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *ptr ) {
	int tmu = glState.currenttmu & 1;
	vk.draw.tcPtr[tmu] = ptr;
	vk.draw.tcStride[tmu] = stride ? stride : 2 * (int)sizeof( float );
}
static void APIENTRY VKstub_PolygonOffset( GLfloat a, GLfloat b ) {}
static void APIENTRY VKstub_PolygonMode( GLenum a, GLenum b ) {}

// qglDepthRange maps to the viewport's min/max depth (used for RF_DEPTHHACK view
// models -> 0..0.3, and the sky -> 1..1).  Re-emit the current viewport.
static void APIENTRY VKcap_DepthRange( GLclampd zNear, GLclampd zFar ) {
	vk.draw.viewport.minDepth = (float)zNear;
	vk.draw.viewport.maxDepth = (float)zFar;
	if ( vk.frameStarted && vk.cmd ) {
		qvkCmdSetViewport( vk.cmd, 0, 1, &vk.draw.viewport );
	}
}
static void APIENTRY VKstub_Color3f( GLfloat a, GLfloat b, GLfloat c ) {}
static void APIENTRY VKstub_Color4f( GLfloat a, GLfloat b, GLfloat c, GLfloat d ) {}
static void APIENTRY VKstub_Color4ubv( const GLubyte *v ) {}
static void APIENTRY VKstub_Begin( GLenum a ) {}
static void APIENTRY VKstub_Void( void ) {}
static void APIENTRY VKstub_Floatv( const GLfloat *v ) {}
static void APIENTRY VKstub_2f( GLfloat a, GLfloat b ) {}
static void APIENTRY VKstub_ArrayElement( GLint i ) {}
static void APIENTRY VKstub_Ortho( GLdouble a, GLdouble b, GLdouble c, GLdouble d, GLdouble e, GLdouble f ) {}
static void APIENTRY VKstub_MultiTexCoord2f( GLenum t, GLfloat s, GLfloat v ) {}

void VK_InstallInertGLProcs( void ) {
	// vertex arrays + draw-state (stage iterators)
	qglEnableClientState  = VKstub_ClientState;
	qglDisableClientState = VKstub_ClientState;
	qglVertexPointer      = VKcap_VertexPointer;
	qglColorPointer       = VKcap_ColorPointer;
	qglTexCoordPointer    = VKcap_TexCoordPointer;
	qglEnable             = VKstub_Cap;
	qglDisable            = VKstub_Cap;
	qglPolygonOffset      = VKstub_PolygonOffset;
	qglPolygonMode        = VKstub_PolygonMode;
	qglDepthRange         = VKcap_DepthRange;
	// immediate mode + matrix stack (sky box, debug draws)
	qglColor3f            = VKstub_Color3f;
	qglColor4f            = VKstub_Color4f;
	qglColor4ubv          = VKstub_Color4ubv;
	qglBegin              = VKstub_Begin;
	qglEnd                = VKstub_Void;
	qglVertex3fv          = VKstub_Floatv;
	qglVertex2f           = VKstub_2f;
	qglTexCoord2fv        = VKstub_Floatv;
	qglTexCoord2f         = VKstub_2f;
	qglMultiTexCoord2fARB = VKstub_MultiTexCoord2f;
	qglArrayElement       = VKstub_ArrayElement;
	qglMatrixMode         = VKstub_Cap;
	qglLoadIdentity       = VKstub_Void;
	qglLoadMatrixf        = VKstub_Floatv;
	qglPushMatrix         = VKstub_Void;
	qglPopMatrix          = VKstub_Void;
	qglTranslatef         = VKstub_Color3f;
	qglOrtho              = VKstub_Ortho;
}

/*
===============
QVK_ClearProcAddresses

Null every pointer on shutdown so a stale entry point can never be called
across a vid_restart that switches back to OpenGL.
===============
*/
void QVK_ClearProcAddresses( void ) {
#define QVK_CLEAR_FUNCTION(name)	q##name = NULL;
	QVK_GLOBAL_FUNCTION_LIST( QVK_CLEAR_FUNCTION )
	QVK_INSTANCE_FUNCTION_LIST( QVK_CLEAR_FUNCTION )
	QVK_DEVICE_FUNCTION_LIST( QVK_CLEAR_FUNCTION )
#undef QVK_CLEAR_FUNCTION
	qvkGetInstanceProcAddr = NULL;
}
