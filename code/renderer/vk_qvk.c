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
static void APIENTRY VKstub_Pointer( GLint a, GLenum b, GLsizei c, const GLvoid *d ) {}
static void APIENTRY VKstub_Cap( GLenum a ) {}
static void APIENTRY VKstub_PolygonOffset( GLfloat a, GLfloat b ) {}
static void APIENTRY VKstub_PolygonMode( GLenum a, GLenum b ) {}
static void APIENTRY VKstub_DepthRange( GLclampd a, GLclampd b ) {}
static void APIENTRY VKstub_Color3f( GLfloat a, GLfloat b, GLfloat c ) {}
static void APIENTRY VKstub_Color4ubv( const GLubyte *v ) {}
static void APIENTRY VKstub_Begin( GLenum a ) {}
static void APIENTRY VKstub_End( void ) {}
static void APIENTRY VKstub_Vertex3fv( const GLfloat *v ) {}
static void APIENTRY VKstub_ArrayElement( GLint i ) {}

void VK_InstallInertGLProcs( void ) {
	qglEnableClientState  = VKstub_ClientState;
	qglDisableClientState = VKstub_ClientState;
	qglVertexPointer      = VKstub_Pointer;
	qglColorPointer       = VKstub_Pointer;
	qglTexCoordPointer    = VKstub_Pointer;
	qglEnable             = VKstub_Cap;
	qglDisable            = VKstub_Cap;
	qglPolygonOffset      = VKstub_PolygonOffset;
	qglPolygonMode        = VKstub_PolygonMode;
	qglDepthRange         = VKstub_DepthRange;
	qglColor3f            = VKstub_Color3f;
	qglColor4ubv          = VKstub_Color4ubv;
	qglBegin              = VKstub_Begin;
	qglEnd                = VKstub_End;
	qglVertex3fv          = VKstub_Vertex3fv;
	qglArrayElement       = VKstub_ArrayElement;
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
